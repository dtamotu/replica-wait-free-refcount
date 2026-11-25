# Escalera de 4 Propuestas Técnicas de Mejora sobre `Autores` (Euro-Par 2024)

Este documento detalla las cuatro mejoras incrementales diseñadas directamente a partir de los cuellos de botella empíricos medidos en nuestra réplica sobre **Intel Core Ultra 9 285** (24 núcleos híbridos: 8 P-cores + 4 clústeres de 4 E-cores sin SMT, 36 MB L3, 1 × 32 GB DDR5-5600) y contrastadas en la literatura de concurrencia.

Todas las propuestas se evalúan en **ablación incremental** frente a la implementación C++ original de los autores (`ivo::atomic_shared_ptr`) usando los mismos binarios oficiales del artefacto Zenodo (`bench_stack`, `bench_atomic_shared_ptrs`, `bench_basura`).

---

## Diagnóstico base de nuestra réplica (`replica/resultados/david-ultra9-285/tablas.md`)

En la réplica cerrada (14 158 mediciones) confirmamos que el algoritmo original de de Wolff et al. cumple su cota *wait-free* en `load`/`store` (máx. 8 reintentos frente a $P = 32$), mantiene liberación inmediata (0 bloques diferidos frente a ~29 000 en Anderson) y logra la menor latencia de lectura (`load`). Sin embargo, emergieron **cuatro debilidades concretas**:

1. **Contención extrema de escritores (`n = 1, u = 10 %` y `u = 50 %` a 24 hilos):** `Autores` cae a `4.43–4.63 Mop/s` (`u = 10 %`) y `1.03–1.07 Mop/s` (`u = 50 %`), quedando a mitad de velocidad frente a `Anderson` (`8.79` y `2.06 Mop/s`) e incluso por debajo de `Folly` (`5.03` y `1.46 Mop/s`).
2. **Desventaja estructural en `n = 10⁶, u = 50 %`:** a 1–12 hilos, `Autores` original rinde `3.0–3.13 Mop/s` frente a `4.54` de `Anderson` y `4.68` de `Libstdc++`, y a 24 hilos retiene `1 075 MB` de RSS frente a `911 MB` de `Anderson`.
3. **Bimodalidad de latencia y caídas por topología híbrida P/E + L2:** en `fig6` a 24 hilos sin afinidad, `load` fluctuaba entre `~414 ns` y `~1 450 ns` (razón máx/mín `5.75×`), y cruzar clústeres L2 en Arrow Lake introduce una penalización abrupta (`−45 %`) en la coherencia de caché.
4. **Limitación teórica declarada en §5.1 del paper:** `load` y `store` son *wait-free*, pero `compare_and_set` / `compare_exchange_*` es solo *lock-free* porque lectores concurrentes modifican el campo de peso (*tag*) de la misma palabra atómica de 64 bits y pueden hacer fallar indefinidamente el `CAS` de un escritor.

---

## Propuesta 1 — `Autores-v1`: Higiene de contadores y semántica `CAS` sin copias

### Debilidad exacta que ataca
- En [`codigo/autores/cpp/atomic_shared_ptr.h#L311-L348`](../codigo/autores/cpp/atomic_shared_ptr.h), `compare_exchange_weak` recibe `new_ptr` **por valor** y delega en `compare_and_set(expected, new_ptr)` pasando **ambos argumentos por valor** (a diferencia del propio Rust de los autores en `codigo/autores/rust/src/arc.rs` y de `parlay`/`folly`).
- En cada intento fallido de `push_front`/`pop_front` en `stack.h`, el original ejecuta hasta **6 operaciones atómicas RMW desperdiciadas** (`fetch_add`/`fetch_sub` sobre los bloques de control de `expected` —la línea de caché más caliente de la pila— y de `new_ptr`), además de incrementar el contador de `new_ptr` *antes* de comprobar si la palabra atómica coincide con `expected`.
- Además, `rc_increment` usa `memory_order_acq_rel` cuando el hilo llamante ya posee una referencia viva (donde `memory_order_relaxed` es suficiente por el modelo de memoria C++11/C++20).

### Cambios implementados ([`mejoras/atomic_shared_ptr_v1.h`](../atomic_shared_ptr_v1.h))
1. Sobrecargas por `const&` y `&&` en `compare_and_set`, `compare_exchange_weak` y `compare_exchange_strong`.
2. **Pre-chequeo de lectura (`test-and-test-and-set`):** leer la palabra atómica con `memory_order_acquire` y verificar `unpack_ptr(observed) == expected.control_block` *antes* de sumar peso al nuevo bloque `new_cb`. Si ya no coincide, recargar `expected` y retornar `false` sin tocar `new_cb`.
3. `rc_increment_relaxed` para incrementos desde una referencia ya poseída, manteniendo `rc_increment_acquire` en `shift_references` y `reference_count.load(memory_order_acquire)` antes de `delete` en `rc_decrement` (limpio bajo TSan y costo cero en x86-64).
4. Protección ante auto-asignación en `operator=`.
5. Parámetro de plantilla `UsePause` (`ivo-v1` vs `ivo-v1p`) para aislar empíricamente el efecto de `_mm_pause()` en Arrow Lake.

### Garantías y validación
- **Progreso y memoria:** 100 % *wait-free* en `load`/`store`; liberación inmediata (0 bloques diferidos).
- **Verificación ejecutada:** ASan + UBSan (0 errores, 0 fugas), TSan bajo `setarch -R` (0 *data races*, 0 advertencias), prueba de recarga forzada `P=32, C=64` y `bench_basura`.
- **Resultado empírico medido (`24 h`):** mejora **+32 %** en `n=1, u=10%` (`4.63 → 6.13 Mop/s`) y **+44 %** en `n=1, u=50%` (`1.03 → 1.48 Mop/s`). Hallazgo negativo documentado: `_mm_pause()` (`v1p`) degrada el rendimiento (`1.48 → 0.94 Mop/s`) porque en microarquitecturas Intel modernas `PAUSE` detiene el *pipeline* ~140 ciclos, excediendo la latencia de coherencia L2 intra-clúster.

### Respaldo bibliográfico
- Mellor-Crummey & Scott (1991), *ACM TOCS*, DOI `10.1145/103727.103729`.
- de Wolff et al. (2024), *Euro-Par 2024*, §5.1, DOI `10.1007/978-3-031-69583-4_8` (y su propio `arc.rs`).
- Anderson, Blelloch & Wei (2022), *PLDI 2022*, DOI `10.1145/3519939.3523730` (nota en `parlay/atomic_shared_ptr_custom.hpp` sobre copias innecesarias en `CAS`).

---

## Propuesta 2 — `Autores-v2`: Bloque de control *in-place* (asignación única) y `shared_ptr` de 8 bytes

### Debilidad exacta que ataca
- En el arnés oficial (`common.hpp`), los 4 rivales (`Anderson`, `Folly`, `Williams`, `Libstdc++`) y el código Rust oficial de los autores alojan el bloque de control **contiguo al objeto `T`** (1 asignación). Solo `ivo` en C++ hacía 2 `new`/`delete` separados + una clase base polimórfica con *vtable* + `shared_ptr<T>` de 16 B, generando doble indirección serial (`tagged_ptr → control_block → ptr → Node`) en cada lectura y el doble de presión sobre el asignador y la caché/DDR5.

### Cambios implementados ([`mejoras/atomic_shared_ptr_v2.h`](../atomic_shared_ptr_v2.h))
1. Todo lo validado en `Autores-v1` (sin `_mm_pause()` en la configuración ganadora).
2. **`ptr_control_block<T>` sin *vtable* con almacenamiento híbrido:** `union Storage { T* ext_ptr; T value; }` + `bool is_inplace`. Al construir con `make_shared<T>(args...)`, `T` se construye *in-place* con una única llamada al asignador (`sizeof(ptr_control_block<Node>) == 32 B` para la pila de Treiber). Se conserva compatibilidad con constructores desde `T*` externo.
3. **`shared_ptr<T>` compacto de 8 bytes:** almacena únicamente `ptr_control_block<T>* control_block` (igual que `Arc<T>` en Rust). El `Node` de la pila se reduce a 16 B y el bloque completo a 32 B.
4. **`load()` de cero indirecciones extra:** ya no lee `control_block->ptr` dentro del camino rápido de lectura; devuelve directamente el puntero al bloque de control.

### Garantías y validación
- **Progreso y memoria:** 100 % *wait-free* en `load`/`store`; cota *wait-free* verificada en [test_cota_v2.cpp](../test_cota_v2.cpp) (`28.45 M ops` ASan, `3.86 M ops` TSan, 0 errores); liberación inmediata (`53` bloques pico a 24 h en `bench_basura` frente a límite `122` y `29 094` de Anderson).
- **Resultado empírico medido:**
  - En `n = 10⁶, u = 50 %`: **+81 %** a 1 hilo (`3.13 → 5.65 Mop/s`), **+39 %** a 24 hilos (`47.31 → 65.98 Mop/s`, superando en **+30 %** a Anderson `50.62` y Libstdc++ `50.05` sin cambiar la RAM DDR5) y **−29.3 % de RSS** (`1 075 MB → 760 MB`).
  - En `n = 1, u = 50 %`: **+58 %** a 24 hilos (`1.03 → 1.63 Mop/s`), superando a `Folly` (`1.46`) en los 6 paneles y recortando la ventaja de Anderson de `2.00×` a `1.26×`.
  - En latencia controlada con afinidad (`todos24-P`): dispersión máx/mín colapsa de `5.75×` a `1.04×`, y `Autores-v2` lidera `load` (`414 ns`), `store` (`484 ns`) y `store-del` (`417 ns`).

### Respaldo bibliográfico
- de Wolff et al. (2024), *Euro-Par 2024*, §2.1 Fig. 1a, DOI `10.1007/978-3-031-69583-4_8`.
- Dimov & Dawes (2007), ISO/IEC C++ Proposal **N2351** (*Improving `shared_ptr` for C++0x*, origen de `std::make_shared`).
- Anderson, Blelloch & Wei (2022), *PLDI 2022*, DOI `10.1145/3519939.3523730`.
- Hoefler & Belli (2015), *SC 2015*, DOI `10.1145/2807591.2807644` y Schöne et al. (2024), *ACM ICPE 2024*, DOI `10.1145/3629526.3645040` (metodología de afinidad P/E).

---

## Propuesta 3 — `Autores-v3`: *Elimination-Backoff* consciente de clústeres L2 + *backoff* adaptativo P/E

### Debilidad exacta que ataca
- Tras `Autores-v2`, **la única brecha real restante frente a Anderson** ocurre bajo contención extrema de escritores sobre una sola pila (`n = 1, u = 10 %`: `6.55` vs `8.79 Mop/s`; `n = 1, u = 50 %`: `1.63` vs `2.06 Mop/s`).
- En esa carga, 24 hilos compiten por una sola línea de caché (`head`), generando tráfico de invalidación entre los 8 P-cores y los 4 clústeres L2 de E-cores. Además, al haber probado en `v1p`/`v2p` que un `_mm_pause()` fijo perjudica en Arrow Lake, cualquier mecanismo de amortiguación debe ser **adaptativo y consciente de la topología**.

### Diseño técnico propuesto (sobre `Autores-v2`)
Se descompone en dos componentes medibles por separado en ablación:
1. **Componente 3a (en `atomic_shared_ptr_v3.h` — *backoff* ligero consciente de núcleo P/E):**
   - Detección en tiempo de ejecución (vía `sched_getcpu()` + tabla de topología de `/sys/devices/system/cpu/cpu*/cache/index2/shared_cpu_list` y `cpu_capacity` / frecuencia máxima) de si el hilo corre en un **P-core** o un **E-core** y en qué **clúster L2** reside.
   - Sin `_mm_pause()` en el primer intento (camino rápido intacto). Tras fallos repetidos de `CAS`, *backoff* exponencial acotado calibrado con ciclos ligeros (p. ej. `nop` / lectura local sin saturar el bus, diferenciado entre P-core y E-core para evitar el efecto *convoy* descrito por LibASL).
2. **Componente 3b (en `elimination_stack_v3.h` — *Elimination-Backoff* local por clúster L2):**
   - Cuando `push_front(val)` o `pop_front()` falla un `CAS` sobre `head` de `atomic_stack`, en lugar de martillar inmediatamente `head`, el hilo intenta emparejar su operación opuesta (`push` ↔ `pop`) en un **arreglo de eliminación particionado por clúster L2** (una línea `alignas(128)` por ranura para cero *false sharing*):
     - Hilos en el mismo clúster de 4 E-cores se intercambian el valor a latencia de L2 compartida (~35 ns) sin tocar el anillo L3 ni `head` ni asignar un `Node` nuevo en caso de emparejamiento exitoso.
     - Hilos en P-cores usan ranuras dedicadas al dominio P con ventana corta de espera antes de reintentar `head`.
   - **Semántica linealizable:** un par `(push(x), pop())` emparejado en el arreglo de eliminación es linealizable en el punto de intercambio (`push(x)` seguido inmediatamente de `pop() → x`), conservando la conservación exacta de elementos y el conteo de nodos del benchmark.

### Plan de validación y ablación
- **Variantes a comparar:** `Autores` (orig.) vs `Autores-v2` vs `Autores-v3a` (solo *backoff* P/E) vs `Autores-v3` (`v2` + *Elimination-Backoff* L2) vs `Anderson` vs `Folly` (y opcionalmente `Anderson + Elimination` como control justo de estructura de datos).
- **Verificación:** ASan + UBSan + TSan (`setarch -R`) sobre conservación de elementos en pila concurrente + `bench_basura` (verificar 0 bloques diferidos) + verificación de que `load()`/`store()` siguen 100 % *wait-free* y con cero regresión en `n = 10` y `n = 10⁶` (porque el arreglo de eliminación solo se activa tras fallo de `CAS`, y cuando $N$ es grande puede omitirse o permanecer frío).
- **Criterio de éxito:** superar a `Anderson` (`2.06 Mop/s` en `n=1, u=50%` y `8.79 Mop/s` en `n=1, u=10%`) sin degradar `n = 10` ni `n = 10⁶`, manteniendo liberación inmediata.

### Resultado empírico medido (`resultados_v3/resumen_v3.md`, 360 ejecuciones × 2 s, 0 fallos)
- **Validación:** ASan+UBSan y TSan (`setarch -R`) pasados con 0 errores, 0 fugas y 0 *data races* (conservación exacta 1:1 de conteo y suma). Topología detectada automáticamente: 6 dominios (8 P-cores + 4 clústeres de 4 E-cores).
- **Victoria en los 6 paneles de la Figura 5 a 24 hilos frente a `Autores`, `Anderson` y `Folly`:**
  - `n = 1, u = 50 %`: `Autores 0.96` → `v2 1.72` → `v3a (backoff) 1.61` → `v3g (genérico) 2.22` → **`Autores-v3 (L2+P/E) 3.66 Mop/s` (`3.81×` sobre original, `+65 %` sobre `v3g`, `+80 %` sobre `Anderson 2.03`)**, con **escalado positivo** al pasar de 12 → 18 → 24 hilos (`2.63 → 3.08 → 3.66`).
  - `n = 1, u = 10 %`: `Autores 4.16` → `v2 6.40` → `v3g 7.42` → **`Autores-v3 9.55 Mop/s` (`2.30×`, supera a `Anderson 8.78`)**.
  - `n = 10, u = 10 %`: **`Autores-v3 73.83 Mop/s` (`1.44×`)**, superando incluso al control justo `Anderson-v3` (`59.70`).
  - `n = 10⁶, u = 50 %`: **`64.09 Mop/s`** (`RSS = 763 MB`, solo +3 MB respecto a `v2` y −29 % respecto a `Autores` original).
- **Control científico justo (`Anderson-v3`):** confirma que la capa L2 también acelera a Anderson en `n = 1, u = 10/50 %` (`12.57` y `5.09`), pero `Autores-v3` vence a `Anderson-v3` en los otros 4 paneles y mantiene 0 bloques diferidos.

### Respaldo bibliográfico
- Hendler, Shavit & Yerushalmi (2004), *SPAA 2004*, DOI `10.1145/1007912.1007944` (y versión revista *JPDC* 2010).
- Liu, Gu, Tang, Li, Zang & Chen (2022), *LibASL*, *PPoPP 2022*, DOI `10.48550/arXiv.2108.03355`.
- Ouyang & Zhu (2022), *Core-aware combining*, *JPDC* 162, DOI `10.1016/j.jpdc.2022.01.001`.
- Schöne, Velten, Hackenberg & Ilsche (2024), *ACM ICPE 2024*, DOI `10.1145/3629526.3645040`.

---

## Propuesta 4 — `Autores-v4`: `compare_and_set` verdaderamente *wait-free* (*Fast-Path / Slow-Path Helping*)

### Debilidad exacta que ataca
- Es la **única limitación teórica explícita** del artículo original (de Wolff et al., Euro-Par 2024, §1.2 y §5.1): mientras `load()` y `store()` completan en un número acotado de pasos ($O(P)$ en recarga, $O(1)$ en caso común), `compare_and_set` sigue siendo solo *lock-free*. Si un flujo continuo de lectores ejecuta `fetch_add(1)` sobre el campo de peso en la palabra de 64 bits, el `CAS` de un escritor puede fallar indefinidamente (*starvation*).

### Diseño técnico propuesto (sobre `Autores-v2`)
- Adoptar el patrón **Fast-Path / Slow-Path Helping** de Kogan & Petrank (*PPoPP 2012*):
  1. **Camino rápido (*fast path*):** idéntico a `Autores-v2` durante los primeros $K$ intentos (p. ej. $K = 8$ o $16$). Costo añadido en caso común: comprobar 1 bit libre de alineación (`HELP_BIT`, bit 0 del puntero alineado) o un contador global de solicitudes lentas (`slow_ops_active == 0`).
  2. **Transición al camino lento (*slow path*):** tras $K$ fallos por contención de lectores, el hilo registra su descriptor `CasOp { expected_cb, new_cb, state }` en una tabla acotada por hilo (`state_array[MAX_THREADS]`) y marca el bit/estado de ayuda.
  3. **Ayuda cooperativa acotada:** cualquier hilo que entra en `load()` o `store()` y detecta `slow_ops_active > 0` ayuda a completar la operación anunciada más antigua (usando un esquema de *round-robin* por fase) antes de proseguir. Al estar acotado el número de hilos $P$, cada `compare_and_set` termina en $O(P)$ pasos → **wait-free en las tres primitivas (`load`, `store`, `compare_and_set`)**.
  4. **Seguridad de memoria del descriptor:** los descriptores residen en un arreglo estático por hilo reutilizable por número de secuencia (o protegidos por el propio mecanismo de conteo ponderado), preservando la propiedad de **liberación inmediata sin epochs**.

### Plan de validación y criterios de éxito
- **Verificación de corrección:** ASan + UBSan + TSan + verificación exhaustiva de *model checking* acotado (GenMC si se instala, o prueba de estrés adversarial con 23 lectores continuos + 1 escritor `CAS`).
- **Métricas:**
  - *Throughput* y latencia de cola (`p99`, `p99.95`, `máx`) del escritor `CAS` bajo ataque adversarial de 23 lectores continuos: en `Autores` original el escritor puede sufrir latencias extremas o inanición; en `Autores-v4` el número de reintentos y la latencia máxima quedan acotados.
  - Sobrecosto en camino rápido (`n = 10`, `n = 10⁶`, `p = 1`): meta `< 3 %` frente a `Autores-v2`.

### Respaldo bibliográfico
- de Wolff et al. (2024), *Euro-Par 2024*, §1.2 y §5.1, DOI `10.1007/978-3-031-69583-4_8`.
- Kogan & Petrank (2012), *«A Methodology for Creating Fast Wait-Free Data Structures»*, *PPoPP 2012*, DOI `10.1145/2370036.2145835`.
- Timnat & Petrank (2014), *«A Practical Wait-Free Simulation for Lock-Free Data Structures»*, *PPoPP 2014*, DOI `10.1145/2555243.2555261`.
- Nikolaev & Ravindran (2024), *Crystalline-W*, *PLDI 2024*, DOI `10.1145/3658851`.
