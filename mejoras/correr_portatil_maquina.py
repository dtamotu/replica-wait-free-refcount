#!/usr/bin/env python3
"""Runner portátil y desatendido para CUALQUIER máquina Linux (Core Ultra 9 285 de 24 h,
Core i5-10400 de 12 h, o cualquier otro procesador x86-64).

Detecta automáticamente `nproc` (p. ej. 12 en i5-10400, 24 en Ultra 9 285), nunca fija
CPUs inexistentes, escribe CSVs reanudables línea a línea y genera `resumen_maquina.md`.
"""
import argparse
import csv
import json
import os
import platform
import random
import re
import shutil
import statistics as st
import subprocess
import sys
import tempfile
import threading
import time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "replica/completa/artefacto"
LIBS = ART / "raiz/usr/lib/x86_64-linux-gnu"
JEMALLOC = LIBS / "libjemalloc.so.2"
BENCH_STACK = ROOT / "replica/completa/bin/bench_stack"
BENCH_ADV = ROOT / "replica/completa/bin/bench_adversarial_v4"
BENCH_LAT = ART / "atomic_shared_ptr/build/benchmark/bench_atomic_shared_ptrs"
BENCH_BASURA = ART / "atomic_shared_ptr/build/benchmark/bench_basura"
NUMACTL = ["numactl", "-i", "all"] if shutil.which("numactl") else []

PANELES = [(1, 1), (1, 10), (1, 50), (10, 1), (10, 10), (1000000, 50)]
ALGS_REND = [
    ("ivo", "Autores"),
    ("ivo-v1", "Autores-v1"),
    ("ivo-v1p", "Autores-v1p"),
    ("ivo-v2", "Autores-v2"),
    ("ivo-v2p", "Autores-v2p"),
    ("ivo-v3a", "Autores-v3a"),
    ("ivo-v3g", "Autores-v3g"),
    ("ivo-v3", "Autores-v3"),
    ("ivo-v4", "Autores-v4"),
    ("mine", "Anderson"),
    ("mine-v3", "Anderson-v3"),
    ("folly", "Folly"),
    ("gnu", "Libstdc++"),
]
ALGS_ADV = [("ivo", "Autores"), ("ivo-v2", "Autores-v2"), ("ivo-v4", "Autores-v4"),
            ("mine", "Anderson"), ("folly", "Folly")]
ALGS_LAT = [("Our", "Autores"), ("Our-v1", "Autores-v1"), ("Our-v2", "Autores-v2"),
            ("Our-v4", "Autores-v4"), ("Parlay", "Anderson"), ("Folly", "Folly")]


def temp_max():
    temps = []
    for z in Path("/sys/class/thermal").glob("thermal_zone*/temp"):
        try:
            temps.append(int(z.read_text()) / 1000)
        except (OSError, ValueError):
            pass
    return f"{max(temps):.0f}" if temps else ""


def entorno(jemalloc=True, hilos=None):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{ART / 'install/lib/x86_64-linux-gnu'}:{ART / 'install/folly/lib'}:{LIBS}"
    if jemalloc and JEMALLOC.exists():
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


def correr_fig5(salida, max_h, rondas, segundos):
    campos = ["impl", "alg", "n", "u", "hilos", "ronda", "mops", "rss_kb", "segundos_reales", "temp_c", "codigo"]
    reg = Registro(salida / "rendimiento_portatil.csv", campos, ["impl", "n", "u", "hilos", "ronda"])
    hilos_escalado = sorted({1, max(2, max_h // 4), max(4, max_h // 2), max(6, (3 * max_h) // 4), max_h})
    puntos = []
    # Todos los 6 paneles a 1 hilo y máquina llena (max_h)
    for (n, u) in PANELES:
        for h in (1, max_h):
            for alg, impl in ALGS_REND:
                if alg == "gnu" and n != 1000000:
                    continue
                puntos.append((alg, impl, n, u, h))
    # Escalado en contención extrema (n=1, u=50%) y memoria (n=10^6, u=50%)
    for h in hilos_escalado:
        for alg, impl in ALGS_REND:
            if alg in ("ivo", "ivo-v2", "ivo-v3g", "ivo-v3", "ivo-v4", "mine", "mine-v3", "folly"):
                puntos.append((alg, impl, 1, 50, h))
            if alg in ("ivo", "ivo-v2", "ivo-v3", "ivo-v4", "mine", "gnu"):
                puntos.append((alg, impl, 1000000, 50, h))
    puntos = list(dict.fromkeys(puntos))
    total = len(puntos) * rondas
    hecho = 0
    t0 = time.time()
    print(f"[fig5 portátil] max_h={max_h} | {len(puntos)} puntos × {rondas} rondas = {total} corridas ({segundos}s)", flush=True)
    for r in range(1, rondas + 1):
        orden = list(puntos)
        random.Random(5500 * r + max_h).shuffle(orden)
        for alg, impl, n, u, h in orden:
            hecho += 1
            if reg.hecho(impl=impl, n=n, u=u, hilos=h, ronda=r):
                continue
            cmd = NUMACTL + [str(BENCH_STACK), f"--threads={h}", f"--alg={alg}",
                             "-r", str(segundos), "-s", str(n), "-u", str(u), "-i", "1", "--peek=true"]
            cod, txt, rss, dur = ejecutar(cmd, entorno(jemalloc=True, hilos=h), limite=180.0)
            vals = [float(x) for x in re.findall(r"Throughput = (\S+) Mop/s", txt)]
            if cod != 0 or not vals:
                print(f"  FALLO {alg} n={n} u={u} h={h}: cod={cod}", file=sys.stderr, flush=True)
            reg.escribir({
                "impl": impl, "alg": alg, "n": n, "u": u, "hilos": h, "ronda": r,
                "mops": vals[0] if vals else "", "rss_kb": rss,
                "segundos_reales": f"{dur:.2f}", "temp_c": temp_max(), "codigo": cod,
            })
            if hecho % 40 == 0 or hecho == total:
                print(f"  [fig5] {hecho}/{total}  {(time.time()-t0)/60:.1f} min  {temp_max()} °C", flush=True)


def correr_fig6(salida, max_h, rondas, min_tiempo):
    campos = ["impl", "op", "p", "ronda", "p1", "p25", "p50", "p75", "p99", "p9995", "iteraciones", "temp_c", "codigo"]
    reg = Registro(salida / "latencia_portatil.csv", campos, ["impl", "op", "p", "ronda"])
    tareas = [(pref, impl, p, r) for r in range(1, rondas + 1) for p in (1, max_h) for pref, impl in ALGS_LAT]
    random.Random(6601 + max_h).shuffle(tareas)
    print(f"[fig6 portátil] {len(tareas)} corridas (p=1 y p={max_h}, min_time={min_tiempo}s)", flush=True)
    for idx, (pref, impl, p, r) in enumerate(tareas, 1):
        if reg.hecho(impl=impl, op="load", p=p, ronda=r):
            continue
        cmd = [str(BENCH_LAT), f"--benchmark_filter=^{pref}::.*/{p}/manual_time",
               "--benchmark_format=json", f"--benchmark_min_time={min_tiempo}s"]
        cod, txt, _, _ = ejecutar(cmd, entorno(jemalloc=False), limite=180.0)
        try:
            datos = json.loads(txt[txt.index("{"):])
        except ValueError:
            print(f"  FALLO lat {impl} p={p}: cod={cod}", file=sys.stderr, flush=True)
            continue
        for b in datos.get("benchmarks", []):
            op = b["name"].split("::")[1].split("/")[0]
            reg.escribir({
                "impl": impl, "op": op, "p": p, "ronda": r,
                "p1": f"{b['1%']*1e9:.1f}", "p25": f"{b['25%']*1e9:.1f}", "p50": f"{b['50%']*1e9:.1f}",
                "p75": f"{b['75%']*1e9:.1f}", "p99": f"{b['99%']*1e9:.1f}", "p9995": f"{b['99.95%']*1e9:.1f}",
                "iteraciones": b.get("iterations", ""), "temp_c": temp_max(), "codigo": cod,
            })
        if idx % 10 == 0 or idx == len(tareas):
            print(f"  [fig6] {idx}/{len(tareas)}  {temp_max()} °C", flush=True)


def correr_adversarial(salida, max_h, rondas, segundos):
    lectores = max(1, max_h - 1)
    campos = ["impl", "ronda", "lectores", "segundos", "p50_ns", "p99_ns", "p9995_ns",
              "max_ns", "writer_kops", "reader_mops", "cas_completados", "temp_c", "codigo"]
    reg = Registro(salida / "adversarial_portatil.csv", campos, ["impl", "ronda"])
    tareas = [(alg, impl, r) for r in range(1, rondas + 1) for alg, impl in ALGS_ADV]
    random.Random(7701 + max_h).shuffle(tareas)
    print(f"[adversarial portátil] {lectores} lectores vs 1 escritor CAS | {len(tareas)} corridas ({segundos}s)", flush=True)
    for idx, (alg, impl, r) in enumerate(tareas, 1):
        if reg.hecho(impl=impl, ronda=r):
            continue
        cmd = [str(BENCH_ADV), alg, str(lectores), str(segundos)]
        cod, txt, _, _ = ejecutar(cmd, entorno(jemalloc=True), limite=120.0)
        lineas = [l.strip() for l in txt.strip().splitlines() if l.strip()]
        partes = lineas[-1].split(",") if lineas else []
        if cod != 0 or len(partes) != 10:
            print(f"  FALLO adv {alg} r={r}: cod={cod}", file=sys.stderr, flush=True)
            continue
        reg.escribir({
            "impl": partes[0], "ronda": r, "lectores": partes[1], "segundos": partes[2],
            "p50_ns": partes[3], "p99_ns": partes[4], "p9995_ns": partes[5], "max_ns": partes[6],
            "writer_kops": partes[7], "reader_mops": partes[8], "cas_completados": partes[9],
            "temp_c": temp_max(), "codigo": cod,
        })
    print(f"  [adversarial] {len(tareas)}/{len(tareas)} OK", flush=True)


def correr_basura(salida, max_h, rondas, segundos):
    ruta = salida / "basura_portatil.csv"
    hilos = sorted({4, min(8, max_h), max_h})
    with open(ruta, "w", encoding="utf-8") as f:
        for r in range(1, rondas + 1):
            cmd = [str(BENCH_BASURA), str(segundos)] + [str(h) for h in hilos]
            cod, txt, _, _ = ejecutar(cmd, entorno(jemalloc=False), limite=300.0)
            lineas = txt.strip().splitlines()
            if r == 1 and lineas:
                f.write("ronda," + lineas[0] + "\n")
            for l in lineas[1:]:
                f.write(f"{r},{l}\n")
            f.flush()
            print(f"  [basura] ronda {r}/{rondas} cod={cod}", flush=True)


def resumir(salida, max_h):
    def med(xs):
        return st.median(xs) if xs else float("nan")

    rend = defaultdict(list)
    with open(salida / "rendimiento_portatil.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["codigo"] == "0" and r["mops"]:
                rend[(r["impl"], int(r["n"]), int(r["u"]), int(r["hilos"]))].append(float(r["mops"]))

    algs = ["Autores", "Autores-v1", "Autores-v2", "Autores-v3g", "Autores-v3", "Autores-v4", "Anderson", "Anderson-v3", "Folly"]
    md = [
        f"# Resumen Automático Portátil ({platform.node()}, {max_h} hilos)",
        "",
        f"## Fig. 5 a {max_h} hilos (mediana, Mop/s)",
        "",
        "| Panel | " + " | ".join(algs) + " |",
        "|" + "---|" * (len(algs) + 1),
    ]
    for n, u in PANELES:
        row = [f"n={n}, u={u}%"]
        for a in algs:
            v = med(rend[(a, n, u, max_h)])
            row.append(f"{v:.2f}" if v == v else "—")
        md.append("| " + " | ".join(row) + " |")

    adv_csv = salida / "adversarial_portatil.csv"
    if adv_csv.exists():
        adv = defaultdict(lambda: defaultdict(list))
        with open(adv_csv, newline="", encoding="utf-8") as f:
            for r in csv.DictReader(f):
                for c in ("p50_ns", "p99_ns", "p9995_ns", "max_ns", "writer_kops"):
                    adv[r["impl"]][c].append(float(r[c]))
        md += [
            "",
            f"## Ataque adversarial §5.1 ({max_h - 1} lectores vs 1 escritor CAS, mediana)",
            "",
            "| Algoritmo | p50 (ns) | p99 (ns) | p99.95 (ns) | Máx (ns) | Escritor (Kops/s) |",
            "|---|---:|---:|---:|---:|---:|",
        ]
        for _, impl in ALGS_ADV:
            d = adv[impl]
            if d["p50_ns"]:
                md.append(f"| {impl} | {med(d['p50_ns']):,.0f} | {med(d['p99_ns']):,.0f} | "
                          f"{med(d['p9995_ns']):,.0f} | {med(d['max_ns']):,.0f} | {med(d['writer_kops']):,.1f} |")

    (salida / "resumen_maquina.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print("Resumen generado:", salida / "resumen_maquina.md", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--salida", required=True)
    ap.add_argument("--max-hilos", type=int, default=os.cpu_count() or 12)
    ap.add_argument("--rondas-rend", type=int, default=3)
    ap.add_argument("--rondas-lat", type=int, default=3)
    ap.add_argument("--rondas-adv", type=int, default=5)
    ap.add_argument("--segundos", type=float, default=2.0)
    a = ap.parse_args()
    salida = Path(a.salida)
    salida.mkdir(parents=True, exist_ok=True)
    correr_fig5(salida, a.max_hilos, a.rondas_rend, a.segundos)
    correr_fig6(salida, a.max_hilos, a.rondas_lat, 0.3)
    correr_adversarial(salida, a.max_hilos, a.rondas_adv, a.segundos)
    correr_basura(salida, a.max_hilos, 3, 1.5)
    resumir(salida, a.max_hilos)
    print("BATERIA PORTATIL TERMINADA", flush=True)


if __name__ == "__main__":
    main()
