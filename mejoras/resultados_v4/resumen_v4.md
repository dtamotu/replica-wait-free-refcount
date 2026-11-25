# Resultados de Autores-v4 (CAS Wait-Free Fast-Path / Slow-Path)

- Ejecuciones adversariales (23 lectores + 1 escritor CAS): **50**.
- Ejecuciones de camino rápido (`bench_stack`): **180**.

## 1. Ataque adversarial §5.1 (23 lectores continuos vs 1 escritor CAS, mediana de 5 rondas × 2 s)

### Escritor CAS fijado en P-core (cpu2), 23 lectores en el resto

| Algoritmo | p50 (ns) | p99 (ns) | p99.95 (ns) | Máx (ns) | Escritor (Kops/s) | Lectores (Mop/s) |
|---|---:|---:|---:|---:|---:|---:|
| Autores | 1,165 | 3,049 | 4,153 | 507,201 | 779.2 | 20.87 |
| Autores-v2 | 1,335 | 2,577 | 3,880 | 140,067 | 717.4 | 23.78 |
| **Autores-v4** | **1,068** | **2,079** | **3,043** | **684,632** | **908.2** | 17.19 |
| Anderson | 1,156 | 2,974 | 4,305 | 11,195,337 | 788.1 | 21.10 |
| Folly | 3,887 | 54,489 | 957,962 | 1,597,956 | 119.8 | 9.86 |

### Ataque híbrido Arrow Lake: escritor CAS fijado en E-core (cpu8), 23 lectores incluyendo los 8 P-cores

| Algoritmo | p50 (ns) | p99 (ns) | p99.95 (ns) | Máx (ns) | Escritor (Kops/s) | Lectores (Mop/s) |
|---|---:|---:|---:|---:|---:|---:|
| Autores | 7,366 | 87,788 | 259,481 | 746,501 | 98.7 | 22.76 |
| Autores-v2 | 14,414 | 138,776 | 549,332 | 745,706 | 44.4 | 23.74 |
| **Autores-v4** | **5,182** | **10,748** | **14,305** | **124,060** | **210.0** | 15.54 |
| Anderson | 5,136 | 17,966 | 23,857 | 11,116,882 | 159.4 | 16.58 |
| Folly | 2,476,727 | 15,559,044 | 18,707,672 | 25,110,782 | 0.3 | 10.76 |

## 2. Sobrecosto en camino rápido sobre la Figura 5 (mediana de 3 rondas × 2 s, Mop/s)

Entre paréntesis, razón `Autores-v4 / Autores-v2` (1.00× = cero sobrecosto).

| Panel | Hilos | Autores | Autores-v2 | Autores-v4 | Anderson | Folly |
|---|---:|---:|---:|---:|---:|---:|
| n = 1, u = 1 % | 1 | 93.51 | 99.12 | **101.04** (1.02×) | 78.63 | 83.81 |
| n = 1, u = 1 % | 24 | 19.65 | 18.45 | **15.66** (0.85×) | 17.92 | 9.42 |
| n = 1, u = 10 % | 1 | 52.68 | 67.87 | **68.26** (1.01×) | 53.87 | 55.55 |
| n = 1, u = 10 % | 24 | 4.46 | 6.91 | **5.48** (0.79×) | 8.75 | 5.06 |
| n = 1, u = 50 % | 1 | 18.08 | 28.34 | **28.31** (1.00×) | 22.48 | 22.89 |
| n = 1, u = 50 % | 24 | 1.04 | 1.61 | **1.49** (0.93×) | 2.04 | 1.46 |
| n = 10, u = 1 % | 1 | 94.67 | 99.91 | **101.44** (1.02×) | 80.59 | 84.24 |
| n = 10, u = 1 % | 24 | 129.66 | 136.72 | **85.21** (0.62×) | 71.32 | 50.55 |
| n = 10, u = 10 % | 1 | 55.06 | 69.90 | **70.13** (1.00×) | 55.29 | 58.35 |
| n = 10, u = 10 % | 24 | 55.44 | 55.32 | **47.98** (0.87×) | 60.85 | 36.53 |
| n = 10⁶, u = 50 % | 1 | 3.32 | 5.68 | **5.84** (1.03×) | 4.57 | 4.31 |
| n = 10⁶, u = 50 % | 24 | 49.44 | 69.44 | **63.71** (0.92×) | 58.70 | 51.99 |

## 3. Memoria retenida (`bench_basura`, mediana de 3 rondas: pico de bloques vivos)

| Hilos | Límite | Autores | Autores-v2 | Autores-v4 | Anderson | Folly |
|---:|---:|---:|---:|---:|---:|---:|
| 8 | 42 | 21 | 21 | 21 | 15,442 | 23 |
| 24 | 122 | 53 | 53 | 55 | 25,524 | 53 |
