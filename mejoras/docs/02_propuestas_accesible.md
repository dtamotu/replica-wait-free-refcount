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

## Mejora 3 (`Autores-v3`): «Mesas de intercambio rápido en el pasillo» *(Siguiente paso)*

### Qué problema queda por resolver
Después de `v2`, el único lugar donde `Anderson` todavía nos gana (`2.06` vs `1.63 Mop/s`) es cuando los **24 bibliotecarios hacen fila frente a un solo tablón** (`n = 1, u = 50 %`), metiendo (`push`) y sacando (`pop`) libros sin parar. Todos chocan en la misma puerta.

Además, tu procesador Core Ultra 9 285 tiene **dos tipos de trabajadores**:
- **8 núcleos P** (muy rápidos, cada uno en su oficina propia).
- **16 núcleos E** (más tranquilos, organizados en **4 mesitas de 4 compañeros** que comparten caché L2 rapidísima de ~35 ns).

### Cómo funciona la idea
Piensa en qué pasa cuando un bibliotecario llega a **dejar** un libro (`push`) y otro llega al mismo segundo a **llevarse** cualquier libro de arriba (`pop`):
- En vez de que ambos hagan cola y choquen en el tablón central, si fallan una vez en la puerta, se dan vuelta a una **mesa de intercambio local de su propio grupo de 4 núcleos** (*Elimination-Backoff*): el que trae el libro se lo entrega directo en mano al que venía a buscar uno.
- Ambos terminan al instante sin tocar el tablón central ni crear cajas nuevas, y el resultado final de la pila es matemáticamente idéntico (como si uno hubiera puesto el libro y el otro lo hubiera sacado un instante después).
- Además, si hay choque, ajustamos la espera según si el trabajador es P o E para que no se pisen los talones.

### Qué buscamos lograr
Superar a `Anderson` también en el peor escenario de contención extrema (`n = 1, u = 10 %` y `50 %`), sin perder nada en los otros 5 escenarios y manteniendo **cero basura acumulada**.

---

## Mejora 4 (`Autores-v4`): «Ayuda solidaria para que nadie se quede atascado nunca» *(Extensión teórica del paper)*

### Qué problema teórico deja abierto el paper original
El propio paper admite una pequeña trampa en su sección 5.1:
- **Leer (`load`)** y **escribir a la fuerza (`store`)** están 100 % garantizados contra atascos (*wait-free*).
- Pero **cambiar solo si nadie tocó (`compare_and_set`)** NO es *wait-free*: si 23 lectores no paran de entrar a leer el tablón, cada lector mueve el numerito de peso del tablón, y el escritor que intenta cambiar el libro falla una y otra vez — en teoría, podría quedarse intentando para siempre (inanición).

### Cómo funciona la idea (*Fast-Path / Slow-Path Helping*)
1. **Camino rápido (99.9 % de las veces):** el escritor intenta cambiar el tablón normal, igual de rápido que `Autores-v2`.
2. **Botón de ayuda (camino lento):** si falla demasiadas veces seguidas (por ejemplo 8 o 16 veces), anota su pedido en una **pizarra de emergencias** y prende un foquito (1 bit del puntero).
3. **Ayuda cooperativa:** los siguientes bibliotecarios que llegan a leer ven el foquito prendido y, antes de hacer lo suyo, **le ayudan a completar su cambio**. Así nadie puede quedarse atascado para siempre: el algoritmo pasa a ser **100 % *wait-free* en todas sus operaciones** (`load`, `store` y `compare_and_set`).

---

## Resumen en una mirada

| Mejora | Analogía rápida | Qué arregla | Estado |
|---|---|---|---|
| **`Autores-v1`** | Mirar el tablón antes de firmar 6 papeles | Pelea de escritores (`n=1`) | **Hecha (+44 % en `n=1, u=50%`)** |
| **`Autores-v2`** | Pegar la libreta a la tapa del libro | Tráfico de RAM (`n=10⁶`) y tamaño en memoria | **Hecha (+39–81 % en `n=10⁶`, −29 % RSS, +58 % en `n=1`)** |
| **`Autores-v3`** | Mesas locales de intercambio `push`↔`pop` por grupo L2 | Vencer a Anderson en `n=1, u=50%` | **Siguiente** |
| **`Autores-v4`** | Pizarra de ayuda cuando alguien falla muchas veces | Cerrar la última brecha *wait-free* del §5.1 del paper | **Planificada** |
