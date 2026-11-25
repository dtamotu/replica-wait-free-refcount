#!/usr/bin/env python3
"""Batería experimental de ablación para Autores-v3 (Propuesta 3):
  - Autores (ivo): original del paper
  - Autores-v2 (ivo-v2): base in-place + 8B + CAS limpio
  - Autores-v3a (ivo-v3a): ablación de solo backoff ligero P/E en CAS
  - Autores-v3g (ivo-v3g): ablación de Elimination-Backoff genérico (ciego a topología)
  - Autores-v3  (ivo-v3):  Elimination-Backoff consciente de clústeres L2 y P/E-cores
  - Anderson    (mine):    rival oficial sin modificar
  - Anderson-v3 (mine-v3): control científico justo (Anderson + misma capa L2)
  - Folly       (folly):   rival oficial sin modificar

Metodología idéntica a replica/completa/baterias.py y correr_v1_v2.py:
  - Proceso nuevo por medición, orden aleatorizado por ronda, jemalloc, numactl -i all.
  - CSV reanudable línea a línea, registrando Mop/s, pico RSS (KB), duración y temperatura.
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
BENCH_STACK = ROOT / "replica/completa/bin/bench_stack"
NUMACTL = ["numactl", "-i", "all"] if shutil.which("numactl") else []

NOMBRES = {
    "ivo": "Autores",
    "ivo-v2": "Autores-v2",
    "ivo-v3a": "Autores-v3a",
    "ivo-v3g": "Autores-v3g",
    "ivo-v3": "Autores-v3",
    "mine": "Anderson",
    "mine-v3": "Anderson-v3",
    "folly": "Folly",
}

PANELES = [(1, 1), (1, 10), (1, 50), (10, 1), (10, 10), (1000000, 50)]
CAMPOS_REND = ["impl", "alg", "n", "u", "hilos", "ronda", "mops", "rss_kb", "segundos_reales", "temp_c", "codigo"]


def temp_max():
    temps = []
    for z in Path("/sys/class/thermal").glob("thermal_zone*/temp"):
        try:
            temps.append(int(z.read_text()) / 1000)
        except (OSError, ValueError):
            pass
    return f"{max(temps):.0f}" if temps else ""


def entorno(hilos=None):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{ART / 'install/lib/x86_64-linux-gnu'}:{ART / 'install/folly/lib'}:{LIBS}"
    env["LD_PRELOAD"] = str(JEMALLOC)
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


def correr_rendimiento(salida, rondas, segundos):
    reg = Registro(salida / "rendimiento_v3.csv", CAMPOS_REND, ["impl", "n", "u", "hilos", "ronda"])
    algs_todos = ["ivo", "ivo-v2", "ivo-v3a", "ivo-v3g", "ivo-v3", "mine", "mine-v3", "folly"]
    puntos = []

    # 1. Los 6 paneles de la Figura 5 a 24 hilos (todas las 8 variantes)
    for (n, u) in PANELES:
        for alg in algs_todos:
            puntos.append((alg, n, u, 24))

    # 2. Los 6 paneles a 1 hilo (verificar 0 regresión sin contención)
    for (n, u) in PANELES:
        for alg in ("ivo", "ivo-v2", "ivo-v3a", "ivo-v3", "mine"):
            puntos.append((alg, n, u, 1))

    # 3. Escalado en contención extrema (n=1, u=50% y u=10%) a 6, 12, 18 hilos
    for (n, u) in ((1, 50), (1, 10)):
        for h in (6, 12, 18):
            for alg in ("ivo", "ivo-v2", "ivo-v3a", "ivo-v3g", "ivo-v3", "mine", "mine-v3"):
                puntos.append((alg, n, u, h))

    puntos = list(dict.fromkeys(puntos))
    total = len(puntos) * rondas
    hecho = 0
    t0 = time.time()
    print(f"[v3 rendimiento] {len(puntos)} puntos × {rondas} rondas = {total} ejecuciones ({segundos} s c/u)", flush=True)
    for r in range(1, rondas + 1):
        orden = list(puntos)
        random.Random(8300 * r + len(puntos)).shuffle(orden)
        for alg, n, u, h in orden:
            hecho += 1
            impl = NOMBRES[alg]
            if reg.hecho(impl=impl, n=n, u=u, hilos=h, ronda=r):
                continue
            cmd = NUMACTL + [str(BENCH_STACK), f"--threads={h}", f"--alg={alg}",
                             "-r", str(segundos), "-s", str(n), "-u", str(u), "-i", "1", "--peek=true"]
            cod, txt, rss, dur = ejecutar(cmd, entorno(hilos=h), limite=180.0)
            vals = [float(x) for x in re.findall(r"Throughput = (\S+) Mop/s", txt)]
            if cod != 0 or not vals:
                print(f"  FALLO {alg} n={n} u={u} h={h}: cod={cod} {txt[-250:]}", file=sys.stderr, flush=True)
            reg.escribir({
                "impl": impl, "alg": alg, "n": n, "u": u, "hilos": h, "ronda": r,
                "mops": vals[0] if vals else "", "rss_kb": rss,
                "segundos_reales": f"{dur:.2f}", "temp_c": temp_max(), "codigo": cod,
            })
            if hecho % 40 == 0 or hecho == total:
                print(f"  [v3] {hecho}/{total}  {(time.time()-t0)/60:.1f} min  {temp_max()} °C", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--salida", default=str(ROOT / "mejoras/resultados_v3"))
    ap.add_argument("--rondas", type=int, default=3)
    ap.add_argument("--segundos", type=float, default=2.0)
    a = ap.parse_args()
    salida = Path(a.salida)
    salida.mkdir(parents=True, exist_ok=True)
    correr_rendimiento(salida, a.rondas, a.segundos)
    print("EXPERIMENTO V3 TERMINADO", flush=True)


if __name__ == "__main__":
    main()
