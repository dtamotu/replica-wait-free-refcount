#!/usr/bin/env python3
"""Réplica oficial Euro-Par 2024 en Intel Core i5-10400 (12h SMT, Linux nativo).
100% compatible con el esquema CSV de replica/resultados/david-ultra9-285/nativo_oficial/
e incorpora los controles complementarios propuestos por Codex (p=56, pares SMT vs L3,
ablación SMT 6x1 vs 3x2 y 4x1 vs 2x2, Williams con/sin -mcx16, paridad 7x10s e IMC).
"""
import csv, json, os, random, re, shutil, statistics as st, subprocess, sys, tempfile, threading, time
from pathlib import Path

ROOT = Path("/home/david/replica-wait-free-refcount")
ART = ROOT / "replica/completa/artefacto"
LIBS = ART / "raiz/usr/lib/x86_64-linux-gnu"
JEMALLOC = LIBS / "libjemalloc.so.2"
SHIM = ROOT / "replica/completa/bin/fijar_hilos.so"
BENCH_STACK = ROOT / "replica/completa/bin/bench_stack"
BENCH_LAT = ART / "atomic_shared_ptr/build/benchmark/bench_atomic_shared_ptrs"
BENCH_LAT_CX16 = ART / "atomic_shared_ptr/build_cx16/benchmark/bench_atomic_shared_ptrs"
BENCH_BASURA = ART / "atomic_shared_ptr/build/benchmark/bench_basura"
NUMACTL = ["numactl", "-i", "all"] if shutil.which("numactl") else []

OUT = ROOT / "replica/resultados/david-i5-10400/nativo_oficial"
OUT.mkdir(parents=True, exist_ok=True)

PANELES = [(1, 1), (1, 10), (1, 50), (10, 1), (10, 10), (1000000, 50)]
IMPL_OFICIAL = [("ivo", "Autores"), ("gnu", "Libstdc++"), ("jss-free", "Williams"),
                ("folly", "Folly"), ("mine", "Anderson")]
LAT_OFICIAL = [("Our", "Autores"), ("STL", "Libstdc++"), ("JSS-OS", "Williams"),
               ("Folly", "Folly"), ("Parlay", "Anderson")]

CAMPOS_REND = ["experimento", "motor", "impl", "n", "u", "hilos", "colocacion", "familia",
               "ronda", "mops", "rss_kb", "segundos_reales", "carga1", "temp_c", "codigo"]
CAMPOS_LAT = ["experimento", "motor", "impl", "op", "p", "colocacion", "familia", "ronda",
              "p1", "p25", "p50", "p75", "p99", "p9995", "iteraciones", "carga1", "temp_c", "codigo"]


def estado():
    carga = Path("/proc/loadavg").read_text().split()[0]
    temps = []
    for z in Path("/sys/class/thermal").glob("thermal_zone*/temp"):
        try:
            temps.append(int(z.read_text()) / 1000)
        except (OSError, ValueError):
            pass
    return carga, f"{max(temps):.0f}" if temps else ""


def entorno(jemalloc=True, hilos=None, fijar_cpus=None, fijar_principal=None):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = f"{ART / 'install/lib/x86_64-linux-gnu'}:{ART / 'install/folly/lib'}:{LIBS}"
    preloads = []
    if fijar_cpus:
        preloads.append(str(SHIM))
        env["FIJAR_CPUS"] = fijar_cpus
        if fijar_principal is not None:
            env["FIJAR_PRINCIPAL"] = str(fijar_principal)
    if jemalloc and JEMALLOC.exists():
        preloads.append(str(JEMALLOC))
    if preloads:
        env["LD_PRELOAD"] = ":".join(preloads)
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


def run_fig5_and_topology():
    reg = Registro(OUT / "rendimiento.csv", CAMPOS_REND,
                   ["experimento", "impl", "n", "u", "hilos", "colocacion", "ronda"])
    t0 = time.time()

    # 1. Fig 5 oficial: 5 rondas para [1,3,6,9,12], 3 rondas para [18]
    tareas = []
    for (n, u) in PANELES:
        for alg, impl in IMPL_OFICIAL:
            for h in (1, 3, 6, 9, 12):
                for r in range(1, 6):
                    tareas.append(("fig5", alg, impl, n, u, h, "sistema", "sistema", r, 2.0, None, None))
            for r in range(1, 4):
                tareas.append(("fig5", alg, impl, n, u, 18, "sistema", "sistema", r, 2.0, None, None))

    # 2. Pares de nucleos (n=1, u=50%, 10 rondas x 1s, principal=11)
    pares = [
        ("mismo_nucleo_SMT", "SMT", [(0, 6), (1, 7), (2, 8), (3, 9)]),
        ("nucleos_distintos_L3", "L3", [(0, 1), (1, 2), (2, 3), (3, 4)]),
    ]
    for fam_nom, fam_key, lista_pares in pares:
        for (x, y) in lista_pares:
            for alg, impl in [("ivo", "Autores"), ("jss-free", "Williams"), ("mine", "Anderson")]:
                for r in range(1, 11):
                    tareas.append(("pares", alg, impl, 1, 50, 2, f"{x}-{y}", fam_nom, r, 1.0, f"{x},{y}", "11"))

    # 3. Ablacion SMT 6 hilos (6x1 vs 3x2) y 4 hilos (4x1 vs 2x2) (5 rondas x 1s, principal=11)
    algs_smt6 = [("ivo", "Autores"), ("ivo-v2", "Autores-v2"), ("ivo-v3g", "Autores-v3g"),
                 ("ivo-v3", "Autores-v3"), ("mine", "Anderson"), ("mine-v3", "Anderson-v3")]
    for col, fam, cpus in [("6x1_fisicos", "SMT-6x1", "0,1,2,3,4,5"), ("3x2_hermanos", "SMT-3x2", "0,6,1,7,2,8")]:
        for alg, impl in algs_smt6:
            for r in range(1, 6):
                tareas.append(("smt_ablacion", alg, impl, 1, 50, 6, col, fam, r, 1.0, cpus, "11"))

    for col, fam, cpus in [("4x1_fisicos", "SMT-4x1", "0,1,2,3"), ("2x2_hermanos", "SMT-2x2", "0,6,1,7")]:
        for alg, impl in [("ivo-v3g", "Autores-v3g"), ("ivo-v3", "Autores-v3")]:
            for r in range(1, 6):
                tareas.append(("smt_ablacion", alg, impl, 1, 50, 4, col, fam, r, 1.0, cpus, "11"))

    # Aleatorizar por ronda dentro de cada experimento
    random.Random(20261127).shuffle(tareas)
    total = len(tareas)
    print(f"[rendimiento.csv] Total tareas: {total}", flush=True)
    for idx, (exp, alg, impl, n, u, h, col, fam, r, seg, cpus, princ) in enumerate(tareas, 1):
        if reg.hecho(experimento=exp, impl=impl, n=n, u=u, hilos=h, colocacion=col, ronda=r):
            continue
        cmd = NUMACTL + [str(BENCH_STACK), f"--threads={h}", f"--alg={alg}",
                         "-r", str(seg), "-s", str(n), "-u", str(u), "-i", "1", "--peek=true"]
        carga, temp = estado()
        cod, txt, rss, dur = ejecutar(cmd, entorno(jemalloc=True, hilos=h, fijar_cpus=cpus, fijar_principal=princ), limite=180.0)
        vals = [float(x) for x in re.findall(r"Throughput = (\S+) Mop/s", txt)]
        if cod != 0 or not vals:
            print(f"  FALLO {exp} {alg} n={n} u={u} h={h} col={col}: cod={cod}", file=sys.stderr, flush=True)
        reg.escribir({
            "experimento": exp, "motor": "oficial", "impl": impl, "n": n, "u": u, "hilos": h,
            "colocacion": col, "familia": fam, "ronda": r, "mops": vals[0] if vals else "",
            "rss_kb": rss, "segundos_reales": f"{dur:.2f}", "carga1": carga, "temp_c": temp, "codigo": cod,
        })
        if idx % 60 == 0 or idx == total:
            print(f"  [rend] {idx}/{total}  {(time.time()-t0)/60:.1f} min  {temp} C", flush=True)


def run_fig6():
    reg = Registro(OUT / "latencia.csv", CAMPOS_LAT,
                   ["experimento", "impl", "op", "p", "colocacion", "ronda"])
    tareas = []
    # Fig 6 oficial (Williams con build_cx16, resto con build)
    for pref, impl in LAT_OFICIAL:
        bin_p = BENCH_LAT_CX16 if impl == "Williams" else BENCH_LAT
        for p in (1, 4, 8, 12, 56):
            for r in range(1, 4):
                tareas.append(("fig6", bin_p, pref, impl, p, r, 0.4))
    # Control Williams sin -mcx16 (candado lock_pool)
    for p in (1, 12, 56):
        for r in range(1, 4):
            tareas.append(("fig6_sin_cx16", BENCH_LAT, "JSS-OS", "Williams", p, r, 0.4))

    random.Random(20261128).shuffle(tareas)
    print(f"[latencia.csv] Total bloques: {len(tareas)}", flush=True)
    for idx, (exp, bin_p, pref, impl, p, r, mint) in enumerate(tareas, 1):
        if reg.hecho(experimento=exp, impl=impl, op="load", p=p, colocacion="sistema", ronda=r):
            continue
        cmd = [str(bin_p), f"--benchmark_filter=^{pref}::.*/{p}/manual_time",
               "--benchmark_format=json", f"--benchmark_min_time={mint}s"]
        carga, temp = estado()
        cod, txt, _, _ = ejecutar(cmd, entorno(jemalloc=False), limite=180.0)
        try:
            datos = json.loads(txt[txt.index("{"):])
        except ValueError:
            print(f"  FALLO lat {exp} {impl} p={p}: cod={cod}", file=sys.stderr, flush=True)
            continue
        for b in datos.get("benchmarks", []):
            op = b["name"].split("::")[1].split("/")[0]
            reg.escribir({
                "experimento": exp, "motor": "oficial", "impl": impl, "op": op, "p": p,
                "colocacion": "sistema", "familia": "sistema", "ronda": r,
                "p1": f"{b['1%']*1e9:.1f}", "p25": f"{b['25%']*1e9:.1f}", "p50": f"{b['50%']*1e9:.1f}",
                "p75": f"{b['75%']*1e9:.1f}", "p99": f"{b['99%']*1e9:.1f}", "p9995": f"{b['99.95%']*1e9:.1f}",
                "iteraciones": b.get("iterations", ""), "carga1": carga, "temp_c": temp, "codigo": cod,
            })
        if idx % 20 == 0 or idx == len(tareas):
            print(f"  [fig6] {idx}/{len(tareas)}  {temp} C", flush=True)


def run_basura():
    ruta = OUT / "basura.csv"
    print("[basura.csv] 2,4,6,8,12 hilos x 3 rondas (1.5s)", flush=True)
    with open(ruta, "w", encoding="utf-8") as f:
        for r in range(1, 4):
            cmd = [str(BENCH_BASURA), "1.5", "2", "4", "6", "8", "12"]
            cod, txt, _, _ = ejecutar(cmd, entorno(jemalloc=False), limite=300.0)
            lineas = txt.strip().splitlines()
            if r == 1 and lineas:
                f.write("ronda," + lineas[0] + "\n")
            for l in lineas[1:]:
                f.write(f"{r},{l}\n")
            f.flush()
            print(f"  [basura] ronda {r}/3 cod={cod}", flush=True)


def run_paridad():
    ruta = OUT / "paridad.csv"
    reg = Registro(ruta, ["impl", "n", "u", "hilos", "mediana_7x10s", "minimo", "maximo"],
                   ["impl", "n", "u", "hilos"])
    print("[paridad.csv] 4 celdas clave (7x10s a 12h)", flush=True)
    for (alg, impl) in [("ivo", "Autores"), ("mine", "Anderson")]:
        for (n, u) in [(1, 50), (1000000, 50)]:
            if reg.hecho(impl=impl, n=n, u=u, hilos=12):
                continue
            cmd = NUMACTL + [str(BENCH_STACK), "--threads=12", f"--alg={alg}",
                             "-r", "10", "-s", str(n), "-u", str(u), "-i", "7", "--peek=true"]
            cod, txt, _, _ = ejecutar(cmd, entorno(jemalloc=True, hilos=12), limite=240.0)
            vals = [float(x) for x in re.findall(r"Throughput = (\S+) Mop/s", txt)]
            if cod == 0 and vals:
                reg.escribir({
                    "impl": impl, "n": n, "u": u, "hilos": 12,
                    "mediana_7x10s": f"{st.median(vals):.4f}",
                    "minimo": f"{min(vals):.4f}", "maximo": f"{max(vals):.4f}",
                })
                print(f"  [paridad] {impl} n={n} u={u}: med={st.median(vals):.4f} [{min(vals):.4f}..{max(vals):.4f}]", flush=True)


def run_imc():
    ruta = OUT / "memoria_imc.csv"
    print("[memoria_imc.csv] Sondas perf stat uncore_imc (fondo + n=10^6 a 12h)", flush=True)
    campos = ["caso", "ronda", "mops", "data_reads_mib", "data_writes_mib", "gt_requests_mib", "segundos_reales", "gb_s_total"]
    with open(ruta, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, campos)
        w.writeheader()
        ev = "uncore_imc/data_reads/,uncore_imc/data_writes/,uncore_imc/gt_requests/"
        casos = [("fondo_reposo", None)] + [(impl, alg) for alg, impl in [("ivo", "Autores"), ("ivo-v2", "Autores-v2"), ("mine", "Anderson"), ("folly", "Folly")]]
        for caso, alg in casos:
            for r in range(1, 4):
                if alg is None:
                    cmd = ["perf", "stat", "-a", "-x,", "-e", ev, "sleep", "2"]
                    env = os.environ.copy()
                else:
                    cmd = ["perf", "stat", "-a", "-x,", "-e", ev] + NUMACTL + [
                        str(BENCH_STACK), "--threads=12", f"--alg={alg}", "-r", "2", "-s", "1000000", "-u", "50", "-i", "1", "--peek=true"
                    ]
                    env = entorno(jemalloc=True, hilos=12)
                cod, txt, _, dur = ejecutar(cmd, env, limite=120.0)
                mops = re.findall(r"Throughput = (\S+) Mop/s", txt)
                vals = {}
                for line in txt.splitlines():
                    for k in ("data_reads", "data_writes", "gt_requests"):
                        if f"uncore_imc/{k}/" in line:
                            try:
                                vals[k] = float(line.split(",")[0].replace(",", "."))
                            except ValueError:
                                pass
                rd = vals.get("data_reads", 0.0)
                wr = vals.get("data_writes", 0.0)
                gt = vals.get("gt_requests", 0.0)
                gbs = ((rd + wr) * 1.048576 / 1000.0) / dur if dur > 0 else 0.0
                w.writerow({
                    "caso": caso, "ronda": r, "mops": mops[0] if mops else "",
                    "data_reads_mib": f"{rd:.2f}", "data_writes_mib": f"{wr:.2f}",
                    "gt_requests_mib": f"{gt:.2f}", "segundos_reales": f"{dur:.2f}", "gb_s_total": f"{gbs:.2f}",
                })
                f.flush()
    print("  [imc] OK", flush=True)


def main():
    t0 = time.time()
    run_fig5_and_topology()
    run_fig6()
    run_basura()
    run_paridad()
    run_imc()
    print(f"=== REPLICA_OFICIAL_I5_COMPLETA en {(time.time()-t0)/60:.1f} min ===", flush=True)


if __name__ == "__main__":
    main()