**Dictamen de revisión.** Los CSV sostienen dos resultados fuertes: `v3` recupera rendimiento a 9–12 hilos en el i5 bajo contención extrema, y `v4` mejora el caudal del escritor adversarial en ambos arranques. La atribución del salto a Intel Flex Mode sigue abierta. La afirmación de un impacto universal de las mitigaciones inferior al 0,2 % contradice los CSV. La garantía *wait-free* general de `v4` contradice el código de la ruta lenta.

Ejecuté Python por stdin en modo solo lectura sobre **16 CSV y 2 953 filas**. Recalculé las medianas a partir de las rondas y los cocientes a partir de cifras sin redondear. En el i5, `off → on` compara condiciones con las mismas claves experimentales en **dos arranques**. El CV es la desviación estándar muestral dividida por la media.

| Plataforma y campaña | Rendimiento | Latencia | Adversarial | Basura | Total |
|---|---:|---:|---:|---:|---:|
| i5, **cada** modo | 564 | 108 | 25 | 72 | **769** |
| Ultra 9, tres campañas | 885 | 390 | 50 | 90 | **1 415** |

Las 769 claves del i5 coinciden uno a uno entre modos. El total es **1 538 filas**, frente a las 1 256 declaradas. Todos los `codigo` disponibles son cero. `bench_basura` carece de ese campo.

## A. Hallazgos en los datos crudos

### A.1. En el i5, `v3` toca fondo a 6 hilos y se recupera a 9

Medianas en Mop/s para `n=1, u=50 %`. Cada celda muestra `off → on`. Es el único panel de conjunto pequeño con medidas intermedias a 3, 6 y 9 hilos.

| Variante | 1 hilo | 3 hilos | 6 hilos | 9 hilos | 12 hilos |
|---|---:|---:|---:|---:|---:|
| Autores | 12,7015 → 12,705 | 3,60753 → 3,56087 | 2,83366 → 2,83884 | 2,52967 → 2,50314 | 1,64359 → 1,63622 |
| `v2` | 19,3668 → 19,3158 | 5,18966 → 5,20536 | 3,58427 → 3,57687 | 2,90843 → 2,95437 | 1,79171 → 1,8061 |
| `v3g` | 18,3601 → 18,3678 | 4,57438 → 4,71295 | 3,51947 → 3,45731 | 3,77479 → 3,84363 | 4,32998 → 4,44567 |
| **`v3`** | **18,8985 → 18,9645** | **4,98334 → 4,68496** | **3,12592 → 3,1481** | **5,13925 → 4,92095** | **6,97641 → 6,70602** |
| `v4` | 18,7546 → 18,7794 | 4,11928 → 4,38623 | 3,05938 → 3,11449 | 2,65404 → 2,63922 | 1,76665 → 1,6565 |
| Anderson | 14,5094 → 14,5289 | 4,78621 → 4,78277 | 3,7679 → 3,77534 | 3,79445 → 3,8104 | 3,45982 → 3,45535 |
| Anderson-v3 | 14,2195 → 14,1828 | 5,19115 → 5,11299 | 3,74785 → 3,72147 | 5,38602 → 5,41025 | 6,98126 → 6,96666 |
| Folly | 13,4212 → 13,4246 | 2,64952 → 2,60961 | 1,5883 → 1,60289 | 1,23147 → 1,292 | 0,923552 → 0,917096 |

- **El cruce ocurre entre 6 y 9 hilos.** A 6, `v3` pierde contra `v3g` por **11,18 % / 8,94 %** (`off/on`). A 9, gana por **36,15 % / 28,03 %**. De 6 a 12, `v3` sube **123,18 % / 113,02 %**, mientras `v2` cae **50,01 % / 49,51 %**. A 12, `v3` conserva el **36,92 % / 35,36 %** de su caudal monohilo. El codo coincide con el punto a partir del cual usar SMT resulta inevitable si los trabajadores ocupan CPU lógicas distintas. Para atribuirlo al emparejamiento entre hermanos SMT hace falta registrar la colocación por corrida. [Rendimiento i5 off](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_off/portatil_12h/rendimiento_portatil.csv), [rendimiento i5 on](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_on/portatil_12h/rendimiento_portatil.csv)
- **El detector confunde L2 compartida por SMT con clase E.** Marca `is_p_core=false` cuando la lista L2 contiene dos CPU lógicas y agrupa por esa lista. Con las seis L2 del i5, la salida esperada son seis dominios de dos hermanos, todos con la ventana de **96** iteraciones. El log confirma `dominios=6, p_cores(0..23)=0, e_cores(0..23)=24`. El diagnóstico también recorrió 12 IDs fuera del intervalo en línea `0–11`, que reciben `false` por defecto. `v3g` usa el mismo número de ranuras, **72** iteraciones y elección aleatoria. `v3` cambia a la vez la selección primaria, la espera y el sondeo secundario en otro dominio. El cociente `v3/v3g` mide ese paquete de cambios. [Detector](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/pe_topology.h:85), [intercambiador](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/elimination_table_v3.h:222), [log de topología](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_off/sanitizers.log:33)
- **Anderson-v3 es el control decisivo.** A 12 hilos gana a `v3` en las tres rondas de **cada** arranque, tanto para `u=10 %` como para `u=50 %`. El aparente empate `6,98 / 6,98` del informe es **6,98126 / 6,97641** con `off`. Con `on` la diferencia crece a **6,96666 / 6,70602**. En Ultra 9, `v3` pasa de **2,62621 → 3,07537 → 3,66384** Mop/s de 12 a 24 hilos para `u=50 %`. Para `u=10 %`, a 24 hilos aún queda por debajo de su resultado a 6, **9,55189 frente a 10,4063**. [Rendimiento Ultra 9 v3](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_v3/rendimiento_v3.csv)

### A.2. `PAUSE`, backoff y dispersión entre rondas

Cambios porcentuales de las medianas. Cada tripleta compara `v1p/v1`, `v2p/v2` y `v3a/v2`, respectivamente. Las comparaciones del Ultra se hacen dentro de la campaña correspondiente a cada variante.

| `n, u` | i5 12 hilos, `off` | i5 12 hilos, `on` | Ultra 9, 24 hilos |
|---|---:|---:|---:|
| 1, 1 % | −26,98 / −8,83 / +2,06 | −29,50 / −23,95 / −4,73 | −14,56 / −27,37 / −1,35 |
| 1, 10 % | −10,89 / −12,18 / −1,32 | −9,43 / −10,37 / +7,81 | −23,87 / −25,73 / +4,30 |
| 1, 50 % | −8,26 / −13,12 / +8,89 | −5,47 / −11,94 / +0,83 | −36,65 / −32,71 / −6,43 |
| 10, 1 % | −0,51 / −0,81 / +0,22 | −0,49 / −0,97 / −0,16 | −1,94 / −3,32 / −1,38 |
| 10, 10 % | −4,20 / −6,56 / +0,42 | −4,19 / −6,56 / +0,58 | −15,75 / −22,52 / +1,50 |
| 10⁶, 50 % | −0,53 / −0,60 / −0,62 | −0,47 / −0,08 / −0,004 | +0,009 / −1,91 / +1,86 |

La penalización de `PAUSE` depende de carga y plataforma. En `n=1,u=50 %`, el coste de `v2p` es **11,94–13,12 %** en i5 y **32,71 %** en Ultra 9. Con un hilo hay cambios de signo, por ejemplo `v1p` gana **2,93 %** en el Ultra para `n=1,u=1 %`. En el código, `v3a` espera tras dos fallos **externos** de `compare_exchange_weak`. `compare_and_set` delega directamente a `v2`, y la espera de `v3a` en i5 usa una única clase con tope de **64** iteraciones. Su **+8,89 %** en contención extrema `off` cae a **+0,83 %** en `on`. [Backoff v3a](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/atomic_shared_ptr_v3.h:49), [ablación Ultra 9 v1/v2](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_v1_v2/rendimiento_v1_v2.csv)

En `n=1`, **52 de 96** celdas pareadas de rendimiento cambian más de 0,2 % en valor absoluto. En `n=10` son **6 de 48**. El caso más claro a 12 hilos es `v2` en `n=1,u=1 %`, **23,6181 → 28,8151** Mop/s (**+22,00 %**), mientras Autores pasa de **37,0591 → 33,075** (**−10,75 %**). En ese panel, las rondas `off` de `v1` dan **40,6296 / 29,8641 / 25,2242** Mop/s, con CV **24,77 %**. Los resultados distinguen un `n=10` muy estable de una sola pila sensible al arranque y a la ronda.

### A.3. Adversarial y Figura 6 del i5

Medianas de cinco rondas adversariales, con 11 lectores y un escritor. «Máximo mediano» es la mediana de los cinco máximos por corrida.

| Implementación | Escritor, Kops/s | Lectores, Mop/s | p50 / p99, ns | p99,95, ns | Máximo mediano, ns |
|---|---:|---:|---:|---:|---:|
| Autores | 849,14 → 836,91 | 41,45 → 39,80 | 963 / 2 238 → 978 / 3 346 | 3 322 → 5 903 | 53 651 → 16 738 |
| `v2` | 526,28 → 625,09 | 42,52 → 39,89 | 1 210 / 8 732 → 1 083 / 6 788 | 15 339 → 12 205 | 55 084 → 27 445 |
| **`v4`** | **1 030,73 → 1 445,19** | **24,32 → 27,47** | **766 / 2 097 → 534 / 1 777** | **2 977 → 2 773** | **16 172 → 54 048** |
| Anderson | 768,46 → 791,82 | 23,44 → 23,34 | 1 133 / 2 819 → 1 089 / 2 925 | 4 271 → 4 476 | 9 966 413 → 10 514 575 |
| Folly | 23,96 → 23,41 | 8,29 → 8,40 | 29 259 / 188 630 → 29 007 / 209 873 | 301 609 → 358 931 | 405 108 → 489 429 |

`v4` mejora al activar `on` en caudal del escritor **5/5 rondas** y en p99,95 **5/5**. La mediana del escritor sube **40,21 %**, el p99,95 baja **6,85 %** y el máximo mediano sube **3,34×**. Su peor máximo observado pasa de **67 239 a 218 921 ns**. Frente a `v2`, `v4` gana **1,9585× / 2,3120×** en escritor, con **42,80 % / 31,14 % menos caudal lector**. El contraste mide un intercambio entre escritor y lectores. `v2` además es muy variable en `off`, **72,80–1 170,47 Kops/s**, CV **74,61 %**. [Adversarial i5 off](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_off/portatil_12h/adversarial_portatil.csv), [adversarial i5 on](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_on/portatil_12h/adversarial_portatil.csv)

Figura 6 del i5, `p=12`. Cada par es **p50 / p99,95**, en ns, `off → on`.

| Implementación | `load` | `store` | `store-del` |
|---|---:|---:|---:|
| Autores | 103 / 360 → 77 / 293 | 248 / 654 → 236 / 682 | 231 / 689 → 206 / 530 |
| `v1` | 88 / 266 → 118 / 344 | 234 / 582 → 240 / 638 | 206 / 529 → 227 / 648 |
| `v2` | 130 / 416 → 94 / 265 | 238 / 516 → 223 / 562 | 249 / 669 → 232 / 615 |
| `v4` | 150 / 417 → 133 / 330 | 203 / 508 → 209 / 570 | 220 / 607 → 206 / 575 |
| Anderson | 4 120 / 45 360 → 3 954 / 43 894 | 185 / 554 → 155 / 501 | 239 / 569 → 146 / 409 |
| Folly | 2 025 / 19 131 → 1 978 / 19 134 | 397 / 1 233 → 473 / 1 880 | 491 / 1 722 → 579 / 2 207 |

A `p=1` coinciden los **18 p50 y 18 p99** entre arranques, a la resolución del CSV. Los **20 ns en `load` y 28 ns en `store`** corresponden a la familia Autores. En `store-del`, Anderson tiene p99 de **44 ns** en ambos modos y p99,95 de **20 249 → 20 270 ns**. La cola aparece a un hilo y queda oculta en p99. A `p=12`, `v4` cuesta **15,38 % / 41,49 %** más que `v2` en p50 de `load`, aunque mejora el p50 de `store` y `store-del`. [Latencia i5 off](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_off/portatil_12h/latencia_portatil.csv), [latencia i5 on](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_on/portatil_12h/latencia_portatil.csv)

### A.4. `bench_basura` tiene un pico a 8 hilos

Medianas por columna, `off → on`.

| Hilos | Cota de bloques | Pico de bloques Anderson | Exceso Anderson | Operaciones Anderson | Operaciones `v4` |
|---:|---:|---:|---:|---:|---:|
| 4 | 22 | 7 746 → 7 666 | 7 724 → 7 644 | 26 985 278 → 26 614 915 | 29 559 837 → 34 249 393 |
| 8 | 42 | **12 973 → 9 846** | **12 931 → 9 804** | 25 749 899 → 25 085 102 | 33 124 586 → 35 414 196 |
| 12 | 62 | 9 800 → 9 703 | 9 738 → 9 641 | 22 565 491 → 20 761 927 | 40 734 075 → 34 743 888 |

En Anderson, el pico mediano a 8 hilos supera al de 12 en ambos modos. Con `off`, la diferencia es **3 173 bloques**. A 8 hilos el CV del pico es **12,40 % / 15,72 %**, pese a que el CV de operaciones es **0,48 % / 3,33 %**. El pico de objetos de Anderson permanece por debajo de su cota en cada ronda. `v4` pasa de **+15,86 %** de operaciones entre arranques a 4 hilos a **−14,71 %** a 12, con **32 bloques** de pico mediano en ambos modos a 12. La identidad que cumple el CSV es `exceso_bloques = max(0, pico_bloques − limite_bloques)`. El cero certifica **cero exceso sobre la cota en este banco**. [Basura i5 off](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_off/portatil_12h/basura_portatil.csv), [basura i5 on](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_on/portatil_12h/basura_portatil.csv)

## B. Auditoría de Flex Mode en `n=10⁶`

La diferencia entre arranques **crece gradualmente con el paralelismo**. La columna Δ resume, por hilo, los cambios de las medianas de cada implementación. La última columna cuenta comparaciones por implementación y ronda.

| Hilos | Impl. | Autores, Mop/s | `v2`, Mop/s | Δ mediana [mín., máx.] | CV mediano off → on | Pares con `on > off` |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 13 | 2,79486 → 2,80479 | 3,85092 → 3,85684 | +0,154 % [−1,632; +1,222] | 0,512 % → 0,711 % | 20/39 |
| 3 | 6 | 7,53574 → 7,90492 | 10,2866 → 10,7401 | +3,584 % [−2,938; +4,899] | 0,899 % → 0,593 % | 16/18 |
| 6 | 6 | 13,0719 → 14,2895 | 17,1687 → 19,1767 | +8,779 % [+6,637; +11,696] | 0,512 % → 0,279 % | 18/18 |
| 9 | 6 | 15,3849 → 18,3471 | 18,2773 → 22,3588 | +20,969 % [+17,816; +22,635] | 0,534 % → 0,103 % | 18/18 |
| 12 | 13 | 17,226 → 22,2539 | 19,3692 → 25,1523 | +30,554 % [+28,413; +35,614] | 0,593 % → 0,031 % | 39/39 |

De 6 a 12 hilos, **el mínimo de `on` supera al máximo de `off` en cada implementación medida a cada corte**. A 12 hilos, el CV de `v2` cae de **1,61680 %** a **0,01494 %** y la varianza muestral pasa de **0,099364750** a **0,000014123 (Mop/s)²**. El salto excede la dispersión intrarranque. Las rondas comparten estado de arranque, por lo que esos 75 pares favorables representan evidencia descriptiva de dos arranques.

| Hilos | RSS Autores, KiB | RSS `v2`, KiB | `segundos_reales`, mediana global |
|---:|---:|---:|---:|
| 1 | 1 099 728 → 1 099 704 | 776 768 → 776 752 | 4,41 → 4,41 |
| 3 | 1 099 780 → 1 099 836 | 776 816 → 776 904 | 4,00 → 4,035 |
| 6 | 1 100 152 → 1 100 296 | 777 028 → 777 120 | 3,96 → 3,995 |
| 9 | 1 100 148 → 1 100 100 | 777 424 → 777 396 | 4,015 → 4,08 |
| 12 | 1 100 600 → 1 100 632 | 778 016 → 777 968 | 3,98 → 4,12 |

A 12 hilos, `v2` reduce el RSS **29,30983 % / 29,31625 %** y gana **12,44166 % / 13,02423 %** en Mop/s frente a Autores. A un hilo la ganancia es **37,78579 % / 37,50905 %**. El efecto de `v2` sobre huella es estable. Su ventaja de caudal depende del número de hilos. `segundos_reales` abarca **3,86–5,25 s** en `off` y **3,88–5,28 s** en `on` para `n=10⁶`. A 12 hilos, `v2` pasa de **3,96 a 4,07 s** de tiempo real mientras gana **29,857 %** en Mop/s. Conviene documentar el intervalo cronometrado y desglosar inicialización, trabajo y destrucción.

**Fuerza de la hipótesis.** La asimetría 16+8 permite una zona intercalada de 16 GiB y otra de 8 GiB en un solo canal. El perfil creciente con hilos es compatible con un límite del subsistema de memoria. El RSS mide cantidad residente y el inventario de DIMM describe la configuración física. Para saber qué zona atendió la carga hacen falta direcciones físicas y tráfico por canal. Los **38/19 GB/s** son cifras sin medición local adjunta. La instantánea de `off` muestra **22 GiB disponibles, 873 MiB usados y 0 B de swap**, lo que debilita la explicación de presión de memoria sostenida por la compilación previa. Siguen abiertas THP y compactación, localidad por *first touch*, frecuencia efectiva de núcleo y *uncore*, colocación SMT y actividad de fondo. Con un solo nodo NUMA, `numactl -i all` tampoco selecciona canal. [Entorno i5 off](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_off/entorno_corrida.txt:23), [Intel, organización de memoria en 10.ª generación](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/10th-gen-core-families-datasheet-vol-1-datasheet.pdf), [Linux, contadores y diagnóstico de THP](https://docs.kernel.org/admin-guide/mm/transhuge.html)

**Prueba de 30 s, con la preparación hecha.** En el mismo arranque, preparar dos arenas del mismo tamaño y tamaño de página. Clasificar los marcos físicos de las arenas con `pagemap` y verificar qué canal los atiende con contadores IMC por canal, si están expuestos en esa máquina. Tener las arenas prefaltadas, con afinidad, frecuencia efectiva y semilla controladas. Cronometrar **A–B–B–A de 3 s a 12 hilos**, **A–B de 3 s a un hilo** y **dos sondas de tráfico de 3 s**, dejando 6 s de margen. Usar `bench_stack` con la arena seleccionada, y registrar Mop/s, lecturas/escrituras por canal, fallos de dTLB y `AnonHugePages`. Si el diferencial de alrededor del 30 % sigue a la **arena** a 12 hilos y el tráfico se concentra en un solo canal en la arena lenta, se demuestra el mecanismo para esa repetición. La atribución de los arranques históricos requiere esa misma telemetría o reproducir su diferencia con el control. [Linux, PFN y permisos de `pagemap`](https://docs.kernel.org/admin-guide/mm/pagemap.html), [Intel Performance Counter Monitor](https://github.com/intel/pcm)

## C. Correcciones al informe y al paper

Cotejé programáticamente **517 valores** de las tablas de rendimiento, latencia, adversarial y RSS. **508** coinciden al redondear directamente, **7** son casos de empate decimal sensibles a la convención de redondeo y **2** requieren una corrección numérica. Revisé además las tablas de síntesis, las figuras y las afirmaciones en prosa. `I` designa el informe y `P` el paper.

| Pasaje | Corrección verificable |
|---|---|
| **Muestra y temperatura** | I y P deben indicar **769 + 769** o publicar el filtro que produce 628 por modo. En Ultra hay **42 + 48 = 90** filas de basura, no 84. Los recuentos **885**, **390**, **50** y **360** sí cuadran. `temp_c` abarca **54–71 °C** en i5 y **66–88 °C** en Ultra entre los CSV que lo registran. |
| **`4,26×` del i5 y orden de barras** | El factor directo es **6,97641 / 1,64359 = 4,244617×**, o **4,24×**. `4,26×` sale de dividir cifras previamente redondeadas. Con `on` es **4,098483×**. El **+61,12 %** sobre `v3g` sí cuadra en `off`. En `n=10,u=1 % off`, `v3g` y `v3a` superan a `v2` (**129,029** y **129,077** frente a **128,799**). En `n=10,u=10 % off`, **+45,24 %** corresponde a `v2` y **+42,09 %** a `v3`. En `n=10⁶ on`, `v4` lidera la mediana con **25,2124** frente a **25,1523** de `v2`. |
| **Figura de mitigaciones y procedencia de las tablas i5** | Las tablas de rendimiento usan `off` en los cinco paneles pequeños y `on` en `n=10⁶`. El subpanel adversarial usa `off`. La figura de mitigaciones omite `n=1,u=1 %` y sus propias barras muestran **−2,029 %** para `v2` en `n=1,u=10 %` y **−3,876 %** para `v3` en `n=1,u=50 %`. Rotular por arranque y eliminar «<0,2 % en L3», «doble canal limpio» y la atribución causal a Flex Mode. |
| **Adversarial i5 y Ultra** | En P, Anderson i5 debe figurar como **768,46 Kops/s**, o **768** a entero, frente a **769** en la tabla y figura. Los factores i5 **1,96×**, **43×**, **5,15×** y **616×** sí cuadran para `off` con sus denominadores. En Ultra E, **4,7265×**, **38,4014×** y **89,6089×** se reproducen. El factor contra Folly es **210,00 / 0,26 = 807,69×**. `700×` usa 0,3 redondeado. La mediana de Folly E se apoya en **528 CAS completados** por ronda como mediana, por lo que p99,95 tiene resolución muy limitada. |
| **«Máximo» y «domina»** | En Ultra P, `v4` lidera en escritor y p99,95, con máximo mediano de **684 632 ns** frente a **140 067 ns** de `v2`. En Ultra E, Anderson tiene menor p50 (**5 136 vs. 5 182 ns**). El peor máximo observado de `v4` en E es **2 118 902 ns**, frente a **124 060 ns** como mediana de máximos. Especificar el estadístico y el competidor para cada factor. |
| **Ultra, victoria 6/6 y síntesis** | El **6/6 a 24 hilos** se reproduce contra las medianas de Autores, Anderson y Folly. A 1 hilo la campaña `v3` carece de Folly y `v3` queda por debajo de `v2` en `n=10,u=1 %` (**94,5995 vs. 96,1904**) y en `n=10⁶` (**5,46976 vs. 5,47987**). En la síntesis, **65,9792** de `v2` pertenece a `v1_v2`. En la campaña `v4`, el mismo `v2` da **69,4431** y Autores **49,4363**, fuera del rango publicado **47,3–48,2**. La celda de **413 ns** atribuida a `v3` carece de CSV de latencia para esa variante. |
| **«0 % de sobrecosto» de `v4`** | En Ultra P, p99,95 de `load` a 24 hilos pasa de **1 553 a 1 770 ns** frente a `v2` (**+13,97 %**), aun con p50 de **413 → 410 ns**. En `store-del` monohilo, p50 **27 → 29 ns** (**+7,41 %**). En `bench_stack` monohilo los cocientes de caudal van de **0,9990× a 1,0286×**. A 24 hilos el mayor coste es **0,6232×** en `n=10,u=1 %`. Atribuirlo a `K=4` requiere contar entradas en ruta lenta o barrer `K`. |
| **RSS, `false sharing` y retención** | Ultra `v1_v2` confirma **+80,78 %** a 1 hilo, **+39,47 %** a 24 y **−29,328 %** de RSS. **1 075 → 760** está en **MiB** tras dividir `rss_kb` por 1 024. El bloque de **32 B** requiere identificar el tipo de nodo: el log local registra **40 B** para su nodo de prueba `v2`. El caudal de `n=1,u=1 %` permite plantear *false sharing* como hipótesis. Para `v3`, el RSS mediano Ultra supera al de `v2` en **3 584 KiB**, y falta un CSV `bench_basura` de `v3`. «0 bytes extra» y «0 bloques diferidos» exceden lo medido. |

Los empates de redondeo incluyen `137,075 → 137,07` y `100,345 → 100,34`. Conviene fijar una sola política decimal y calcular factores antes de formatear. Los CSV locales tampoco permiten verificar las **14 158 corridas** de la réplica base, el reparto **9/2/3/1** de afirmaciones, el *ping-pong* **35,0/47,4/67,4/86,3 ns**, el **5,75×** sin afinidad, el control **7×10 s** ni el coste de **≈140 ciclos** de `PAUSE`. El rango **405–421 ns** y el factor **1,04×** sí corresponden a `v2`, `load`, `todos24-P`, de la campaña `v1_v2`. [Informe, resultados i5](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/informe_latex/informe_mejoras_completo.tex:258), [paper, validación cruzada](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/paper/paper_wait_free_hibrido_2026.tex:420), [paper, síntesis](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/paper/paper_wait_free_hibrido_2026.tex:468) :codex-file-citation{path="D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_comparativa/figuras/fig_i5_10400_mitigaciones_y_adversarial.pdf" purpose="source"} :codex-file-citation{path="D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_comparativa/figuras/fig_i5_10400_ablacion_6paneles.pdf" purpose="source"}

**La objeción de corrección más seria está en `v4`.** `rc_try_increment_one()` contiene un bucle CAS sin cota. Un hilo que copia y destruye repetidamente una referencia viva puede cambiar el contador antes de cada CAS de un lector lento, incluso con el puntero estacionario. La cota de ocho intentos externos queda fuera de ese bucle. Además, la ruta lenta vuelve a `fetch_add` si faltan ranuras o se agotan los intentos de validación, con lo que reaparecen escrituras sobre el `tag`. La reclamación usa vectores, listas y barridos. El resultado publicable hoy es la reducción empírica de la interferencia lector→escritor bajo el protocolo medido. El log local registra máximos de **8 y 11 reintentos** en las dos pruebas adversariales instrumentadas, con 10 lectores y 5 000 CAS. Para `v3`, «libre de ABA» también requiere una hipótesis explícita, porque la versión de ranura vuelve a cero después de **2³⁰** incrementos. ASan, UBSan y TSan aportan pruebas dinámicas de regresión. [Ruta lenta y CAS de v4](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/atomic_shared_ptr_v4.h:334), [validación y respaldo](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/atomic_shared_ptr_v4.h:543), [versión de la ranura](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/elimination_table_v3.h:68), [log de estrés](D:/Docu/Cometa_2026B/2026B/wait-free-refcount/actualizacion_i5_10400/mejoras/resultados_i5_10400_mitigations_off/sanitizers.log:47)

## D. Cambios de mayor impacto

1. **Acotar la contribución de `v4` antes de someter el paper.** Publicar caudal de escritor y lectores, mediana de máximos, peor máximo y reintentos por separado. Instrumentar activaciones de ruta lenta, respaldos a `fetch_add` y retiradas efectivas. Barrer `K=2,4,8,16` con un escritor y con escritores concurrentes. Para recuperar una garantía *wait-free*, acotar el incremento de RC, probar que su respaldo conserva el progreso del escritor y formular por separado la garantía de reclamación.
2. **Aislar el efecto SMT de `v3` con una ablación pequeña.** A **6 trabajadores**, comparar **6 núcleos × 1 hilo** con **3 núcleos × 2 hermanos SMT**. Igualar ranuras y ventana, y variar por separado la selección local y el sondeo extra. Registrar emparejamientos locales, remotos y cancelaciones. En `pe_topology.h`, separar identidad del dominio L2 y clase de núcleo, con una clase homogénea explícita.
3. **Publicar el i5 por arranque y generar LaTeX desde los CSV.** Mostrar puntos de ronda y bandas de dispersión. Para estimar un efecto de mitigaciones, alternar modos en varios arranques y tratar el arranque como unidad experimental. Para comprobar *false sharing*, mantener la clase de tamaño y el número de asignaciones, mover el RC a otra línea y medir tráfico de coherencia en la dirección del bloque. Versionar la selección de filas, los denominadores y la unidad de RSS junto a las figuras.