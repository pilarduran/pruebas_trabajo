#include <stdio.h>
#include <stdlib.h>
#include <commons/config.h>
#include "cliente.h"
#include "servidor.h"
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <commons/log.h>
#include <utils/protocolo.h>
#include "cliente.h"
#include "servidor.h"
#include <signal.h>

/*
    Modulo Memory Stick: reserva `tamanio` bytes con malloc() y atiende
    lecturas/escrituras sobre ese espacio. Las direcciones que recibe son
    LOCALES (offset dentro de este stick): el que pide (KM o CPU) traduce
    desde la direccion fisica global usando la base que informa el KM.

    Atiende pedidos por dos vias:
      - la conexion que este modulo abre hacia el Kernel Memory
        (suspension, compactacion, STDIN/STDOUT)
      - las conexiones entrantes de las CPUs (MOV_IN / MOV_OUT / COPY_MEM)
*/

t_log* logger = NULL;

static char*    memoria = NULL;
static uint32_t tamanio_memoria = 0;
static int      memory_delay_ms = 0;

// varias conexiones (KM + N CPUs) pueden operar a la vez sobre el buffer
static pthread_mutex_t mutex_memoria = PTHREAD_MUTEX_INITIALIZER;

static bool recibir_int(int fd, void* destino, size_t tam) {
    return recv(fd, destino, tam, MSG_WAITALL) == (ssize_t)tam;
}

static void enviar_codigo(int fd, int codigo) {
    send(fd, &codigo, sizeof(int), 0);
}

// atiende OP_LEER_MEMORIA / OP_ESCRIBIR_MEMORIA hasta que se corte la conexion
static void atender_operaciones(int fd) {
    while (1) {
        int op = 0;
        if (!recibir_int(fd, &op, sizeof(int))) break;

        if (op == OP_LEER_MEMORIA) {
            // [offset u32][tam u32] -> [OP_OK][bytes] | [OP_ERROR]
            uint32_t offset = 0, tam = 0;
            if (!recibir_int(fd, &offset, sizeof(uint32_t))) break;
            if (!recibir_int(fd, &tam,    sizeof(uint32_t))) break;

            if (offset + tam > tamanio_memoria || tam > tamanio_memoria - offset) {
                log_error(logger, "Lectura fuera de rango: offset %u + tam %u > %u", offset, tam, tamanio_memoria);
                enviar_codigo(fd, OP_ERROR);
                continue;
            }

            if (memory_delay_ms > 0) usleep(memory_delay_ms * 1000);

            enviar_codigo(fd, OP_OK);
            pthread_mutex_lock(&mutex_memoria);
            send(fd, memoria + offset, tam, 0);
            pthread_mutex_unlock(&mutex_memoria);

            log_info(logger, "## Lectura de %u bytes", tam);

        } else if (op == OP_ESCRIBIR_MEMORIA) {
            // [offset u32][tam u32][bytes] -> [OP_OK] | [OP_ERROR]
            uint32_t offset = 0, tam = 0;
            if (!recibir_int(fd, &offset, sizeof(uint32_t))) break;
            if (!recibir_int(fd, &tam,    sizeof(uint32_t))) break;

            if (offset + tam > tamanio_memoria) {
                // consumir el contenido igual para no desincronizar el socket
                char* descarte = malloc(tam);
                if (descarte != NULL) {
                recibir_int(fd, descarte, tam);
                free(descarte);
                }
                log_error(logger, "Escritura fuera de rango: offset %u + tam %u > %u", offset, tam, tamanio_memoria);
                enviar_codigo(fd, OP_ERROR);
                continue;
            }

            pthread_mutex_lock(&mutex_memoria);
            bool ok = recibir_int(fd, memoria + offset, tam);
            pthread_mutex_unlock(&mutex_memoria);
            if (!ok) break;

            if (memory_delay_ms > 0) usleep(memory_delay_ms * 1000);

            enviar_codigo(fd, OP_OK);
            log_info(logger, "## Escritura de %u bytes", tam);

        } else {
            log_warning(logger, "Operacion desconocida: %d", op);
        }
    }
}

// pedidos que llegan por la conexion que abrimos hacia el Kernel Memory
static void* atender_kernel_memory(void* arg) {
    int fd = (int)(intptr_t)arg;
    atender_operaciones(fd);
    log_warning(logger, "Kernel Memory se desconecto");
    exit(EXIT_FAILURE);
    return NULL;
}

// conexiones entrantes de CPUs: [HANDSHAKE_CPU][id] -> [HANDSHAKE_OK]
static void* atender_cliente(void* arg) {
    int fd = (int)(intptr_t)arg;

    int codigo = 0, id_cpu = 0;
    if (!recibir_int(fd, &codigo, sizeof(int)) ||
        !recibir_int(fd, &id_cpu, sizeof(int)) ||
        codigo != HANDSHAKE_CPU) {
        log_warning(logger, "Cliente desconocido (codigo %d), se rechaza", codigo);
        enviar_codigo(fd, HANDSHAKE_ERROR);
        close(fd);
        return NULL;
    }

    enviar_codigo(fd, HANDSHAKE_OK);
    log_info(logger, "## CPU %d Conectada", id_cpu);

    atender_operaciones(fd);
    close(fd);
    return NULL;
}

int main(int argc, char* argv[]) {
    signal (SIGPIPE, SIG_IGN);
    if (argc != 3) {
        printf("Uso: ./bin/memory_stick [Archivo Config] [Tamaño]\n");
        return 1;
    }

    tamanio_memoria = (uint32_t) atoi(argv[2]);

    t_config* config = config_create(argv[1]);
    if (config == NULL) {
        printf("Error: no se pudo leer el archivo de configuración.\n");
        return 1;
    }

    char* log_level  = config_get_string_value(config, "LOG_LEVEL");
    memory_delay_ms  = config_get_int_value(config, "MEMORY_DELAY");
    char* puerto     = config_get_string_value(config, "PUERTO");

    // un archivo de log por instancia (puede haber varios sticks por maquina)
    char nombre_log[64];
    snprintf(nombre_log, sizeof(nombre_log), "memory_stick_%s.log", puerto);
    logger = log_create(nombre_log, "MEMORY_STICK", true, log_level_from_string(log_level));
    if (logger == NULL) {
        printf("Error: no se pudo crear el logger.\n");
        return 1;
    }

    // reservar la memoria que representa este stick
    memoria = malloc(tamanio_memoria);
    if (memoria == NULL) {
        log_error(logger, "No se pudo reservar %u bytes", tamanio_memoria);
        return 1;
    }
    log_info(logger, "Memory Stick iniciado con %u bytes", tamanio_memoria);

    char* ip_kernel_memory     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    char* puerto_kernel_memory = config_get_string_value(config, "PUERTO_KERNEL_MEMORY");

    int fd_kernel_memory = conectar_a(ip_kernel_memory, puerto_kernel_memory,
                                      tamanio_memoria, atoi(puerto));
    if (fd_kernel_memory == -1) {
        log_error(logger, "No se pudo conectar al Kernel Memory");
        return 1;
    }
    log_info(logger, "## Conectado a Kernel Memory");

    // el KM manda pedidos por esta misma conexion (suspension, datos, etc.)
    pthread_t hilo_km;
    pthread_create(&hilo_km, NULL, atender_kernel_memory, (void*)(intptr_t)fd_kernel_memory);
    pthread_detach(hilo_km);

    int fd_escucha = iniciar_servidor(puerto);
    if (fd_escucha == -1) {
        log_error(logger, "No se pudo iniciar el servidor en el puerto %s", puerto);
        return 1;
    }

    while (1) {
        int fd_cliente = accept(fd_escucha, NULL, NULL);
        if (fd_cliente != -1) {
            socket_set_nodelay(fd_cliente);
            pthread_t hilo;
            pthread_create(&hilo, NULL, atender_cliente, (void*)(intptr_t)fd_cliente);
            pthread_detach(hilo);
        }
    }

    return 0;
}
