# Fase de Mejoras sobre la Réplica (`mejoras-autores`)

Este directorio contiene la fase de **extensión e investigación original** construida directamente sobre nuestra réplica experimental del artículo de Euro-Par 2024:

> Ivo Gabe de Wolff, Daniel Anderson, Gabriele K. Keller & Aleksei Seletskiy, *«A Fast Wait-Free Solution to Read-Reclaim Races in Reference Counting»*, Euro-Par 2024, LNCS 14803, pp. 103–118, DOI [`10.1007/978-3-031-69583-4_8`](https://doi.org/10.1007/978-3-031-69583-4_8).

Toda modificación se evalúa en **ablación estricta** frente al código original de los autores (`Autores` / `ivo`) y a los rivales oficiales (`Anderson`, `Folly`, `Williams`, `Libstdc++`) sobre el mismo arnés del artefacto de Zenodo, sin alterar `main`, `codigo/bench.cpp` ni los CSV cerrados de `replica/resultados/david-ultra9-285/`.

## Documentación de la escalera de 4 propuestas

- **[docs/01_propuestas_detalladas.md](docs/01_propuestas_detalladas.md)** — Especificación técnica y experimental de las 4 propuestas (`Autores-v1` a `Autores-v4`): debilidades exactas medidas en la réplica, cambios de diseño, protocolos de validación, métricas y garantías de progreso/memoria.
- **[docs/02_propuestas_accesible.md](docs/02_propuestas_accesible.md)** — Versión accesible («en lenguaje llano») con analogías paso a paso de las 4 propuestas y lectura intuitiva de los resultados de `v1` y `v2`.
- **[docs/03_papers_respaldo.md](docs/03_papers_respaldo.md)** — Catálogo bibliográfico verificado con DOIs, venues, aportes clave y mapeo directo a cada una de las 4 propuestas (y al contexto 2024–2026 de reclamación segura de memoria).

## Informe LaTeX y PDF

- **[informe_latex/informe_mejoras_completo.tex](informe_latex/informe_mejoras_completo.tex)** y **[informe_latex/referencias_mejoras.bib](informe_latex/referencias_mejoras.bib)** — Informe completo de las 4 mejoras (`v1`, `v2`, `v3` y `v4`).
- **[informe_latex/informe_mejoras_completo.pdf](informe_latex/informe_mejoras_completo.pdf)** — PDF compilado completo con todas las tablas, análisis de ablación y figuras.
- *(Versión histórica de la primera etapa: [informe_mejoras_v1_v2.pdf](informe_latex/informe_mejoras_v1_v2.pdf).)*

## Estado actual de la escalera

| Peldaño | Descripción breve | Estado | Código y datos |
|---|---|---|---|
| **1. `Autores-v1`** | `CAS` sin copias por valor, pre-chequeo antes de incrementar `new_cb`, `memory_order_relaxed` en incrementos locales y ablación de `_mm_pause()` | **Implementado, validado y medido** | [atomic_shared_ptr_v1.h](atomic_shared_ptr_v1.h) · [resumen_v1_v2.md](resultados_v1_v2/resumen_v1_v2.md) |
| **2. `Autores-v2`** | Todo `v1` + bloque de control *in-place* sin *vtable* (1 asignación), `shared_ptr<T>` de 8 B y `load()` sin indirección extra | **Implementado, validado y medido** | [atomic_shared_ptr_v2.h](atomic_shared_ptr_v2.h) · [resumen_v1_v2.md](resultados_v1_v2/resumen_v1_v2.md) |
| **3. `Autores-v3`** | *Elimination-Backoff* consciente de clústeres L2 en la pila de Treiber + *backoff* adaptativo P-core / E-core | **Implementado, validado y medido** | [atomic_shared_ptr_v3.h](atomic_shared_ptr_v3.h) · [elimination_table_v3.h](elimination_table_v3.h) · [resumen_v3.md](resultados_v3/resumen_v3.md) |
| **4. `Autores-v4`** | `compare_and_set` sin inanición (*Fast-Path / Slow-Path* híbrido con protección por ranura local + `rc_try_increment`, cierra §5.1) | **Implementado, validado y medido** | [atomic_shared_ptr_v4.h](atomic_shared_ptr_v4.h) · [bench_adversarial_v4.cpp](bench_adversarial_v4.cpp) · [resumen_v4.md](resultados_v4/resumen_v4.md) |

## Pruebas, automatización y resultados (`v1 + v2`)

- Validación de corrección y cota *wait-free*: [test_correctitud.cpp](test_correctitud.cpp), [test_cota_v2.cpp](test_cota_v2.cpp)
- Scripts de medición y análisis: [correr_v1_v2.py](correr_v1_v2.py), [analizar_v1_v2.py](analizar_v1_v2.py)
- Datos crudos y figuras: [resultados_v1_v2/rendimiento_v1_v2.csv](resultados_v1_v2/rendimiento_v1_v2.csv), [resultados_v1_v2/latencia_v1_v2.csv](resultados_v1_v2/latencia_v1_v2.csv), [resultados_v1_v2/basura_v1_v2.csv](resultados_v1_v2/basura_v1_v2.csv), [ablacion_fig5_24h.png](resultados_v1_v2/figuras/ablacion_fig5_24h.png), [escalado_n1M_y_n1u50.png](resultados_v1_v2/figuras/escalado_n1M_y_n1u50.png)
