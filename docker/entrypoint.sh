#!/usr/bin/env bash
# ============================================================================
# Entrypoint por ROL. Cada contenedor corre solo los modulos que le tocan.
# Los configs se parchean en runtime con las IPs estaticas de la red docker
# (los archivos del repo quedan intactos: se editan solo dentro del contenedor).
#
#   ROLE=memoria -> kernel_memory + memory_stick + swap
#   ROLE=kernel  -> kernel_scheduler + io
#   ROLE=cpu     -> cpu
# ============================================================================
set -u

ROLE="${ROLE:?Falta la variable ROLE (memoria|kernel|cpu)}"
IP_MEMORIA="${IP_MEMORIA:-127.0.0.1}"
IP_KERNEL="${IP_KERNEL:-127.0.0.1}"

LOG_DIR=/app/logs
mkdir -p "$LOG_DIR"
PIDS=()

# Reemplaza el valor de una CLAVE=valor dentro de un archivo de config.
set_cfg() { # $1=archivo $2=clave $3=valor
  sed -i "s|^$2=.*|$2=$3|" "$1"
}

# Espera a que un puerto LOCAL entre en estado LISTEN, leyendo /proc/net/tcp
# (sin abrir conexiones, para no ensuciar los logs con handshakes fallidos).
# El orden ENTRE contenedores ya lo garantiza depends_on:service_healthy.
wait_local() { # $1=puerto $2=nombre
  local port_hex; port_hex=$(printf '%04X' "$1")
  echo ">> Esperando a $2 (local :$1)..."
  for _ in $(seq 1 60); do
    if grep -qiE ":${port_hex} [0-9A-F]{8}:[0-9A-F]{4} 0A" /proc/net/tcp /proc/net/tcp6 2>/dev/null; then
      echo ">> $2 escuchando."; return 0
    fi
    sleep 1
  done
  echo ">> TIMEOUT esperando a $2 (:$1)"; return 1
}

launch() { # $1=nombre $2=dir  $3...=comando
  local nombre="$1"; shift
  local dir="$1"; shift
  echo ">> Iniciando ${nombre}: $*"
  ( cd "$dir" && stdbuf -oL -eL "$@" ) > >(stdbuf -oL sed "s/^/[${nombre}] /" | tee -a "$LOG_DIR/${nombre}.log") 2>&1 &
  PIDS+=("$!")
}

# Igual que launch pero redirige stdin desde un archivo (para probar io STDIN).
launch_stdin() { # $1=nombre $2=dir $3=archivo_stdin  $4...=comando
  local nombre="$1"; shift
  local dir="$1"; shift
  local feed="$1"; shift
  echo ">> Iniciando ${nombre} (stdin<${feed}): $*"
  ( cd "$dir" && stdbuf -oL -eL "$@" < "$feed" ) > >(stdbuf -oL sed "s/^/[${nombre}] /" | tee -a "$LOG_DIR/${nombre}.log") 2>&1 &
  PIDS+=("$!")
}

shutdown() {
  echo ">> Deteniendo modulos de [$ROLE]..."
  for pid in "${PIDS[@]}"; do kill "$pid" 2>/dev/null; done
  wait 2>/dev/null
  exit 0
}
trap shutdown SIGTERM SIGINT

case "$ROLE" in
  # --------------------------------------------------------------------------
  memoria)
    # memory_stick se conecta a kernel_memory por la IP DE RED (no 127.0.0.1)
    # para que getpeername reporte una IP ruteable que la CPU pueda alcanzar.
    set_cfg /app/memory_stick/config.txt IP_KERNEL_MEMORY "$IP_MEMORIA"
    set_cfg /app/swap/swap.config        IP_KERNEL_MEMORY "$IP_MEMORIA"

    launch "kernel_memory" /app/kernel_memory ./bin/kernel_memory kernel_memory.config
    wait_local 8001 "kernel_memory"

    # Uno o varios memory_stick (MEMORY_STICKS, default 1). Cada stick escucha en
    # un puerto propio (8003, 8004, ...) con su config generado al vuelo.
    NUM_STICKS="${MEMORY_STICKS:-1}"
    for i in $(seq 1 "$NUM_STICKS"); do
      puerto=$((8002 + i))
      cp /app/memory_stick/config.txt "/app/memory_stick/config_${i}.txt"
      set_cfg "/app/memory_stick/config_${i}.txt" PUERTO "$puerto"
      launch "memory_stick_${i}" /app/memory_stick ./bin/memory_stick "config_${i}.txt" "${MEMORY_STICK_SIZE:-4096}"
    done

    launch "swap"         /app/swap         ./bin/swap swap.config
    ;;

  # --------------------------------------------------------------------------
  kernel)
    # kernel_scheduler apunta a la memoria (otro contenedor). io queda local.
    set_cfg /app/kernel_scheduler/kernel_scheduler.config IP_MEMORY "$IP_MEMORIA"
    set_cfg /app/io/io.config IP_KERNEL_SCHEDULER 127.0.0.1
    # Override opcional del timeout de suspension (para probar swap mas rapido).
    if [ -n "${SUSPENSION_TIMEOUT:-}" ]; then
      set_cfg /app/kernel_scheduler/kernel_scheduler.config SUSPENSION_TIMEOUT "$SUSPENSION_TIMEOUT"
      echo ">> SUSPENSION_TIMEOUT override = ${SUSPENSION_TIMEOUT} ms"
    fi

    # (memoria ya esta lista: garantizado por depends_on:service_healthy)
    launch "kernel_scheduler" /app/kernel_scheduler ./bin/kernel_scheduler kernel_scheduler.config "${PROCESO_INICIAL:-proceso_inicial.txt}"
    wait_local 8002 "kernel_scheduler"
    for t in ${IO_TYPES:-SLEEP STDIN STDOUT}; do
      if [ "$t" = "STDIN" ] && [ -n "${STDIN_FEED:-}" ] && [ -f "${STDIN_FEED}" ]; then
        launch_stdin "io_STDIN" /app/io "$STDIN_FEED" ./bin/io io.config STDIN
      else
        launch "io_${t}" /app/io ./bin/io io.config "$t"
      fi
    done
    ;;

  # --------------------------------------------------------------------------
  cpu)
    # cpu apunta al scheduler y a la memoria (ambos en otros contenedores).
    set_cfg /app/cpu/configs/cpu.config KERNEL_SCHEDULER_IP "$IP_KERNEL"
    set_cfg /app/cpu/configs/cpu.config KERNEL_MEMORY_IP    "$IP_MEMORIA"

    # (memoria y kernel ya estan listos: garantizado por depends_on:service_healthy)
    launch "cpu" /app/cpu ./bin/cpu configs/cpu.config "${CPU_ID:-0}"
    ;;

  *)
    echo "ROLE invalido: $ROLE (esperado: memoria|kernel|cpu)"; exit 1;;
esac

echo ">> [$ROLE] modulos iniciados."
wait
