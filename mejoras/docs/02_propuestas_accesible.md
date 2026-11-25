# Guía Accesible de las 4 Mejoras sobre `Autores` (Sin Jerga Innecesaria)

Imagina una biblioteca con **24 bibliotecarios** (los 24 núcleos del Core Ultra 9 285) que comparten un **tablón** donde está escrito «el libro del día» (el puntero atómico compartido). Cada libro tiene una **libreta de préstamos** (el contador de referencias): quien lo toma suma 1; quien lo devuelve resta 1; el último que deja la libreta en 0 recicla el libro al instante.

El algoritmo del paper original (`Autores`) resolvió muy bien cómo evitar que alguien lea un libro ya reciclado, y lo hizo **sin colas de espera** (*wait-free*) y **sin acumular libros viejos en una bodega** (a diferencia de Anderson, que junta ~29 000 libros antes de botarlos). Pero al medirlo en nuestra PC vimos **cuatro problemas muy concretos**. Aquí están las 4 mejoras, explicadas en simple, una por una.

---

## Mejora 1 (`Autores-v1`): «No hagas 6 viajes inútiles antes de mirar el tablón» *(Ya hecha y probada)*

### Qué pasaba mal en el código original
Cuando un bibliotecario quería cambiar el libro del tablón (`compare_and_set`), el código original en C++ hacía dos cosas absurdas en cada intento fallido:
1. **Sacaba fotocopias de los dos libros** (el que esperaba ver y el que quería poner) al pasarlos a la función — eso obligaba a firmar «+1» y luego «−1» en las libretas de ambos libros (4 firmas inútiles).
2. **Anotaba el libro nuevo como «prestado» antes siquiera de mirar el tablón.** Si otro compañero ya había cambiado el tablón un nanosegundo antes, tenía que tachar todo y empezar de cero (2 firmas más).

Total: **6 firmas atómicas desperdiciadas** en cada choque, justo cuando todos los bibliotecarios estaban peleando por la misma libreta.

### Qué cambiamos
- Pasamos los libros **por referencia** (sin fotocopiar).
- **Miramos primero el tablón** antes de tocar la libreta del libro nuevo: si ya cambió, nos damos la vuelta sin escribir nada.
- También probamos si hacer una pequeña pausa (`_mm_pause`) ayudaba cuando hay choque.

### Qué pasó al medirlo en tu PC
- En alta pelea (`n = 1, u = 50 %`), `Autores-v1` fue **+44 % más rápido** (`1.03 → 1.48 Mop/s`).
- Y descubrimos algo muy interesante: **la pausa `_mm_pause` empeoró el rendimiento** (`1.48 → 0.94 Mop/s`), porque en los procesadores Intel nuevos esa pausa es demasiado larga (~140 ciclos) comparada con lo rápido que hablan los núcleos vecinos. Así que la quitamos.

---

## Mejora 2 (`Autores-v2`): «Pega la libreta a la tapa del libro» *(Ya hecha y probada)*

### Qué pasaba mal en el código original
Todos los rivales (`Anderson`, `Folly`, `Williams`, `Libstdc++`) —y hasta la versión en Rust de los propios autores— guardan la **libreta pegada a la tapa del libro** (una sola caja en la memoria).

En cambio, el código C++ original de `Autores` guardaba el libro en un estante y la libreta en **otro estante distinto**. Cada vez que creabas un nodo tenías que pedir **dos cajas separadas**, y cada vez que leías tenías que caminar dos veces por la memoria: primero al tablón, luego a la libreta, y recién de ahí al libro. Con un millón de pilas (`n = 10⁶`), ese doble viaje colapsaba el tráfico hacia la memoria RAM.

### Qué cambiamos
- Juntamos el libro y su libreta en **una sola caja** (*in-place*), sin piezas extra innecesarias.
- El boleto que lleva cada bibliotecario (`shared_ptr`) bajó de **16 bytes a 8 bytes** (la mitad de tamaño).
- Al leer (`load`), el bibliotecario ya no tiene que abrir la libreta para averiguar dónde está el libro: están juntos.

### Qué pasó al medirlo en tu PC
- **Resolvió el misterio de `n = 10⁶`:** `Autores-v2` saltó de **3.13 → 5.65 Mop/s (+81 %)** con 1 hilo y de **47.31 → 65.98 Mop/s (+39 %)** con 24 hilos, **superando en +30 % a Anderson y Libstdc++ sin agregar más RAM**, y usando **29 % menos memoria** (`1 075 MB → 760 MB`).
- **En pelea extrema (`n = 1, u = 50 %`):** subió **+58 %** (`1.03 → 1.63 Mop/s`), venciendo ya a `Folly` en los **6 gráficos del paper** y quedando muy cerca de `Anderson` (`2.06`), pero con **cero basura retenida**.
- **Bonus — resolvimos el misterio de los saltos de latencia:** descubrimos que cuando Linux ponía al hilo medido en un **núcleo P (rápido)** tardaba **~414 ns**, y cuando lo ponía en un **núcleo E (ahorrador)** saltaba a **~1 400 ns**. Al fijar el hilo en un núcleo P, el ruido desapareció y `Autores-v2` ganó en lectura, escritura y borrado.

---

## Mejora 3 (`Autores-v3`): «Mesas de intercambio rápido en el pasillo» *(Ya hecha y probada)*

### Qué problema atacaba
Después de `v2`, el único lugar donde `Anderson` todavía nos gana (`2.06` vs `1.63 Mop/s`) es cuando los **24 bibliotecarios hacen fila frente a un solo tablón** (`n = 1, u = 50 %`), metiendo (`push`) y sacando (`pop`) libros sin parar. Todos chocan en la misma puerta.

Además, tu procesador Core Ultra 9 285 tiene **dos tipos de trabajadores**:
- **8 núcleos P** (muy rápidos, cada uno en su oficina propia).
- **16 núcleos E** (más tranquilos, organizados en **4 mesitas de 4 compañeros** que comparten caché L2 rapidísima de ~35 ns).

### Cómo funciona la idea
Piensa en qué pasa cuando un bibliotecario llega a **dejar** un libro (`push`) y otro llega al mismo segundo a **llevarse** cualquier libro de arriba (`pop`):
- En vez de que ambos hagan cola y choquen en el tablón central, si fallan una vez en la puerta, se dan vuelta a una **mesa de intercambio local de su propio grupo de 4 núcleos** (*Elimination-Backoff*): el que trae el libro se lo entrega directo en mano al que venía a buscar uno.
- Ambos terminan al instante sin tocar el tablón central ni crear cajas nuevas, y el resultado final de la pila es matemáticamente idéntico (como si uno hubiera puesto el libro y el otro lo hubiera sacado un instante después).
- Además, si hay choque, ajustamos la espera según si el trabajador es P o E para que no se pisen los talones.

### Qué pasó al medirlo en tu PC
- **En pelea extrema (`n = 1, u = 50 %`, 24 hilos):** `Autores-v3` saltó de **0.96 → 3.66 Mop/s (`3.81×` más rápido que el original y `+80 %` por encima de Anderson `2.03`)**. Y lo más llamativo: las mesas agrupadas por clúster L2 fueron **+65 % más rápidas** que mesas aleatorias (`2.22 Mop/s`), logrando que el rendimiento **suba** al añadir más núcleos (`12 → 18 → 24` hilos) en vez de desplomarse.
- **Victoria 6 de 6:** `Autores-v3` supera ahora a `Autores`, `Anderson` y `Folly` en los **6 gráficos del paper** (a 1 y 24 hilos), sin empeorar el consumo de memoria (`763 MB` en `n = 10⁶`) y manteniendo **cero basura diferida**.

---

## Mejora 4 (`Autores-v4`): «Lectura silenciosa para que nadie se quede atascado nunca» *(Ya hecha y probada)*

### Qué problema teórico dejaba abierto el paper original
El propio paper admite una pequeña trampa en su sección 5.1:
- **Leer (`load`)** y **escribir a la fuerza (`store`)** están 100 % garantizados contra atascos (*wait-free*).
- Pero **cambiar solo si nadie tocó (`compare_and_set`)** NO es *wait-free*: si 23 lectores no paran de entrar a leer el tablón, cada lector mueve el numerito de peso del tablón, y el escritor que intenta cambiar el libro falla una y otra vez — especialmente si el escritor trabaja en un **núcleo E (lento)** mientras **8 núcleos P (rápidos)** leen sin parar.

### Cómo funciona lo que implementamos (*Fast-Path / Slow-Path* híbrido)
1. **Camino rápido (99.9 % del tiempo):** todo funciona exactamente igual que `Autores-v2` (sin sobrecosto con 1 hilo y 0 basura diferida).
2. **Alarma de lector silencioso (camino lento):** si el escritor falla 4 veces seguidas solo porque los lectores están moviendo el numerito del tablón, prende una banderita (`slow_writers`).
3. **Lectores sin tocar el tablón:** al ver la banderita, los lectores dejan de tocar el tablón y anotan el libro en su **ficha personal** antes de leerlo. Como el numerito del tablón deja de moverse, el escritor completa su cambio al instante — y si el escritor se detuviera, ningún lector se queda esperando.

### Qué pasó al medirlo en tu PC (ataque de 23 lectores vs 1 escritor CAS)
- **Escritor en núcleo E (ataque híbrido extremo):** `Folly` prácticamente se congela (**0.3 Kops/s**, picos de **25 ms**), `Autores-v2` cae a **44.4 Kops/s** (`p99.95 = 549 µs`), `Anderson` sufre picos de **11.1 ms**, y **`Autores-v4`** salta a **210.0 Kops/s (`4.7×` sobre `v2`)**, baja `p99.95` a **14.3 µs (`38×` menos)** y el peor pico a **124 µs (`90×` menos que Anderson)**, manteniendo **0 bloques diferidos** en `bench_basura`.
- **Compromiso honesto:** a 1 hilo tiene **0 % sobrecosto**, mientras a 24 hilos en cargas de lectura pura (`n = 10, u = 1 %`) pagar esta protección reduce un poco el máximo (`85` vs `137 Mop/s`, aún por encima de Anderson `71` y Folly `51`).

---

## Resumen en una mirada

| Mejora | Analogía rápida | Qué arregla | Estado |
|---|---|---|---|
| **`Autores-v1`** | Mirar el tablón antes de firmar 6 papeles | Pelea de escritores (`n=1`) | **Hecha (+44 % en `n=1, u=50%`)** |
| **`Autores-v2`** | Pegar la libreta a la tapa del libro | Tráfico de RAM (`n=10⁶`) y tamaño en memoria | **Hecha (+39–81 % en `n=10⁶`, −29 % RSS, +58 % en `n=1`)** |
| **`Autores-v3`** | Mesas locales de intercambio `push`↔`pop` por grupo L2 | Vencer a Anderson en `n=1, u=50%` (6 de 6) | **Hecha (`3.81×` en `n=1, u=50%`, escalado positivo)** |
| **`Autores-v4`** | Lectura silenciosa cuando un escritor sufre inanición | Cerrar la brecha de inanición §5.1 del paper | **Hecha (`4.7×` throughput, `38×` menos `p99.95` en ataque E-core)** |
