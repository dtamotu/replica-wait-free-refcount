#!/usr/bin/env python3
"""Analiza los resultados de ablación Autores vs Autores-v1 (1A) vs Autores-v2 (1A+1B)."""
import csv
import statistics as st
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

AQUI = Path(__file__).resolve().parent
RES = AQUI / "resultados_v1_v2"
FIG = RES / "figuras"
FIG.mkdir(exist_ok=True)

PANELES = [("1", "1"), ("1", "10"), ("1", "50"), ("10", "1"), ("10", "10"), ("1000000", "50")]
COL = {
    "Autores": "#c8232c",
    "Autores-v1": "#e06c1f",
    "Autores-v1p": "#d99b66",
    "Autores-v2": "#0b7a75",
    "Autores-v2p": "#59a9a5",
    "Anderson": "#8b5cc4",
    "Folly": "#e39b1e",
    "Libstdc++": "#1f6fb4",
}
MARCA = {
    "Autores": "o", "Autores-v1": "^", "Autores-v1p": "v",
    "Autores-v2": "D", "Autores-v2p": "d",
    "Anderson": "*", "Folly": "P", "Libstdc++": "s",
}


def leer(ruta):
    with open(ruta, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def vals(filas, campo="mops", **filtro):
    return [float(r[campo]) for r in filas if all(r[k] == str(v) for k, v in filtro.items()) and r[campo]]


def med(filas, campo="mops", **filtro):
    v = vals(filas, campo, **filtro)
    return st.median(v) if v else None


def titulo_panel(n, u):
    return f"n = {'10⁶' if n == '1000000' else n}, u = {u} %"


def main():
    rend = leer(RES / "rendimiento_v1_v2.csv")
    lat = leer(RES / "latencia_v1_v2.csv")
    bas = leer(RES / "basura_v1_v2.csv")

    fallos_r = [r for r in rend if r["codigo"] != "0" or not r["mops"]]
    fallos_l = [r for r in lat if r["codigo"] != "0" or not r["p50"]]

    out = ["# Resultados de Ablación: Autores (original) vs Autores-v1 (1A) vs Autores-v2 (1A+1B)\n"]
    out.append(f"- Ejecuciones rendimiento: **{len(rend)}** (fallidas: {len(fallos_r)}).")
    out.append(f"- Filas latencia: **{len(lat)}** (fallidas: {len(fallos_l)}).")
    out.append(f"- Filas basura: **{len(bas)}**.\n")

    # Tabla 1: 6 paneles a 24 hilos y a 1 hilo
    impls7 = ["Autores", "Autores-v1", "Autores-v1p", "Autores-v2", "Autores-v2p", "Anderson", "Folly"]
    for h in (24, 1):
        out.append(f"## Rendimiento en los 6 paneles de la Figura 5 ({h} hilo{'s' if h > 1 else ''}, mediana de 3 rondas × 2 s, Mop/s)\n")
        out.append("Entre paréntesis, mejora respecto a `Autores` original (1,00× = igual).\n")
        out.append("| Panel | " + " | ".join(impls7) + " |")
        out.append("|---|" + "---|" * len(impls7))
        for n, u in PANELES:
            base = med(rend, impl="Autores", n=n, u=u, hilos=h)
            celdas = []
            for i in impls7:
                m = med(rend, impl=i, n=n, u=u, hilos=h)
                if m is None:
                    celdas.append("—")
                else:
                    celdas.append(f"**{m:.2f}** ({m/base:.2f}×)" if i == "Autores-v2" else f"{m:.2f} ({m/base:.2f}×)")
            out.append(f"| {titulo_panel(n, u)} | " + " | ".join(celdas) + " |")
        out.append("")

    # Tabla 2: Escalado en n = 10^6, u = 50 % + RSS
    impls_1m = ["Autores", "Autores-v1", "Autores-v2", "Anderson", "Libstdc++"]
    out.append("## Escalado en n = 10⁶, u = 50 % (Mop/s) y pico RSS a 24 hilos (MB)\n")
    out.append("| Hilos | " + " | ".join(impls_1m) + " |")
    out.append("|---|" + "---|" * len(impls_1m))
    for h in (1, 6, 12, 18, 24):
        base = med(rend, impl="Autores", n="1000000", u="50", hilos=h)
        celdas = []
        for i in impls_1m:
            m = med(rend, impl=i, n="1000000", u="50", hilos=h)
            celdas.append(f"{m:.2f} ({m/base:.2f}×)" if m else "—")
        out.append(f"| {h} | " + " | ".join(celdas) + " |")
    rss_celdas = []
    for i in impls_1m:
        r_kb = med(rend, "rss_kb", impl=i, n="1000000", u="50", hilos=24)
        rss_celdas.append(f"{r_kb/1024:,.0f} MB" if r_kb else "—")
    out.append("| **RSS (24 h)** | " + " | ".join(rss_celdas) + " |\n")

    # Tabla 3: Escalado en n = 1, u = 50 %
    out.append("## Escalado en contención extrema: n = 1, u = 50 % (Mop/s)\n")
    out.append("| Hilos | " + " | ".join(impls7) + " |")
    out.append("|---|" + "---|" * len(impls7))
    for h in (1, 6, 12, 24):
        base = med(rend, impl="Autores", n="1", u="50", hilos=h)
        celdas = [f"{med(rend, impl=i, n='1', u='50', hilos=h):.2f} ({med(rend, impl=i, n='1', u='50', hilos=h)/base:.2f}×)" for i in impls7]
        out.append(f"| {h} | " + " | ".join(celdas) + " |")
    out.append("")

    # Tabla 4: Latencia (p = 1, todos24-P y todos24-E)
    impls_lat = ["Autores", "Autores-v1", "Autores-v2", "Anderson", "Folly"]
    out.append("## Latencia controlada (ns): p50 / p99 / p99,95\n")
    out.append("| Operación | Corte | " + " | ".join(impls_lat) + " |")
    out.append("|---|---|" + "---|" * len(impls_lat))
    for op in ("load", "store", "store-del"):
        for p, col in ((1, "P-fijo"), (24, "todos24-P"), (24, "todos24-E")):
            celdas = []
            for i in impls_lat:
                q = [med(lat, c, impl=i, op=op, p=p, colocacion=col) for c in ("p50", "p99", "p9995")]
                celdas.append(" / ".join(f"{v:,.0f}" for v in q) if all(v is not None for v in q) else "—")
            out.append(f"| {op} | p={p} ({col}) | " + " | ".join(celdas) + " |")
    out.append("")

    # Variabilidad entre rondas con colocación fija P vs E
    out.append("## Estabilidad de latencia con colocación fija (p = 24, 5 rondas: razón máx/mín de p50)\n")
    out.append("| Colocación | " + " | ".join(impls_lat) + " |")
    out.append("|---|" + "---|" * len(impls_lat))
    for col in ("todos24-P", "todos24-E"):
        celdas = []
        for i in impls_lat:
            v = vals(lat, "p50", impl=i, op="load", p=24, colocacion=col)
            celdas.append(f"{max(v)/min(v):.2f}× ({min(v):.0f}–{max(v):.0f} ns)" if v else "—")
        out.append(f"| load {col} | " + " | ".join(celdas) + " |")
    out.append("")

    # Tabla 5: Memoria retenida (bench_basura)
    impls_bas = ["Autores", "Autores-v1", "Autores-v2", "Anderson", "Folly", "Williams", "Libstdc++"]
    out.append("## Memoria retenida (bench_basura, mediana de 3 rondas: pico de bloques vivos)\n")
    out.append("| Hilos | Límite | " + " | ".join(impls_bas) + " |")
    out.append("|---|---|" + "---|" * len(impls_bas))
    for h in (8, 24):
        lim = med(bas, "limite_bloques", impl="Autores", hilos=h)
        celdas = [f"{med(bas, 'pico_bloques', impl=i, hilos=h):,.0f}" for i in impls_bas]
        out.append(f"| {h} | {lim:.0f} | " + " | ".join(celdas) + " |")

    texto = "\n".join(out) + "\n"
    (RES / "resumen_v1_v2.md").write_text(texto, encoding="utf-8")
    print(texto)

    # --- Gráfica 1: Comparativa 6 paneles a 24 hilos ---
    plt.rcParams.update({"font.size": 9.5, "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.alpha": 0.35})
    fig, ejes = plt.subplots(2, 3, figsize=(11.5, 5.8), constrained_layout=True)
    im_plot = ["Autores", "Autores-v1", "Autores-v2", "Anderson", "Folly"]
    for ax, (n, u) in zip(ejes.flat, PANELES):
        ys = [med(rend, impl=i, n=n, u=u, hilos=24) for i in im_plot]
        bars = ax.bar(range(len(im_plot)), ys, color=[COL[i] for i in im_plot], width=0.62)
        for b, y in zip(bars, ys):
            ax.text(b.get_x() + b.get_width()/2, y, f"{y:.1f}", ha="center", va="bottom", fontsize=8)
        ax.set_xticks(range(len(im_plot)), ["Orig.", "v1\n(1A)", "v2\n(1A+1B)", "And.", "Folly"], fontsize=8)
        ax.set_title(titulo_panel(n, u), fontweight="bold")
        ax.set_ylim(0, max(ys) * 1.18)
        ax.grid(axis="x", visible=False)
    for ax in ejes[:, 0]:
        ax.set_ylabel("Mop/s (24 hilos)")
    fig.suptitle("Ablación directa sobre Autores (Euro-Par 2024) en Core Ultra 9 285 — 24 hilos", fontweight="bold")
    fig.savefig(FIG / "ablacion_fig5_24h.png", dpi=160, bbox_inches="tight")
    plt.close(fig)

    # --- Gráfica 2: Escalado n=10^6 y n=1 u=50% ---
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(10.5, 4.2), constrained_layout=True)
    for i in ["Autores", "Autores-v1", "Autores-v2", "Anderson", "Libstdc++"]:
        hs = [1, 6, 12, 18, 24]
        ys = [med(rend, impl=i, n="1000000", u="50", hilos=h) for h in hs]
        ax1.plot(hs, ys, color=COL[i], marker=MARCA[i], lw=2.2, ms=6.5, label=i)
    ax1.set_title("n = 10⁶, u = 50 % (baja contención / RAM)", fontweight="bold")
    ax1.set_xlabel("hilos"); ax1.set_ylabel("Mop/s"); ax1.set_xticks([1, 6, 12, 18, 24]); ax1.set_ylim(bottom=0)
    ax1.legend(frameon=False, fontsize=8.5)

    for i in ["Autores", "Autores-v1", "Autores-v2", "Autores-v2p", "Anderson", "Folly"]:
        hs = [1, 6, 12, 24]
        ys = [med(rend, impl=i, n="1", u="50", hilos=h) for h in hs]
        ax2.plot(hs, ys, color=COL[i], marker=MARCA[i], lw=2.2, ms=6.5, label=i)
    ax2.set_title("n = 1, u = 50 % (contención extrema)", fontweight="bold")
    ax2.set_xlabel("hilos"); ax2.set_ylabel("Mop/s"); ax2.set_xticks([1, 6, 12, 24]); ax2.set_ylim(bottom=0)
    ax2.legend(frameon=False, fontsize=8.5)
    fig.savefig(FIG / "escalado_n1M_y_n1u50.png", dpi=160, bbox_inches="tight")
    plt.close(fig)


if __name__ == "__main__":
    main()
