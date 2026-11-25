#!/usr/bin/env python3
"""Batería experimental formal para Autores-v4 (Propuesta 4: CAS wait-free Fast-Path / Slow-Path):
  1. Ataque adversarial §5.1 (23 lectores continuos vs 1 escritor CAS, 5 rondas × 2 s):
     - Corte A (sistema / sin afinidad)
     - Corte B (escritor fijado en P-core cpu2, 23 lectores en el resto)
     - Corte C (ataque híbrido Arrow Lake: escritor fijado en E-core cpu8, 23 lectores incluyendo los 8 P-cores)
     Algoritmos: Autores (orig.), Autores-v2, Autores-v4, Anderson, Folly.
  2. Sobrecosto en camino rápido (bench_stack, los 6 paneles de Fig. 5 a 1 h y 24 h, 3 rondas × 2 s):
     Algoritmos: Autores, Autores-v2, Autores-v4, Anderson, Folly.
  3. Memoria retenida (bench_basura, 3 rondas a 8 y 24 hilos): incluye Autores-v4.
"""
import argparse
import csv
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "replica/completa/artefacto"
LIBS = ART / "raiz/usr/lib/x86_64-linux-gnu"
JEMALLOC = LIBS / "libjemalloc.so.2"
SHIM = ROOT / "replica/completa/bin/fijar_hilos.so"
BENCH_STACK = ROOT / "replica/completa/bin/bench_stack"
BENCH_ADV = ROOT / "replica/completa/bin/bench_adversarial_v4"
BENCH_BASURA = ART / "atomic_shared_ptr/build/benchmark/bench_basura"
NUMACTL = ["numactl", "-i", "all"] if shutil.which("numactl") else []

NUCLEOS_P = [2, 3, 4, 5, 6, 7, 0, 1]
CLUSTERS_E = [8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]

NOMBRES = {
    "ivo": "Autores",
    "ivo-v2": "Autores-v2",
    "ivo-v4": "Autores-v4",
    "mine": "Anderson",
    "folly": "Folly",
}

PANELES = [(1, 1), (1, 10), (1, 50), (10, 1), (10, 10), (1000000, 50)]
CAMPOS_ADV = ["impl", "corte", "ronda", "lectores", "segundos", "p50_ns", "p99_ns", "p9995_ns",
              "max_ns", "writer_kops", "reader_mops", "cas_completados", "temp_c", "codigo"]
CAMPOS_REND = ["impl", "alg", "n", "u", "hilos", "ronda", "mops", "rss_kb", "segundos_reales", "temp_c", "codigo"]


def temp_max():
    temps = []
    for z in Path("/sys/class/thermal").glob("thermal_zone*/temp"):
        try:
            temps.append(int(z.read_text()) / 1000)
        except (OSError, ValueError):
            pass
    return f"{max(temps):.0f}" if temps else ""


def entorno(jemalloc=True, cpus=None, principal=None, hilos=None):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{ART / 'install/lib/x86_64-linux-gnu'}:{ART / 'install/folly/lib'}:{LIBS}"
    preload = []
    if jemalloc:
        preload.append(str(JEMALLOC))
    if cpus is not None:
        preload.append(str(SHIM))
        env["FIJAR_CPUS"] = ",".join(map(str, cpus))
        env["FIJAR_PRINCIPAL"] = str(principal)
    if preload:
        env["LD_PRELOAD"] = ":".join(preload)
    if hilos is not None:
        env["NUM_THREADS"] = str(hilos)
    return env


def ejecutar(cmd, env, limite=180.0):
    with tempfile.TemporaryFile("w+") as out:
        t0 = time.time()
        p = subprocess.Popen(cmd, env=env, stdout=out, stderr=subprocess.STDOUT)
        timer = threading.Timer(limite, p.kill)
        timer.start()
        _, st_proc, uso = os.wait4(p.pid, 0)
        timer.cancel()
        cod = os.waitstatus_to_exitcode(st_proc)
        out.seek(0)
        return cod, out.read(), uso.ru_maxrss, time.time() - t0


class Registro:
    def __init__(self, ruta, campos, clave):
        self.ruta, self.campos, self.clave = ruta, campos, clave
        self.hechos = set()
        nuevo = not ruta.exists()
        if not nuevo:
            with open(ruta, newline="", encoding="utf-8") as f:
                self.hechos = {self._clave(r) for r in csv.DictReader(f)}
        self.f = open(ruta, "a", newline="", encoding="utf-8")
        self.w = csv.DictWriter(self.f, campos)
        if nuevo:
            self.w.writeheader()

    def _clave(self, fila):
        return tuple(str(fila[c]) for c in self.clave)

    def hecho(self, **fila):
        return self._clave(fila) in self.hechos

    def escribir(self, fila):
        self.w.writerow(fila)
        self.f.flush()
        self.hechos.add(self._clave(fila))


def correr_adversarial(salida, rondas, segundos):
    reg = Registro(salida / "adversarial_v4.csv", CAMPOS_ADV, ["impl", "corte", "ronda"])
    algs = ["ivo", "ivo-v2", "ivo-v4", "mine", "folly"]
    # En fijar_hilos.so, FIJAR_PRINCIPAL=2 fija el hilo main (escritor CAS!) al CPU 2 (P-core),
    # y los 23 hilos lectores reciben el resto de CPUs; análogamente principal=8 fija el escritor a E-core 8!
    cortes = [
        ("escritor-P", [c for c in (NUCLEOS_P + CLUSTERS_E) if c != 2], 2),
        ("escritor-E", [c for c in (NUCLEOS_P + CLUSTERS_E) if c != 8], 8),
    ]
    tareas = []
    for r in range(1, rondas + 1):
        for corte, cpus, princ in cortes:
            for alg in algs:
                tareas.append((alg, corte, cpus, princ, r))
    random.Random(9401).shuffle(tareas)
    print(f"[v4 adversarial] {len(tareas)} ejecuciones ({segundos} s c/u)", flush=True)
    for idx, (alg, corte, cpus, princ, r) in enumerate(tareas, 1):
        impl = NOMBRES[alg]
        if reg.hecho(impl=impl, corte=corte, ronda=r):
            continue
        cmd = [str(BENCH_ADV), alg, "23", str(segundos)]
        cod, txt, _, _ = ejecutar(cmd, entorno(jemalloc=True, cpus=cpus, principal=princ), limite=120.0)
        linea = [l.strip() for l in txt.strip().splitlines() if l.strip()]
        partes = linea[-1].split(",") if linea else []
        if cod != 0 or len(partes) != 10:
            print(f"  FALLO adv {alg} {corte} r={r}: cod={cod} {txt[-200:]}", file=sys.stderr, flush=True)
            continue
        reg.escribir({
            "impl": partes[0], "corte": corte, "ronda": r,
            "lectores": partes[1], "segundos": partes[2],
            "p50_ns": partes[3], "p99_ns": partes[4], "p9995_ns": partes[5], "max_ns": partes[6],
            "writer_kops": partes[7], "reader_mops": partes[8], "cas_completados": partes[9],
            "temp_c": temp_max(), "codigo": cod,
        })
        if idx % 10 == 0 or idx == len(tareas):
            print(f"  [v4 adv] {idx}/{len(tareas)}  {temp_max()} °C", flush=True)


def correr_camino_rapido(salida, rondas, segundos):
    reg = Registro(salida / "rendimiento_v4.csv", CAMPOS_REND, ["impl", "n", "u", "hilos", "ronda"])
    algs = ["ivo", "ivo-v2", "ivo-v4", "mine", "folly"]
    puntos = []
    for (n, u) in PANELES:
        for h in (1, 24):
            for alg in algs:
                puntos.append((alg, n, u, h))
    total = len(puntos) * rondas
    hecho = 0
    t0 = time.time()
    print(f"[v4 camino rápido] {len(puntos)} puntos × {rondas} rondas = {total} ejecuciones ({segundos} s c/u)", flush=True)
    for r in range(1, rondas + 1):
        orden = list(puntos)
        random.Random(9450 * r).shuffle(orden)
        for alg, n, u, h in orden:
            hecho += 1
            impl = NOMBRES[alg]
            if reg.hecho(impl=impl, n=n, u=u, hilos=h, ronda=r):
                continue
            cmd = NUMACTL + [str(BENCH_STACK), f"--threads={h}", f"--alg={alg}",
                             "-r", str(segundos), "-s", str(n), "-u", str(u), "-i", "1", "--peek=true"]
            cod, txt, rss, dur = ejecutar(cmd, entorno(jemalloc=True, hilos=h), limite=180.0)
            vals = [float(x) for x in re.findall(r"Throughput = (\S+) Mop/s", txt)]
            if cod != 0 or not vals:
                print(f"  FALLO rend {alg} n={n} u={u} h={h}: cod={cod}", file=sys.stderr, flush=True)
            reg.escribir({
                "impl": impl, "alg": alg, "n": n, "u": u, "hilos": h, "ronda": r,
                "mops": vals[0] if vals else "", "rss_kb": rss,
                "segundos_reales": f"{dur:.2f}", "temp_c": temp_max(), "codigo": cod,
            })
            if hecho % 30 == 0 or hecho == total:
                print(f"  [v4 rend] {hecho}/{total}  {(time.time()-t0)/60:.1f} min  {temp_max()} °C", flush=True)


def correr_basura(salida, rondas, segundos):
    ruta = salida / "basura_v4.csv"
    with open(ruta, "w", encoding="utf-8") as f:
        for r in range(1, rondas + 1):
            cod, txt, _, _ = ejecutar([str(BENCH_BASURA), str(segundos), "8", "24"],
                                      entorno(jemalloc=False), limite=300.0)
            lineas = txt.strip().splitlines()
            if r == 1 and lineas:
                f.write("ronda," + lineas[0] + "\n")
            for l in lineas[1:]:
                f.write(f"{r},{l}\n")
            f.flush()
            print(f"  [v4 basura] ronda {r}/{rondas} cod={cod}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--salida", default=str(ROOT / "mejoras/resultados_v4"))
    ap.add_argument("--rondas-adv", type=int, default=5)
    ap.add_argument("--rondas-rend", type=int, default=3)
    ap.add_argument("--segundos", type=float, default=2.0)
    ap.add_argument("--rondas-basura", type=int, default=3)
    a = ap.parse_args()
    salida = Path(a.salida)
    salida.mkdir(parents=True, exist_ok=True)
    correr_adversarial(salida, a.rondas_adv, a.segundos)
    correr_camino_rapido(salida, a.rondas_rend, a.segundos)
    correr_basura(salida, a.rondas_basura, 1.5)
    print("EXPERIMENTO V4 TERMINADO", flush=True)


if __name__ == "__main__":
    main()
