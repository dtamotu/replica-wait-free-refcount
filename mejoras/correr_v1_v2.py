#!/usr/bin/env python3
"""Batería experimental de ablación: Autores original (ivo) vs Autores-v1 (1A) vs Autores-v2 (1A+1B)
frente a Anderson (mine), Folly (folly) y Libstdc++ (gnu) usando los binarios oficiales del artefacto.

Metodología idéntica a replica/completa/baterias.py:
  - Procesos nuevos por punto, orden aleatorio por ronda, jemalloc vía LD_PRELOAD, numactl -i all.
  - CSV reanudable escrito línea a línea.
  - Mide: rendimiento (Mop/s), pico RSS (KB), latencia (p1..p99.95 en ns, sistema y fijado) y basura.
"""
import argparse
import csv
import json
import os
import random
import re
import shutil
import statistics as st
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
BENCH_STACK = ART / "concurrent_deferred_rc/build/benchmarks/bench_stack"
BENCH_LAT = ART / "atomic_shared_ptr/build/benchmark/bench_atomic_shared_ptrs"
BENCH_BASURA = ART / "atomic_shared_ptr/build/benchmark/bench_basura"
NUMACTL = ["numactl", "-i", "all"] if shutil.which("numactl") else []

NUCLEOS_P = [2, 3, 4, 5, 6, 7, 0, 1]
CLUSTERS_E = [8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23]

NOMBRES = {
    "ivo": "Autores",
    "ivo-v1": "Autores-v1",
    "ivo-v1p": "Autores-v1p",
    "ivo-v2": "Autores-v2",
    "ivo-v2p": "Autores-v2p",
    "mine": "Anderson",
    "folly": "Folly",
    "gnu": "Libstdc++",
}

LAT_PREF = {
    "Autores": "Our",
    "Autores-v1": "Our-v1",
    "Autores-v2": "Our-v2",
    "Anderson": "Parlay",
    "Folly": "Folly",
}

PANELES = [(1, 1), (1, 10), (1, 50), (10, 1), (10, 10), (1000000, 50)]
CAMPOS_REND = ["impl", "alg", "n", "u", "hilos", "ronda", "mops", "rss_kb", "segundos_reales", "temp_c", "codigo"]
CAMPOS_LAT = ["impl", "op", "p", "colocacion", "ronda", "p1", "p25", "p50", "p75", "p99", "p9995", "iteraciones", "temp_c", "codigo"]


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
    env["LD_LIBRARY_PATH"] = f"{LIBS}:{ART / 'install/folly/lib'}"
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


def ejecutar(cmd, env, limite=120.0):
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


def correr_rendimiento(salida, rondas, segundos):
    reg = Registro(salida / "rendimiento_v1_v2.csv", CAMPOS_REND, ["impl", "n", "u", "hilos", "ronda"])
    algs_todos = ["ivo", "ivo-v1", "ivo-v1p", "ivo-v2", "ivo-v2p", "mine", "folly"]
    puntos = []
    # Todos los 6 paneles a 1 hilo y 24 hilos (máquina llena)
    for (n, u) in PANELES:
        for h in (1, 24):
            for alg in algs_todos:
                puntos.append((alg, n, u, h))
    # Barrido intermedio clave en n=10^6 (donde Autores perdía a 1-18 hilos) y n=1,u=50% a 12h
    for h in (6, 12, 18):
        for alg in ("ivo", "ivo-v1", "ivo-v2", "mine", "gnu"):
            puntos.append((alg, 1000000, 50, h))
    for h in (1, 24):
        puntos.append(("gnu", 1000000, 50, h))
    for h in (6, 12):
        for alg in ("ivo", "ivo-v1", "ivo-v1p", "ivo-v2", "ivo-v2p", "mine", "folly"):
            puntos.append((alg, 1, 50, h))

    # Deduplicar manteniendo orden
    puntos = list(dict.fromkeys(puntos))
    total = len(puntos) * rondas
    hecho = 0
    t0 = time.time()
    print(f"[rendimiento] {len(puntos)} puntos × {rondas} rondas = {total} ejecuciones ({segundos} s c/u)", flush=True)
    for r in range(1, rondas + 1):
        orden = list(puntos)
        random.Random(7000 * r + len(puntos)).shuffle(orden)
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
                print(f"  FALLO {alg} n={n} u={u} h={h}: cod={cod} {txt[-250:]}", file=sys.stderr, flush=True)
            reg.escribir({
                "impl": impl, "alg": alg, "n": n, "u": u, "hilos": h, "ronda": r,
                "mops": vals[0] if vals else "", "rss_kb": rss,
                "segundos_reales": f"{dur:.2f}", "temp_c": temp_max(), "codigo": cod,
            })
            if hecho % 40 == 0 or hecho == total:
                print(f"  [rendimiento] {hecho}/{total}  {(time.time()-t0)/60:.1f} min  {temp_max()} °C", flush=True)


def correr_latencia(salida, rondas_p1, rondas_p24, min_tiempo):
    reg = Registro(salida / "latencia_v1_v2.csv", CAMPOS_LAT, ["impl", "op", "p", "colocacion", "ronda"])
    impls = ["Autores", "Autores-v1", "Autores-v2", "Anderson", "Folly"]
    tareas = []
    for r in range(1, rondas_p1 + 1):
        for impl in impls:
            tareas.append((impl, 1, "P-fijo", [2], 2, r))
    for r in range(1, rondas_p24 + 1):
        for impl in impls:
            # Fijado: hilo medido en E-core 8 o P-core 2 para evitar bimodalidad aleatoria
            tareas.append((impl, 24, "todos24-P", NUCLEOS_P + CLUSTERS_E, "lista", r))
            tareas.append((impl, 24, "todos24-E", CLUSTERS_E + NUCLEOS_P, "lista", r))
    random.Random(9001).shuffle(tareas)
    print(f"[latencia] {len(tareas)} ejecuciones (min_time={min_tiempo}s)", flush=True)
    for idx, (impl, p, col, cpus, princ, r) in enumerate(tareas, 1):
        if reg.hecho(impl=impl, op="load", p=p, colocacion=col, ronda=r):
            continue
        pref = LAT_PREF[impl]
        cmd = [str(BENCH_LAT), f"--benchmark_filter=^{pref}::.*/{p}/manual_time",
               "--benchmark_format=json", f"--benchmark_min_time={min_tiempo}s"]
        cod, txt, _, _ = ejecutar(cmd, entorno(jemalloc=False, cpus=cpus, principal=princ), limite=180.0)
        try:
            datos = json.loads(txt[txt.index("{"):])
        except ValueError:
            print(f"  FALLO latencia {impl} p={p} {col}: cod={cod} {txt[-250:]}", file=sys.stderr, flush=True)
            continue
        for b in datos.get("benchmarks", []):
            op = b["name"].split("::")[1].split("/")[0]
            reg.escribir({
                "impl": impl, "op": op, "p": p, "colocacion": col, "ronda": r,
                "p1": f"{b['1%']*1e9:.1f}", "p25": f"{b['25%']*1e9:.1f}", "p50": f"{b['50%']*1e9:.1f}",
                "p75": f"{b['75%']*1e9:.1f}", "p99": f"{b['99%']*1e9:.1f}", "p9995": f"{b['99.95%']*1e9:.1f}",
                "iteraciones": b.get("iterations", ""), "temp_c": temp_max(), "codigo": cod,
            })
        if idx % 15 == 0 or idx == len(tareas):
            print(f"  [latencia] {idx}/{len(tareas)}  {temp_max()} °C", flush=True)


def correr_basura(salida, rondas, segundos):
    ruta = salida / "basura_v1_v2.csv"
    with open(ruta, "w", encoding="utf-8") as f:
        for r in range(1, rondas + 1):
            cod, txt, _, _ = ejecutar([str(BENCH_BASURA), str(segundos), "8", "24"], entorno(jemalloc=False), limite=300.0)
            lineas = txt.strip().splitlines()
            if r == 1 and lineas:
                f.write("ronda," + lineas[0] + "\n")
            for l in lineas[1:]:
                f.write(f"{r},{l}\n")
            f.flush()
            print(f"  [basura] ronda {r}/{rondas} cod={cod}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--salida", default=str(ROOT / "mejoras/resultados_v1_v2"))
    ap.add_argument("--rondas-rend", type=int, default=3)
    ap.add_argument("--segundos", type=float, default=2.0)
    ap.add_argument("--rondas-lat-p1", type=int, default=3)
    ap.add_argument("--rondas-lat-p24", type=int, default=5)
    ap.add_argument("--min-tiempo-lat", type=float, default=0.3)
    ap.add_argument("--rondas-basura", type=int, default=3)
    ap.add_argument("--solo", default="rendimiento,latencia,basura")
    a = ap.parse_args()
    salida = Path(a.salida)
    salida.mkdir(parents=True, exist_ok=True)
    solo = set(a.solo.split(","))
    if "rendimiento" in solo:
        correr_rendimiento(salida, a.rondas_rend, a.segundos)
    if "latencia" in solo:
        correr_latencia(salida, a.rondas_lat_p1, a.rondas_lat_p24, a.min_tiempo_lat)
    if "basura" in solo:
        correr_basura(salida, a.rondas_basura, 1.5)
    print("EXPERIMENTO V1/V2 TERMINADO", flush=True)


if __name__ == "__main__":
    main()
