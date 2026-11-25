#!/usr/bin/env python3
"""Analiza resultados de Autores-v4 (adversarial 23L+1E, camino rápido y basura) y genera resumen_v4.md + figuras."""
import csv
import statistics as st
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
DIR = ROOT / "resultados_v4"
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


def med(vals):
    return st.median(vals) if vals else float("nan")


def main():
    adv = defaultdict(lambda: defaultdict(list))
    n_adv = 0
    with open(DIR / "adversarial_v4.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            n_adv += 1
            k = (r["impl"], r["corte"])
            for col in ("p50_ns", "p99_ns", "p9995_ns", "max_ns", "writer_kops", "reader_mops"):
                adv[k][col].append(float(r[col]))

    rend = defaultdict(list)
    rss = defaultdict(list)
    n_rend = 0
    with open(DIR / "rendimiento_v4.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            n_rend += 1
            if r["codigo"] == "0" and r["mops"]:
                k = (r["impl"], int(r["n"]), int(r["u"]), int(r["hilos"]))
                rend[k].append(float(r["mops"]))
                rss[k].append(int(r["rss_kb"]))

    bas = defaultdict(list)
    bas_lim = {}
    with open(DIR / "basura_v4.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            h = int(r["hilos"])
            bas[(r["impl"], h)].append(int(r["pico_bloques"]))
            bas_lim[h] = int(r["limite_bloques"])

    algs = ["Autores", "Autores-v2", "Autores-v4", "Anderson", "Folly"]
    md = [
        "# Resultados de Autores-v4 (CAS Wait-Free Fast-Path / Slow-Path)",
        "",
        f"- Ejecuciones adversariales (23 lectores + 1 escritor CAS): **{n_adv}**.",
        f"- Ejecuciones de camino rápido (`bench_stack`): **{n_rend}**.",
        "",
        "## 1. Ataque adversarial §5.1 (23 lectores continuos vs 1 escritor CAS, mediana de 5 rondas × 2 s)",
        "",
    ]

    for corte, desc in [("escritor-P", "Escritor CAS fijado en P-core (cpu2), 23 lectores en el resto"),
                        ("escritor-E", "Ataque híbrido Arrow Lake: escritor CAS fijado en E-core (cpu8), 23 lectores incluyendo los 8 P-cores")]:
        md += [
            f"### {desc}",
            "",
            "| Algoritmo | p50 (ns) | p99 (ns) | p99.95 (ns) | Máx (ns) | Escritor (Kops/s) | Lectores (Mop/s) |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
        for a in algs:
            d = adv[(a, corte)]
            p50 = med(d["p50_ns"])
            p99 = med(d["p99_ns"])
            p9995 = med(d["p9995_ns"])
            mx = med(d["max_ns"])
            wk = med(d["writer_kops"])
            rm = med(d["reader_mops"])
            if a == "Autores-v4":
                md.append(f"| **{a}** | **{p50:,.0f}** | **{p99:,.0f}** | **{p9995:,.0f}** | **{mx:,.0f}** | **{wk:,.1f}** | {rm:.2f} |")
            else:
                md.append(f"| {a} | {p50:,.0f} | {p99:,.0f} | {p9995:,.0f} | {mx:,.0f} | {wk:,.1f} | {rm:.2f} |")
        md.append("")

    md += [
        "## 2. Sobrecosto en camino rápido sobre la Figura 5 (mediana de 3 rondas × 2 s, Mop/s)",
        "",
        "Entre paréntesis, razón `Autores-v4 / Autores-v2` (1.00× = cero sobrecosto).",
        "",
        "| Panel | Hilos | Autores | Autores-v2 | Autores-v4 | Anderson | Folly |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for n, u, eti in PANELES:
        for h in (1, 24):
            v0 = med(rend[("Autores", n, u, h)])
            v2 = med(rend[("Autores-v2", n, u, h)])
            v4 = med(rend[("Autores-v4", n, u, h)])
            va = med(rend[("Anderson", n, u, h)])
            vf = med(rend[("Folly", n, u, h)])
            md.append(f"| {eti} | {h} | {v0:.2f} | {v2:.2f} | **{v4:.2f}** ({v4/v2:.2f}×) | {va:.2f} | {vf:.2f} |")

    md += [
        "",
        "## 3. Memoria retenida (`bench_basura`, mediana de 3 rondas: pico de bloques vivos)",
        "",
        "| Hilos | Límite | Autores | Autores-v2 | Autores-v4 | Anderson | Folly |",
        "|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for h in (8, 24):
        row = [str(h), str(bas_lim.get(h, 0))]
        for a in ("Autores", "Autores-v2", "Autores-v4", "Anderson", "Folly"):
            vals = bas[(a, h)]
            row.append(f"{med(vals):,.0f}" if vals else "—")
        md.append("| " + " | ".join(row) + " |")

    (DIR / "resumen_v4.md").write_text("\n".join(md) + "\n", encoding="utf-8")

    # Figura: Latencia de cola adversarial (escala log) + Throughput del escritor
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5.0), dpi=160)
    colores = {
        "Autores": "#6E7B8B",
        "Autores-v2": "#2E86AB",
        "Autores-v4": "#1B7F3B",
        "Anderson": "#C8551F",
        "Folly": "#B3261E",
    }
    x = np.arange(len(algs))
    w = 0.26
    for ax, corte, tit in [(ax1, "escritor-P", "Escritor CAS en P-core vs 23 lectores"),
                           (ax2, "escritor-E", "Ataque híbrido: escritor CAS en E-core vs 23 lectores")]:
        p50s = [med(adv[(a, corte)]["p50_ns"]) / 1e3 for a in algs]
        p9995s = [med(adv[(a, corte)]["p9995_ns"]) / 1e3 for a in algs]
        mxs = [med(adv[(a, corte)]["max_ns"]) / 1e3 for a in algs]
        b1 = ax.bar(x - w, p50s, w, label="p50", color="#90CAF9", edgecolor="#1B2430", linewidth=0.5)
        b2 = ax.bar(x, p9995s, w, label="p99.95", color="#F4A261", edgecolor="#1B2430", linewidth=0.5)
        b3 = ax.bar(x + w, mxs, w, label="Máx", color="#C8551F", edgecolor="#1B2430", linewidth=0.5)
        ax.set_yscale("log")
        ax.set_title(tit, fontsize=11, fontweight="bold")
        ax.set_xticks(x)
        ax.set_xticklabels(algs, fontsize=9)
        ax.set_ylabel("Latencia del escritor CAS (µs, escala log)")
        ax.grid(axis="y", which="both", alpha=0.25)
        ax.legend(fontsize=8.5)
        for b, v in zip(b2, p9995s):
            ax.text(b.get_x() + b.get_width()/2, v * 1.15, f"{v:.1f}", ha="center", va="bottom", fontsize=7.5)
        for b, v in zip(b3, mxs):
            ax.text(b.get_x() + b.get_width()/2, v * 1.15, f"{v:.0f}" if v >= 100 else f"{v:.1f}",
                    ha="center", va="bottom", fontsize=7.5, fontweight="bold")
    fig.suptitle("Ataque adversarial §5.1: 23 lectores continuos vs 1 escritor CAS (mediana 5 rondas × 2 s)",
                 fontsize=12.5, fontweight="bold")
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(FIG / "adversarial_v4_latencia.png")
    plt.close(fig)
    print("Análisis v4 generado:", DIR / "resumen_v4.md")


if __name__ == "__main__":
    main()
