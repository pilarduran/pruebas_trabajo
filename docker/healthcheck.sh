#!/usr/bin/env bash
# Healthcheck que verifica si un puerto TCP esta en estado LISTEN
# SIN abrir una conexion (para no disparar handshakes fallidos en los modulos).
# Uso: healthcheck.sh <puerto_decimal>
set -u
port_hex=$(printf '%04X' "$1")
grep -qiE ":${port_hex} [0-9A-F]{8}:[0-9A-F]{4} 0A" /proc/net/tcp /proc/net/tcp6 2>/dev/null
