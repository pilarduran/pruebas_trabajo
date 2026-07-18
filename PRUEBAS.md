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
| 8 | Aritmética/saltos (`SUB`, `JNZ`, bucles) | `proceso_bucle.txt` | ✅ |
| 9 | Sincronización (`MUTEX_CREATE/LOCK/UNLOCK`) | `mutex_init.txt` | ✅ |
| 10 | Multiprocesamiento (varias CPUs en paralelo) | `multicpu_init.txt` | ✅ |
| 11 | Segmentation Fault | `proceso_segfault.txt` | ✅ |
| 12 | Varios `memory_stick` | `proceso_multistick.txt` | ✅ |
| 13 | IO `STDIN` | `proceso_stdin.txt` | ✅ |
| 14 | BSOD / memoria corrupta | `proceso_bsod.txt` | ✅ |

**Cobertura: 14/14 ✅**

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

### 8. Aritmética y saltos (bucle `SUB`/`JNZ`)
- **Script:** `proceso_bucle.txt` — `SET AX 5 / SET BX 1 / SUB AX BX / JNZ AX 2 / EXIT`.
- **Comando:** `PROCESO_INICIAL=proceso_bucle.txt docker compose up --build`
- **Resultado:** bucle correcto de 5 iteraciones: `SUB` deja `AX` en 4,3,2,1,0; `JNZ AX 2`
  salta a la instrucción 2 mientras `AX != 0` (4 saltos) y al llegar a 0 cae en `EXIT`.

### 11. Segmentation Fault
- **Script:** `proceso_segfault.txt` — `SET AX 1 / SET DI 0 / MOV_OUT AX / EXIT`
  (accede al segmento 0 **sin** haberlo alocado).
- **Comando:** `PROCESO_INICIAL=proceso_segfault.txt docker compose up --build`
- **Resultado:** la CPU detecta `Segmentation Fault - el segmento 0 no existe` y el
  proceso `finalizo su ejecucion con motivo de SEG_FAULT`.

### 12. Varios memory_stick
- **Script:** `proceso_multistick.txt` — `MEM_ALLOC 0 200`, escribe `11` en DF 0 y `22`
  en DF 150, y los lee de vuelta.
- **Requiere 2+ sticks:** variable `MEMORY_STICKS` (default 1).
- **Comando:** `MEMORY_STICKS=2 MEMORY_STICK_SIZE=128 PROCESO_INICIAL=proceso_multistick.txt docker compose up --build`
- **Resultado:** se conectan 2 sticks de 128 B (total 256). El segmento de 200 B abarca
  ambos: la escritura en DF 0 la atiende `memory_stick_1` y la de DF 150 (>128) la atiende
  `memory_stick_2`. Las lecturas devuelven `11` y `22` → routing de direcciones entre
  sticks correcto.

### 13. IO STDIN
- **Script:** `proceso_stdin.txt` — `MEM_ALLOC 0 32`, `STDIN` de 5 chars a DF 0, `STDOUT`
  de esos 5 chars.
- **Entrada:** el io `STDIN` lee de un archivo vía `STDIN_FEED` (el entrypoint redirige su
  stdin). El feed por defecto (`docker/stdin_feed.txt`) contiene `HELLO`.
- **Comando:** `STDIN_FEED=/app/docker/stdin_feed.txt PROCESO_INICIAL=proceso_stdin.txt docker compose up --build`
- **Resultado:** io `STDIN` lee `HELLO`, se escribe en memoria, y io `STDOUT` lo imprime
  (`## PID: 0 - HELLO`) → round-trip STDIN → memoria → STDOUT verificado.

### 14. BSOD / memoria corrupta
- **Script:** `proceso_bsod.txt` — 400 `NOOP` + `EXIT` (proceso vivo durante la prueba).
- **Comando:**
  ```bash
  PROCESO_INICIAL=proceso_bsod.txt docker compose up -d --build
  sleep 6
  docker exec tp-memoria pkill -9 -f "bin/memory_stick"   # matar el stick en caliente
  ```
- **Resultado:** kernel_memory detecta `Memory Stick desconectado - Memoria corrupta` y
  notifica al scheduler, que dispara `## BSOD - Memoria corrupta detectada` y finaliza los
  procesos `con motivo de BSOD`.

---

## Estado

**Las 14 funcionalidades de la matriz fueron probadas end-to-end (14/14 ✅).**

Casos borde adicionales que se podrían sumar más adelante: prioridad heredada en
mutex bajo CMN de forma explícita, `INIT_PROC` con prioridad fuera de rango
(`lista_ready_invalida`), y `MEM_ALLOC` mayor a `SEGMENT_MAX_SIZE`.
