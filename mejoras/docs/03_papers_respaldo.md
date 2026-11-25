# Catálogo de Papers de Respaldo Verificados (Propuestas `v1` a `v4`)

Todas las referencias listadas en este documento han sido verificadas por título, autores, *venue* y DOI (o número de propuesta ISO C++). Se agrupan según la propuesta que sustentan y el contexto del estado del arte (2024–2026).

---

## 0. Artículo base y artefacto oficial

1. **de Wolff, I. G., Anderson, D., Keller, G. K., & Seletskiy, A. (2024).** *A Fast Wait-Free Solution to Read-Reclaim Races in Reference Counting.* En *Euro-Par 2024: Parallel Processing*, LNCS 14803, pp. 103–118, Springer.
   - **DOI:** [`10.1007/978-3-031-69583-4_8`](https://doi.org/10.1007/978-3-031-69583-4_8)
   - **Artefacto Zenodo:** DOI [`10.5281/zenodo.11775182`](https://doi.org/10.5281/zenodo.11775182)
   - **Por qué es central:** Define el conteo ponderado con pesos locales y recarga acotada en $O(P)$. En §2.1 (Fig. 1a) asume el contador junto al objeto (base de **`v2`**), en `arc.rs` pasa referencias sin copia (base de **`v1`**) y en §1.2 / §5.1 declara explícitamente que `compare_and_set` queda solo *lock-free* (base de **`v4`**).

---

## 1. Respaldo para Propuesta 1 (`Autores-v1`: Higiene de `CAS`, *test-and-test-and-set* y ablación de `PAUSE`)

2. **Mellor-Crummey, J. M., & Scott, M. L. (1991).** *Algorithms for Scalable Synchronization on Shared-Memory Multiprocessors.* *ACM Transactions on Computer Systems (TOCS)*, 9(1), 21–65.
   - **DOI:** [`10.1145/103727.103729`](https://doi.org/10.1145/103727.103729)
   - **Aporte directo:** Fundamenta el principio de lectura previa antes de emitir escrituras atómicas (*test-and-test-and-set*) para reducir el tráfico de invalidación de coherencia de caché bajo contención, así como la gestión de *backoff*/*livelock* en bucles de sincronización.

3. **Anderson, D., Blelloch, G. E., & Wei, Y. (2022).** *Turning Manual Concurrent Memory Reclamation into Automatic Reference Counting.* *Proceedings of ACM SIGPLAN PLDI 2022*.
   - **DOI:** [`10.1145/3519939.3523730`](https://doi.org/10.1145/3519939.3523730)
   - **Aporte directo:** En la implementación `parlay` (`atomic_shared_ptr_custom.hpp`), los autores documentan explícitamente el costo de pasar `desired` por copia en `compare_exchange_weak` (incrementos/decrementos atómicos desperdiciados en cada fallo) y emplean `control_block_inplace` para asignación única (respaldando tanto **`v1`** como **`v2`**).

---

## 2. Respaldo para Propuesta 2 (`Autores-v2`: Bloque de control *in-place* y metodología P/E)

4. **Dimov, P., & Dawes, B. (2007).** *Improving `shared_ptr` for C++0x, Revision 2.* ISO/IEC JTC1/SC22/WG21 Proposal **N2351**.
   - **Enlace oficial:** [https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2351.htm](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2351.htm)
   - **Aporte directo:** Propuesta estándar que introdujo `std::make_shared` para fusionar el bloque de control y el objeto `T` en una sola reserva de memoria, reduciendo a la mitad las asignaciones y mejorando la localidad de caché.

5. **Hoefler, T., & Belli, R. (2015).** *Scientific Benchmarking of Parallel Computing Systems: Twelve Ways to Tell the Masses When Reporting Performance Results.* *Proceedings of SC '15*.
   - **DOI:** [`10.1145/2807591.2807644`](https://doi.org/10.1145/2807591.2807644)
   - **Aporte directo:** Guía metodológica de referencia para experimentos paralelos reproducibles: uso de medianas, percentiles, control de colocación/afinidad de hilos y reporte honesto de variabilidad y resultados negativos.

6. **Schöne, R., Velten, M., Hackenberg, D., & Ilsche, T. (2024).** *Energy Efficiency Features of the Intel Alder Lake Architecture.* *Proceedings of ACM/SPEC ICPE 2024*.
   - **DOI:** [`10.1145/3629526.3645040`](https://doi.org/10.1145/3629526.3645040)
   - **Aporte directo:** Caracteriza la asimetría P-core / E-core, latencias de coherencia entre clústeres L2 y efectos del gobernador DVFS en arquitecturas híbridas modernas de Intel; explica directamente la bimodalidad de latencia (`todos24-P` vs `todos24-E`) que comprobamos causalmente en `v1/v2`.

7. **Mytkowicz, T., Diwan, A., Hauswirth, M., & Sweeney, P. F. (2009).** *Producing Wrong Data Without Doing Anything Obviously Wrong!* *Proceedings of ASPLOS 2009*.
   - **DOI:** [`10.1145/1508244.1508275`](https://doi.org/10.1145/1508244.1508275)
   - **Aporte directo:** Muestra cómo detalles de disposición en memoria, *layout* de estructuras o banderas del compilador pueden invertir comparativas experimentales entre sistemas (clave para explicar por qué el `ivo` C++ original perdía en `n = 10⁶` y por qué `Autores-v2` revierte el resultado).

---

## 3. Respaldo para Propuesta 3 (`Autores-v3`: *Elimination-Backoff* L2 + *backoff* adaptativo P/E)

8. **Hendler, D., Shavit, N., & Yerushalmi, L. (2004).** *A Scalable Lock-Free Stack Algorithm.* *Proceedings of the 16th Annual ACM Symposium on Parallelism in Algorithms and Architectures (SPAA '04)*, pp. 206–215 (y versión extendida en *Journal of Parallel and Distributed Computing*, 70(1), 2010).
   - **DOI:** [`10.1145/1007912.1007944`](https://doi.org/10.1145/1007912.1007944)
   - **Aporte directo:** Introduce el **Elimination-Backoff Stack**: ante contención en la pila de Treiber, las operaciones opuestas (`push` y `pop`) se emparejan en un arreglo lateral de eliminación sin tocar `head`, convirtiendo el cuello de botella secuencial en paralelismo linealizable.

9. **Liu, J., Gu, J., Tang, Y., Li, Y., Zang, B., & Chen, H. (2022).** *LibASL: Asymmetry-aware Scalable Locking.* *PPoPP 2022* / arXiv:2108.03355.
   - **DOI:** [`10.48550/arXiv.2108.03355`](https://doi.org/10.48550/arXiv.2108.03355)
   - **Aporte directo:** Demuestra cómo las CPUs heterogéneas (núcleos rápidos y lentos) sufren degradación y efectos *convoy* cuando todos los núcleos compiten simétricamente por las mismas variables de sincronización, y propone políticas conscientes del tipo de núcleo.

10. **Ouyang, P., & Zhu, W. (2022).** *Core-aware combining: Accelerating critical section execution on heterogeneous multi-core systems via combining synchronization.* *Journal of Parallel and Distributed Computing (JPDC)*, 162, 27–43.
    - **DOI:** [`10.1016/j.jpdc.2022.01.001`](https://doi.org/10.1016/j.jpdc.2022.01.001)
    - **Aporte directo:** Evidencia empírica y algorítmica de que explotar la localidad por dominio/tipo de núcleo en arquitecturas heterogéneas reduce drásticamente el tráfico inter-clúster bajo contención extrema.

---

## 4. Respaldo para Propuesta 4 (`Autores-v4`: `CAS` *wait-free* mediante *Fast-Path / Slow-Path Helping*)

11. **Kogan, A., & Petrank, E. (2012).** *A Methodology for Creating Fast Wait-Free Data Structures.* *Proceedings of ACM SIGPLAN PPoPP 2012*, pp. 141–150.
    - **DOI:** [`10.1145/2370036.2145835`](https://doi.org/10.1145/2370036.2145835)
    - **Aporte directo:** Propone el esquema clásico **Fast-Path / Slow-Path** para convertir operaciones *lock-free* en *wait-free* conservando el rendimiento del camino rápido y activando ayuda cooperativa solo tras $K$ fallos consecutivos.

12. **Timnat, S., & Petrank, E. (2014).** *A Practical Wait-Free Simulation for Lock-Free Data Structures.* *Proceedings of ACM SIGPLAN PPoPP 2014*, pp. 357–368.
    - **DOI:** [`10.1145/2555243.2555261`](https://doi.org/10.1145/2555243.2555261)
    - **Aporte directo:** Generaliza la simulación *wait-free* práctica sobre primitivas *lock-free* con sobrecosto mínimo en ausencia de inanición.

13. **Nikolaev, R., & Ravindran, B. (2024).** *A Family of Fast and Memory Efficient Lock- and Wait-Free Reclamation* (*Crystalline-W*). *PACMPL* 8 (*PLDI 2024*).
    - **DOI:** [`10.1145/3658851`](https://doi.org/10.1145/3658851)
    - **Aporte directo:** Estado del arte reciente en reclamación *wait-free* acotada mediante ayuda cooperativa eficiente y baja huella de memoria; referencia directa para diseñar y evaluar el costo del mecanismo de ayuda de `Autores-v4`.

---

## 5. Literatura reciente relacionada (contexto 2021–2025 para discusión y trabajo futuro)

14. **Anderson, D., Blelloch, G. E., & Wei, Y. (2021).** *Concurrent Deferred Reference Counting with Constant-Time Overhead.* *PLDI 2021*. **DOI:** [`10.1145/3453483.3454060`](https://doi.org/10.1145/3453483.3454060) — Algoritmo rival principal (`Anderson` / CDRC) usado en la réplica.
15. **Parkinson, M. J., Clebsch, S., & Simner, B. (2023).** *Wait-Free Weak Reference Counting.* *ISMM 2023*. **DOI:** [`10.1145/3591195.3595271`](https://doi.org/10.1145/3591195.3595271) — Extensión hacia punteros débiles *wait-free*.
16. **Jung, J., Kim, J., Parkinson, M. J., & Kang, J. (2024).** *Concurrent Immediate Reference Counting (CIRC).* *PLDI 2024* (*PACMPL* 8). **DOI:** [`10.1145/3656383`](https://doi.org/10.1145/3656383) — Alternativa contemporánea de conteo inmediato con punteros débiles y ciclos.
17. **Kim, D., Brown, T., & Singh, A. (2024).** *Are Your Epochs Too Epic? Batch Free Can Be Harmful.* *PPoPP 2024*. **DOI:** [`10.1145/3627535.3638491`](https://doi.org/10.1145/3627535.3638491) — Explica los picos `p99.95` de `7 770 ns` que medimos en la liberación por lotes de Anderson frente a `32 ns` de `Autores-v2`.
18. **Moreno, P., Areias, M., & Rocha, R. (2025).** *A sleek lock-free hash map in an ERA of safe memory reclamation methods.* *Parallel Computing*, 125. **DOI:** [`10.1016/j.parco.2025.103162`](https://doi.org/10.1016/j.parco.2025.103162) — Contexto de evaluación comparativa de esquemas modernos de reclamación segura de memoria.
19. **Kokologiannakis, M., & Vafeiadis, V. (2021).** *GenMC: A Model Checker for Weak Memory Models.* *CAV 2021*, LNCS 12759. **DOI:** [`10.1007/978-3-030-81685-8_20`](https://doi.org/10.1007/978-3-030-81685-8_20) — Herramienta de verificación formal recomendada para certificar `Autores-v4` bajo modelos de memoria débil.
20. **Arovi, O., & Nikolaev, R. (2025).** *RRR-SMR: Reduce, Reuse, Recycle.* *PLDI 2025*. **DOI:** [`10.1145/3729337`](https://doi.org/10.1145/3729337) y **Singh, A., & Brown, T. (2025).** *Publish on Ping: A Better Way to Publish Reservations in Memory Reclamation.* *PPoPP 2025*. **DOI:** [`10.1145/3710848.3710890`](https://doi.org/10.1145/3710848.3710890) — Estado del arte 2025 en reclamación segura con baja contención.
