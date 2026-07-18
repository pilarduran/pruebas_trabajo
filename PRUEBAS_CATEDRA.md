# Pruebas oficiales de la cátedra (plug-n-pray-pruebas)

Resultados de correr los scripts del repo
[sisoputnfrba/plug-n-pray-pruebas](https://github.com/sisoputnfrba/plug-n-pray-pruebas)
sobre el entorno de 3 contenedores (ver [`DOCKER.md`](DOCKER.md)).

Los `.prc` están en `kernel_memory/scripts/catedra/`. Como se referencian entre sí
por nombre relativo a `SCRIPTS_BASEPATH`, se corre apuntando ahí:
`SCRIPTS_BASEPATH=./scripts/catedra`.

## Resumen

| Suite | Descripción | Config clave | Resultado |
|---|---|---|---|
| **Memoria Preliminar** | crear/escribir/leer/eliminar segmentos + seg fault | `SEGMENT_MAX_SIZE=128`, stick 256 | ✅ |
| **Planificación Preliminar** | planificación corto plazo + suspensión | 1 CPU, `SUSPENSION_TIMEOUT=15000` | ✅ (destapó bug #2, ver abajo) |
| **PLANI_MEM** | memoria mixta (7 segmentos) | `SEGMENT_MAX_SIZE=128` | ✅ |
| **PCP** | estrés de planificación (con loops infinitos) | 1 CPU | ✅ (comportamiento correcto) |
| **PMP** | memoria + STDIN + sleeps largos + suspensión | `SEGMENT_MAX_SIZE=128` | ✅ (lento por diseño) |
| **PHP** | sincronización con 3 mutex, prioridades 1–5 | 6 colas, `QUEUES_ALGORITHMS` | ✅ mecánica OK (largo por diseño) |

---

## 🐛 Bug #2 encontrado y corregido: de-suspensión nunca se intenta al terminar la IO

Al correr **Planificación Preliminar**, los procesos `PLANI_PRE_1` (que hacen
`SLEEP 20000` con `SUSPENSION_TIMEOUT=15000`) se suspendían y, al terminar la IO,
pasaban a `SUSP_READY` pero **nunca volvían** (quedaban colgados para siempre).

**Causa:** `manejar_fin_io` (en `kernel_scheduler`) movía el proceso a `SUSP_READY`
pero no intentaba de-suspenderlo. La de-suspensión (`intentar_dessuspender_procesos`)
sólo se invocaba ante una notificación `OP_MEMORIA_DISPONIBLE` de memoria, que en un
escenario de pura planificación (sin actividad de memoria) puede no ocurrir nunca.

**Fix:** llamar a `intentar_dessuspender_procesos()` justo después de pasar el proceso
a `SUSP_READY` en `manejar_fin_io`.

**Verificación:** antes del fix, sólo 4/6 procesos terminaban (PID 1 y 2 colgados en
`SUSP_READY`). Después del fix, **6/6 terminan**, con 4 suspensiones y 4 de-suspensiones.
Este bug es el que además hacía que `PMP` (con `SLEEP 60000`) no pudiera completar.

---

## Detalle

### Memoria Preliminar
- **Init:** `MEMORIA_PRE_0.prc` (crea `MEMORIA_PRE_3` prio1, luego `MEMORIA_PRE_1` y
  `MEMORIA_PRE_2` prio2).
- **Comando:**
  ```bash
  SEGMENT_MAX_SIZE=128 MEMORY_STICK_SIZE=256 SCRIPTS_BASEPATH=./scripts/catedra \
    STDIN_FEED=/app/docker/stdin_feed8.txt PROCESO_INICIAL=MEMORIA_PRE_0.prc \
    docker compose up --build
  ```
- **Resultado:**
  - `MEMORIA_PRE_3` → `Segmentation Fault - segmento 0 - desplazamiento 16 - tamanio
    acceso 4 - limite 16` → `SEG_FAULT` (finaliza rápido, según lo esperado).
  - `MEMORIA_PRE_1` → crea 4 segmentos, escribe, libera 0 y 2, aloca seg4 de 128 y lee
    de vuelta correctamente.
  - `MEMORIA_PRE_2` → `STDIN` lee 8 chars (`ABCDEFGH`) y `STDOUT` los imprime.

### Planificación Preliminar
- **Init:** `PLANI_PRE_0.prc` (crea 5 procesos: 2×`PLANI_PRE_1` prio3, 2×`PLANI_PRE_2`
  prio2, 1×`PLANI_PRE_3` prio1).
- **Comando:**
  ```bash
  SCRIPTS_BASEPATH=./scripts/catedra SUSPENSION_TIMEOUT=15000 \
    PROCESO_INICIAL=PLANI_PRE_0.prc docker compose up --build
  ```
- **Resultado:** 6/6 procesos finalizan. Se observan desalojos por fin de quantum (RR),
  desalojos entre colas (multinivel) y **suspensión + de-suspensión** de los
  `PLANI_PRE_1` (`SLEEP 20000` > `SUSPENSION_TIMEOUT`). Requirió el fix del bug #2.

### PLANI_MEM
- **Comando:**
  ```bash
  SEGMENT_MAX_SIZE=128 MEMORY_STICK_SIZE=512 SCRIPTS_BASEPATH=./scripts/catedra \
    PROCESO_INICIAL=PLANI_MEM.prc docker compose up --build
  ```
- **Resultado:** crea 7 segmentos (64/32/16/32/64/32/48), escribe valores 64–70, libera
  el seg0 y lo reutiliza para el seg5, y lee de vuelta los valores correctos (`65`–`70`)
  → integridad de memoria con reutilización de huecos. Finaliza con EXIT.

### PCP (estrés de planificación)
- **Comando:** `SCRIPTS_BASEPATH=./scripts/catedra PROCESO_INICIAL=PCP.prc docker compose up --build`
- **Resultado (correcto):** `PCP_1` (×4) y `PCP_3` terminan en `SET PC 0` → son **bucles
  infinitos intencionales** (procesos CPU-bound que corren para siempre). Sólo finalizan
  `PCP` (init) y los 2 `PCP_2` = **3 procesos**; el resto sigue ciclando. Se observan
  decenas de desalojos por quantum y entre colas, **sin errores ni deadlock**.

### PMP (memoria + STDIN + sleeps largos)
- **Comando:**
  ```bash
  SEGMENT_MAX_SIZE=128 MEMORY_STICK_SIZE=512 SCRIPTS_BASEPATH=./scripts/catedra \
    STDIN_FEED=/app/docker/stdin_feed_multi.txt PROCESO_INICIAL=PMP.prc docker compose up --build
  ```
- **Resultado:** **5/5 procesos finalizan** (verificado), con **6 de-suspensiones** y
  0 errores. Es largo por diseño (`PMP_1` hace `SLEEP 60000`, `PMP_2/3` `SLEEP 30000`):
  `PMP_1` se suspende (60 s > 35 s) y vuelve gracias al fix del bug #2 — este escenario
  es la validación definitiva de dicho fix.

### PHP (sincronización con mutex, prioridades 1–5)
- **Requiere 6 colas** (usa prioridades hasta 5).
- **Comando:**
  ```bash
  QUEUES_ALGORITHMS='[FIFO,RR,RR,RR,RR,FIFO]' SCRIPTS_BASEPATH=./scripts/catedra \
    PROCESO_INICIAL=PHP.prc docker compose up --build
  ```
- **Resultado:** los mutex `MUTEX_1/2/3` se toman y liberan correctamente; los 5
  `PHP_3` serializan sobre `MUTEX_3`. **Sin deadlock** (no hay espera circular entre
  mutex) ni errores. Es un test **muy largo** por los `SLEEP` de 15–30 s combinados con
  la serialización, por lo que su ejecución completa lleva varios minutos.

---

## Variables de entorno agregadas para estas pruebas
- `SEGMENT_MAX_SIZE` — tamaño máximo de segmento (memoria **y** cpu).
- `SCRIPTS_BASEPATH` — carpeta de scripts (`./scripts/catedra`).
- `QUEUES_ALGORITHMS` — override de las colas (p.ej. 6 colas para prioridades 0–5).
- `MEMORY_DELAY` — retardo de acceso físico del memory_stick (para acelerar pruebas).
- `STDIN_FEED` — archivo de entrada para io `STDIN`.

## Estado
Todas las suites del repo de la cátedra corren correctamente en el entorno.
La campaña destapó un **segundo bug real** (de-suspensión), ya corregido.
