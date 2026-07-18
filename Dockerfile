# TP Sistemas Operativos - UTN FRBA
# Imagen que compila so-commons-library + los 6 modulos del TP.
# Cada contenedor levantado desde esta imagen es un "SO" completo:
# los 6 modulos corren adentro comunicandose por 127.0.0.1.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# ---- Dependencias de compilacion ----
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        gcc \
        make \
        git \
        curl \
        ca-certificates \
        libreadline-dev \
    && rm -rf /var/lib/apt/lists/*

# ---- so-commons-library de la catedra ----
RUN git clone https://github.com/sisoputnfrba/so-commons-library /tmp/so-commons \
    && cd /tmp/so-commons \
    && make debug \
    && make install \
    && ldconfig \
    && rm -rf /tmp/so-commons

# ---- Codigo del TP ----
WORKDIR /app
COPY . /app

# ---- Compilacion de cada modulo (utils se compila como dependencia) ----
RUN cd /app/utils            && make \
 && cd /app/kernel_memory    && make \
 && cd /app/kernel_scheduler && make \
 && cd /app/memory_stick     && make \
 && cd /app/swap             && make \
 && cd /app/io               && make \
 && cd /app/cpu              && make

RUN chmod +x /app/docker/entrypoint.sh /app/docker/healthcheck.sh

ENTRYPOINT ["/app/docker/entrypoint.sh"]
