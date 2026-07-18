# Plan de pruebas del TP (entorno de 3 contenedores)

Documento vivo con las pruebas funcionales del TP, cómo reproducirlas y el
resultado observado. El entorno de ejecución (3 contenedores Docker) está
descripto en [`DOCKER.md`](DOCKER.md).

## Cómo se ejecutan las pruebas

Cada prueba usa un **proceso inicial** distinto (un script de pseudocódigo en
`kernel_memory/scripts/`). Se selecciona con la variable `PROCESO_INICIAL` y,
según el caso, se ajustan `MEMORY_STICK_SIZE` y `SUSPENSION_TIMEOUT`:

```bash
PROCESO_INICIAL=<script.txt> docker compose up --build
# ver la ejecución:
docker compose logs -f cpu       # ciclo de instrucción
docker compose logs -f kernel    # planificación / estados
docker compose logs -f memoria   # memoria / swap / compactación
docker compose down
```

> Los scripts NO admiten comentarios ni líneas en blanco (el parser divide por
> espacios). Direccionamiento: `dirección_lógica = nº_segmento × 256 + offset`
> (`SEGMENT_MAX_SIZE=256`). Registros: `AX/BX/CX/DX` = 1 byte;
> `EAX/EBX/ECX/EDX/SI/DI/PC` = 4 bytes. `MOV_IN` lee desde `SI`, `MOV_OUT`
> escribe en `DI`.

## Matriz de cobertura

| # | Funcionalidad | Script | Estado |
|---|---|---|---|
| 1 | Ciclo base (`NOOP/SET/SUM/EXIT`) | `proceso_inicial.txt` | ✅ |
| 2 | Memoria de usuario (`MEM_ALLOC/FREE`, `MOV_IN/OUT`, `COPY_MEM`) | `proceso_completo.txt` | ✅ |
| 3 | IO `STDOUT` + `SLEEP` (BLOCK) | `proceso_completo.txt` | ✅ |
| 4 | Compactación de memoria | `proceso_compactacion.txt` | ✅ |
| 5 | Swap out **e** in (suspensión/de-suspensión) | `proceso_swap_full.txt` | ✅ (destapó bug, ver abajo) |
| 6 | Planificación RR (quantum + desalojo) | `planif_rr_init.txt` | ✅ |
| 7 | Colas multinivel (CMN) + desalojo entre colas | `planif_multinivel_init.txt` | ✅ |
| 8 | Aritmética/saltos (`SUB`, `JNZ`, bucles) | — | ⬜ pendiente |
| 9 | Sincronización (`MUTEX_CREATE/LOCK/UNLOCK`) | `mutex_init.txt` | ✅ |
| 10 | Multiprocesamiento (varias CPUs en paralelo) | `multicpu_init.txt` | ✅ |
| 11 | Segmentation Fault | — | ⬜ pendiente |
| 12 | Varios `memory_stick` | — | ⬜ pendiente |
| 13 | IO `STDIN` (interactivo) | — | ⬜ pendiente |
| 14 | BSOD / memoria corrupta | — | ⬜ pendiente |

---

## Bug encontrado y corregido

**`kernel_memory` no respondía en `OP_SUSPENDER_PROCESO` ni `OP_FINALIZAR_PROCESO`.**
El `kernel_scheduler` espera confirmación (`recibir_entero`) tras ambos pedidos,
así que quedaba bloqueado con `mutex_pedidos_memoria` tomado de forma permanente:
tras la primera suspensión o finalización, ningún pedido posterior a memoria se
completaba (deadlock), impidiendo el swap-in. Corregido agregando
`enviar_codigo(fd, OP_OK)` en ambos handlers (commit `2763fb5`).

---

## Detalle de las pruebas

### 1. Ciclo de instrucción base
- **Script:** `proceso_inicial.txt` — `NOOP / SET AX 10 / SET BX 5 / SUM AX BX / NOOP / EXIT`
- **Comando:** `PROCESO_INICIAL=proceso_inicial.txt docker compose up --build`
- **Resultado:** el proceso pasa `NEW→READY→EXEC→EXIT`; la CPU hace FETCH de cada
  instrucción desde memoria (otro contenedor) y ejecuta hasta `EXIT`.

### 2 y 3. Memoria de usuario + IO
- **Script:** `proceso_completo.txt` — `MEM_ALLOC`, `MOV_OUT`/`MOV_IN`, `COPY_MEM`,
  `STDOUT`, `SLEEP`, `MEM_FREE`, `EXIT`.
- **Comando:** `PROCESO_INICIAL=proceso_completo.txt docker compose up --build`
- **Resultado:**
  - Escritura y lectura verificadas: se escribe `42` y se lee `42` (DF 0).
  - `COPY_MEM` copia 1 byte de DF 0 → DF 10.
  - `STDOUT` imprime `*` (ASCII 42, = el byte escrito), confirmando coherencia.
  - `SLEEP 2000` bloquea (`EXEC→BLOCK`) y la IO lo devuelve a `READY`.
  - Estados completos: `NEW→READY→EXEC→BLOCK→READY→EXEC→EXIT` con **dos** ciclos
    de BLOCK (uno por `STDOUT`, otro por `SLEEP`).

### 4. Compactación de memoria
- **Script:** `proceso_compactacion.txt` — reserva 5 segmentos de 200 B, escribe
  datos, libera los intermedios y pide un segmento de 256 B.
- **Comando:** `MEMORY_STICK_SIZE=1024 PROCESO_INICIAL=proceso_compactacion.txt docker compose up --build`
- **Resultado:** memoria loguea `## Inicio de compactación` → `## Fin de compactación`
  → `Segmento Creado 5`. **Integridad verificada:** el segmento 2 se reubicó de
  **DF 400 → DF 200**, y la lectura posterior siguió devolviendo el valor correcto
  (`67`), y el segmento 0 su `65`.

### 5. Swap (suspensión / de-suspensión)
- **Scripts:** `proceso_swap_full.txt` (proc0: aloca, escribe 99, `INIT_PROC`,
  `SLEEP 6000`, lee, `EXIT`) + `proceso_liberador.txt` (proc1: 400 `NOOP` + `EXIT`,
  libera memoria para gatillar la de-suspensión).
- **Comando:** `SUSPENSION_TIMEOUT=3000 PROCESO_INICIAL=proceso_swap_full.txt docker compose up --build`
- **Resultado:** ciclo completo
  `EXEC→BLOCK→SUSP_BLOCK` (swap-out: `Escritura del bloque 0`, `Proceso suspendido`)
  `→SUSP_READY→READY` (swap-in: `Lectura del bloque 0`, `Proceso des-suspendido`)
  `→EXEC→EXIT`. El valor `99` sobrevive al viaje a disco y vuelta (integridad de swap).
- **Nota:** con un único proceso la de-suspensión no se dispara (la notificación
  `OP_MEMORIA_DISPONIBLE` llega mientras el proceso todavía está `SUSP_BLOCK`); por
  eso el escenario usa un segundo proceso que libera memoria.

### 6. Planificación Round Robin
- **Scripts:** `planif_rr_init.txt` (crea 2 procesos en la cola 1 con `INIT_PROC ... 1`)
  + `proceso_rr_a.txt` / `proceso_rr_b.txt` (90 `NOOP` + `EXIT` cada uno).
- **Config relevante:** `QUEUES_ALGORITHMS=[FIFO,RR,RR,FIFO]`, `RR_QUANTUM=1500`.
- **Comando:** `PROCESO_INICIAL=planif_rr_init.txt docker compose up --build`
- **Resultado:** los 2 procesos de la cola 1 (RR) **alternan cada ~1.52 s** (≈ quantum),
  con `## (pid) - Desalojado por fin de quantum` en cada ciclo. Secuencia observada:
  `1→2→1→2→1`, cada uno desalojado 2 veces antes de terminar.

### 7. Colas multinivel (CMN) + desalojo entre colas
- **Scripts:** `planif_multinivel_init.txt` (crea proc de prioridad 3) +
  `proceso_baja.txt` (prioridad 3; a mitad hace `INIT_PROC proceso_alta.txt 0`) +
  `proceso_alta.txt` (prioridad 0).
- **Config relevante:** `PLANIFICATION_ALGORITHM=CMN`, `QUEUE_PREEMPTION=TRUE`
  (prioridad = índice de cola; 0 = más prioritaria).
- **Comando:** `PROCESO_INICIAL=planif_multinivel_init.txt docker compose up --build`
- **Resultado:** el proceso de prioridad 3 se está ejecutando y, al aparecer el de
  prioridad 0, es desalojado:
  `## (1) Prioridad: 3 - Desalojado por cola mas prioritaria por el proceso 2 con prioridad 0`.
  El de prioridad 0 corre hasta `EXIT` y **recién entonces** el de prioridad 3 retoma
  y finaliza.

### 9. Sincronización con mutex
- **Scripts:** `mutex_init.txt` (crea 2 procesos) + `proceso_mtx1.txt`
  (`MUTEX_CREATE mtx`, `MUTEX_LOCK mtx`, `SLEEP 3000`, `MUTEX_UNLOCK mtx`, `EXIT`)
  + `proceso_mtx2.txt` (`MUTEX_LOCK mtx`, `NOOP`, `MUTEX_UNLOCK mtx`, `EXIT`).
- **Comando:** `PROCESO_INICIAL=mutex_init.txt docker compose up --build`
- **Resultado (contención real):**
  - PID 1: `Creo el Mutex mtx` → `Toma el Mutex mtx` → `SLEEP` (`EXEC→BLOCK`,
    libera la CPU pero **sigue siendo dueño** del mutex).
  - PID 2: intenta `MUTEX_LOCK mtx` → **se bloquea** (`EXEC→BLOCK`) porque está tomado.
  - PID 1 despierta y hace `MUTEX_UNLOCK`: `Libera el Mutex mtx` → hand-off inmediato
    `## (2) Toma el Mutex mtx` + PID 2 `BLOCK→READY`.
  - El mutex es global por nombre; en `CMN` además aplica herencia de prioridad.

### 10. Multiprocesamiento (varias CPUs)
- **Scripts:** `multicpu_init.txt` (crea 2 procesos) + `multicpu_worker.txt`
  (120 `NOOP` + `EXIT`).
- **Requiere 2 CPUs:** se levanta con el perfil `multicpu` (agrega el contenedor
  `tp-cpu2` con `CPU_ID=1`).
- **Comando:** `PROCESO_INICIAL=multicpu_init.txt docker compose --profile multicpu up --build`
- **Resultado (paralelismo real):** el scheduler registra `## CPU 0 Conectada` y
  `## CPU 1 Conectada`; dos procesos entran a `EXEC` casi simultáneamente y corren
  ~5 s en paralelo. En memoria el FETCH aparece intercalado en los mismos
  milisegundos (p.ej. `PID 1 instr 35` y `PID 2 instr 32` con < 5 ms de diferencia),
  confirmando ejecución concurrente en ambas CPUs.
- **Nota:** `id_cpu = atoi(CPU_ID)`, así que `CPU_ID` debe ser **numérico** y distinto
  por CPU (`0`, `1`, …). Un valor no numérico como `"CPU1"` da `atoi = 0`.

---

## Pendiente

Ver filas ⬜ de la matriz. Próximos objetivos sugeridos por valor:
sincronización con **mutex**, **múltiples CPUs**, **Segmentation Fault** y
**varios memory_stick**.
