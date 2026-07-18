# Correr el TP en 3 contenedores (distribuido)

Los módulos se reparten en 3 contenedores que se comunican por red:

| Contenedor | IP estática | Módulos |
|---|---|---|
| `tp-memoria` | `172.28.0.11` | `kernel_memory` (:8001), `swap`, `memory_stick` (:8003) |
| `tp-kernel`  | `172.28.0.12` | `kernel_scheduler` (:8002), `io` (STDIN/STDOUT/SLEEP) |
| `tp-cpu`     | `172.28.0.13` | `cpu` |

**Orden de arranque garantizado:** `memoria → kernel → cpu`, mediante
`healthcheck` + `depends_on: condition: service_healthy`. Cada contenedor
además espera a los puertos de los que depende antes de arrancar sus módulos.

> No hace falta Linux ni instalar `so-commons-library`: se compila dentro de
> la imagen.

## Requisitos
- Docker Desktop corriendo.

## Uso
```bash
docker compose up --build          # construye y levanta los 3 en orden
docker compose up --build -d       # en segundo plano
docker compose logs -f cpu         # ver logs de un contenedor
docker compose down                # bajar todo
```

## Por qué IPs estáticas
- `cpu` usa `inet_pton` → solo acepta IPs numéricas (no nombres de host),
  por eso la red usa direcciones fijas en `cpu.config`.
- `kernel_memory` descubre la IP del `memory_stick` con `getpeername`. Por eso,
  dentro del contenedor `memoria`, el `memory_stick` se conecta a `kernel_memory`
  por la IP de red (`172.28.0.11`) y **no** por `127.0.0.1`; así la IP que se le
  reenvía a la CPU es ruteable entre contenedores.

Las IPs se inyectan en runtime (el entrypoint parchea los configs dentro del
contenedor con `sed`). **Los archivos de config del repo quedan intactos.**

## Parámetros (variables de entorno)
- `PROCESO_INICIAL` — script inicial (en `kernel_memory/scripts/`).
- `CPU_ID` — identificador **numérico** de la CPU (`id_cpu = atoi(CPU_ID)`).
- `MEMORY_STICK_SIZE` — tamaño de cada memoria física (bytes).
- `MEMORY_STICKS` — cantidad de `memory_stick` a levantar (default 1; puertos 8003, 8004, …).
- `SUSPENSION_TIMEOUT` — override del timeout de suspensión (ms) para probar swap rápido.
- `STDIN_FEED` — ruta (dentro del contenedor) de un archivo del que io `STDIN` lee la entrada.
- `IO_TYPES` — tipos de IO a levantar (`SLEEP STDIN STDOUT`).

Ejemplos:
```bash
# 2 CPUs en paralelo
PROCESO_INICIAL=multicpu_init.txt docker compose --profile multicpu up --build
# 2 memory_stick
MEMORY_STICKS=2 MEMORY_STICK_SIZE=128 PROCESO_INICIAL=proceso_multistick.txt docker compose up --build
```

Ver la batería completa de pruebas en [`PRUEBAS.md`](PRUEBAS.md).

## Logs
Dentro de cada contenedor en `/app/logs/*.log`, y también por
`docker compose logs`.
