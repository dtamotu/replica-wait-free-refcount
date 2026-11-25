#!/usr/bin/env python3
"""Analiza resultados de ablación de Autores-v3 y genera resumen_v3.md + figuras PNG."""
import csv
import statistics as st
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
DIR = ROOT / "resultados_v3"
FIG = DIR / "figuras"
FIG.mkdir(parents=True, exist_ok=True)

PANELES = [
    (1, 1, "n = 1, u = 1 %"),
    (1, 10, "n = 1, u = 10 %"),
    (1, 50, "n = 1, u = 50 %"),
    (10, 1, "n = 10, u = 1 %"),
    (10, 10, "n = 10, u = 10 %"),
    (1000000, 50, "n = 10⁶, u = 50 %"),
]


def cargar():
    rend = defaultdict(list)
    rss = defaultdict(list)
    total = 0
    fallos = 0
    with open(DIR / "rendimiento_v3.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            total += 1
            if r["codigo"] != "0" or not r["mops"]:
                fallos += 1
                continue
            k = (r["impl"], int(r["n"]), int(r["u"]), int(r["hilos"]))
            rend[k].append(float(r["mops"]))
            rss[k].append(int(r["rss_kb"]))
    return rend, rss, total, fallos


def med(vals):
    return st.median(vals) if vals else float("nan")


def main():
    rend, rss, total, fallos = cargar()
    md = [
        "# Resultados de Ablación: Autores-v3 (Elimination-Backoff L2 + P/E-core)",
        "",
        f"- Ejecuciones totales: **{total}** (fallidas: {fallos}).",
        "",
        "## Rendimiento en los 6 paneles de la Figura 5 (24 hilos, mediana de 3 rondas × 2 s, Mop/s)",
        "",
        "Entre paréntesis, factor frente a `Autores` original (`1.00×` = igual). `Anderson-v3` es el control justo (Anderson + misma capa de eliminación L2).",
        "",
    ]

    algs24 = ["Autores", "Autores-v2", "Autores-v3a", "Autores-v3g", "Autores-v3", "Anderson", "Anderson-v3", "Folly"]
    md.append("| Panel | " + " | ".join(algs24) + " |")
    md.append("|" + "---|" * (len(algs24) + 1))
    for n, u, eti in PANELES:
        base = med(rend[("Autores", n, u, 24)])
        row = [eti]
        for a in algs24:
            v = med(rend[(a, n, u, 24)])
            if a == "Autores-v3":
                row.append(f"**{v:.2f}** ({v/base:.2f}×)")
            else:
                row.append(f"{v:.2f} ({v/base:.2f}×)")
        md.append("| " + " | ".join(row) + " |")

    md += [
        "",
        "## Rendimiento a 1 hilo (verificar cero regresión sin contención, Mop/s)",
        "",
    ]
    algs1 = ["Autores", "Autores-v2", "Autores-v3a", "Autores-v3", "Anderson"]
    md.append("| Panel | " + " | ".join(algs1) + " |")
    md.append("|" + "---|" * (len(algs1) + 1))
    for n, u, eti in PANELES:
        base = med(rend[("Autores", n, u, 1)])
        row = [eti]
        for a in algs1:
            v = med(rend[(a, n, u, 1)])
            row.append(f"{v:.2f} ({v/base:.2f}×)")
        md.append("| " + " | ".join(row) + " |")

    for n, u, titulo in [(1, 50, "n = 1, u = 50 %"), (1, 10, "n = 1, u = 10 %")]:
        md += [
            "",
            f"## Escalado en contención extrema: {titulo} (Mop/s)",
            "",
        ]
        algs_sc = ["Autores", "Autores-v2", "Autores-v3a", "Autores-v3g", "Autores-v3", "Anderson", "Anderson-v3"]
        md.append("| Hilos | " + " | ".join(algs_sc) + " |")
        md.append("|" + "---|" * (len(algs_sc) + 1))
        for h in (1, 6, 12, 18, 24):
            base = med(rend[("Autores", n, u, h)])
            row = [str(h)]
            for a in algs_sc:
                v = med(rend[(a, n, u, h)])
                if np.isnan(v):
                    row.append("—")
                elif a == "Autores-v3":
                    row.append(f"**{v:.2f}** ({v/base:.2f}×)")
                else:
                    row.append(f"{v:.2f} ({v/base:.2f}×)")
            md.append("| " + " | ".join(row) + " |")

    md += [
        "",
        "## Memoria residente pico RSS en n = 10⁶, u = 50 % a 24 hilos (MB)",
        "",
        "| Algoritmo | Pico RSS (MB) |",
        "|---|---:|",
    ]
    for a in algs24:
        r_mb = med(rss[(a, 1000000, 50, 24)]) / 1024.0
        md.append(f"| {a} | {r_mb:,.0f} MB |")

    (DIR / "resumen_v3.md").write_text("\n".join(md) + "\n", encoding="utf-8")

    # Figura 1: Ablación en los 6 paneles a 24 hilos
    fig, axes = plt.subplots(2, 3, figsize=(14, 7.6), dpi=160)
    colores = {
        "Autores": "#6E7B8B",
        "Autores-v2": "#2E86AB",
        "Autores-v3a": "#9B5DE5",
        "Autores-v3g": "#F4A261",
        "Autores-v3": "#1B7F3B",
        "Anderson": "#C8551F",
        "Anderson-v3": "#D62828",
        "Folly": "#8D99AE",
    }
    algs_plot = ["Autores", "Autores-v2", "Autores-v3a", "Autores-v3g", "Autores-v3", "Anderson", "Anderson-v3", "Folly"]
    for ax, (n, u, eti) in zip(axes.flat, PANELES):
        vals = [med(rend[(a, n, u, 24)]) for a in algs_plot]
        bars = ax.bar(range(len(algs_plot)), vals, color=[colores[a] for a in algs_plot], width=0.72)
        ax.set_title(eti, fontsize=11, fontweight="bold")
        ax.set_xticks(range(len(algs_plot)))
        ax.set_xticklabels(["Orig", "v2", "v3a", "v3g", "v3", "And", "And-v3", "Folly"], fontsize=8, rotation=20)
        ax.set_ylabel("Mop/s" if ax in axes[:, 0] else "")
        ax.grid(axis="y", alpha=0.28)
        for b, v in zip(bars, vals):
            ax.text(b.get_x() + b.get_width() / 2, v, f"{v:.2f}" if v < 15 else f"{v:.1f}",
                    ha="center", va="bottom", fontsize=7.5, fontweight="bold" if b is bars[4] else "normal")
    fig.suptitle("Ablación Autores-v3 en los 6 paneles de la Figura 5 (24 hilos, Intel Core Ultra 9 285)",
                 fontsize=13, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(FIG / "ablacion_v3_fig5_24h.png")
    plt.close(fig)

    # Figura 2: Escalado en n=1, u=50% y n=1, u=10%
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12.5, 4.8), dpi=160)
    hs = [6, 12, 18, 24]
    series = [
        ("Autores", "--", "o"),
        ("Autores-v2", "-", "s"),
        ("Autores-v3a", ":", "v"),
        ("Autores-v3g", "-.", "^"),
        ("Autores-v3", "-", "D"),
        ("Anderson", "--", "x"),
        ("Anderson-v3", ":", "P"),
    ]
    for ax, (n, u, tit) in [(ax1, (1, 50, "Contención extrema: n = 1, u = 50 %")),
                            (ax2, (1, 10, "Contención alta: n = 1, u = 10 %"))]:
        for a, ls, mk in series:
            ys = [med(rend[(a, n, u, h)]) for h in hs]
            lw = 2.6 if a == "Autores-v3" else 1.7
            ax.plot(hs, ys, linestyle=ls, marker=mk, linewidth=lw, color=colores[a], label=a)
        ax.set_title(tit, fontsize=11, fontweight="bold")
        ax.set_xlabel("Hilos")
        ax.set_ylabel("Rendimiento (Mop/s)")
        ax.set_xticks(hs)
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8.5)
    fig.suptitle("Efecto de Elimination-Backoff L2+P/E vs Genérico vs Backoff puro (6 a 24 hilos)",
                 fontsize=12.5, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(FIG / "escalado_v3_contencion.png")
    plt.close(fig)

    print("Análisis v3 generado:", DIR / "resumen_v3.md")


if __name__ == "__main__":
    main()
