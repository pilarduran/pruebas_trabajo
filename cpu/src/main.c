#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <commons/config.h>
#include <commons/log.h>
#include <utils/protocolo.h>
#include "cpu.h"
#include "ciclo_instruccion.h"
#include "contexto.h"
#include "memoria.h"

// Variables globales
t_log* logger;
int socket_ks = -1;
int socket_km = -1;

uint32_t tam_max_segmento;

// -------------------------------------------------------------
// FUNCIONES DE SERIALIZACION
// -------------------------------------------------------------
static bool enviar_exactamente_socket(int socket_fd, const void* buffer, size_t tamanio) {
    size_t enviados_totales = 0;

    while (enviados_totales < tamanio) {
        ssize_t enviados = send(
            socket_fd,
            (const char*) buffer + enviados_totales,
            tamanio - enviados_totales,
            0
        );

        if (enviados <= 0) {
            return false;
        }

        enviados_totales += enviados;
    }

    return true;
}

static bool recibir_exactamente_socket(int socket_fd, void* buffer, size_t tamanio) {
    size_t recibidos_totales = 0;

    while (recibidos_totales < tamanio) {
        ssize_t recibidos = recv(
            socket_fd,
            (char*) buffer + recibidos_totales,
            tamanio - recibidos_totales,
            0
        );

        if (recibidos <= 0) {
            return false;
        }

        recibidos_totales += recibidos;
    }

    return true;
}

bool enviar_entero(int socket, int valor) {
    return enviar_exactamente_socket(socket, &valor, sizeof(int));
}

bool recibir_entero(int socket, int* valor_recibido) {
    return recibir_exactamente_socket(socket, valor_recibido, sizeof(int));
}

bool enviar_string(int socket, const char* string) {
    int tamanio = strlen(string);

    if (!enviar_entero(socket, tamanio)) {
        return false;
    }

    return enviar_exactamente_socket(socket, string, tamanio);
}

char* recibir_string(int socket) {
    int tamanio = 0;

    if (!recibir_entero(socket, &tamanio)) {
        return NULL;
    }

    if (tamanio < 0 || tamanio > 10000) {
        return NULL;
    }

    char* string = malloc(tamanio + 1);

    if (string == NULL) {
        return NULL;
    }

    if (!recibir_exactamente_socket(socket, string, tamanio)) {
        free(string);
        return NULL;
    }

    string[tamanio] = '\0';
    return string;
}

// -------------------------------------------------------------
// CONEXION
// -------------------------------------------------------------

int conectar(char* ip, int puerto) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (socket_fd == -1) {
        return -1;
    }

    struct sockaddr_in servidor = {
        .sin_family = AF_INET,
        .sin_port = htons(puerto)
    };

    inet_pton(AF_INET, ip, &servidor.sin_addr);

    int resultado = connect(socket_fd, (struct sockaddr*)&servidor, sizeof(servidor));

    if (resultado == -1) {
        printf("Error: no se pudo conectar a %s:%d\n", ip, puerto);
        close(socket_fd);
        return -1;
    }

    socket_set_nodelay(socket_fd); // desactiva la espera para enviar muchos paquetes chiquitos todo junto, envia al instante


    return socket_fd;
}

// -------------------------------------------------------------
// HANDSHAKES
// -------------------------------------------------------------

static bool hacer_handshake_con_scheduler(char* identificador) {
    enviar_entero(socket_ks, HANDSHAKE_CPU);

    int id_cpu = atoi(identificador);
    enviar_entero(socket_ks, id_cpu);

    int respuesta = 0;

    if (!recibir_entero(socket_ks, &respuesta)) {
        log_error(logger, "## No se pudo recibir respuesta de handshake del Kernel Scheduler");
        return false;
    }

    if (respuesta != HANDSHAKE_OK) {
        log_error(logger, "## Handshake rechazado por Kernel Scheduler");
        return false;
    }

    log_info(logger, "## Conectado a Kernel Scheduler como CPU %d", id_cpu);
    return true;
}

static bool hacer_handshake_con_memoria(char* identificador) {
    enviar_entero(socket_km, HANDSHAKE_CPU);

    int id_cpu = atoi(identificador);
    enviar_entero(socket_km, id_cpu);

    int respuesta = 0;

    if (!recibir_entero(socket_km, &respuesta)) {
        log_error(logger, "## No se pudo recibir respuesta de handshake del Kernel Memory");
        return false;
    }

    if (respuesta != HANDSHAKE_OK) {
        log_error(logger, "## Handshake rechazado por Kernel Memory");
        return false;
    }

    log_info(logger, "## Conectado a Kernel Memory como CPU %d", id_cpu);
    return true;
}

// -------------------------------------------------------------
// FIN DE EJECUCION DE UN PID
// -------------------------------------------------------------

static void avisar_fin_a_scheduler(int pid, t_motivo_desalojo motivo) {
    switch (motivo) {
        case CPU_MOTIVO_EXIT:
            guardar_contexto(pid);
            enviar_entero(socket_ks, OP_PROCESO_EXIT);
            enviar_entero(socket_ks, pid);
            break;

        case CPU_MOTIVO_FIN_QUANTUM:
            guardar_contexto(pid);
            enviar_entero(socket_ks, OP_PROCESO_FIN_QUANTUM);
            enviar_entero(socket_ks, pid);
            break;

        case CPU_MOTIVO_SEG_FAULT:
            guardar_contexto(pid);
            enviar_entero(socket_ks, OP_PROCESO_SEG_FAULT);
            enviar_entero(socket_ks, pid);
            break;

        case CPU_MOTIVO_SYSCALL_BLOQUEANTE:
            /*
                No mandamos nada mas.

                Para syscalls bloqueantes, la CPU ya aviso antes:

                    OP_PROCESO_SYSCALL
                    PID
                    TIPO_SYSCALL
                    PARAMETROS

                El Scheduler ya se encarga de pasar EXEC -> BLOCK.
            */
            break;

        case CPU_MOTIVO_ERROR:
            /*
                Por ahora, ante error de CPU, avisamos EXIT para no dejar
                al Scheduler esperando eternamente.

                Mas adelante podemos diferenciar mejor el motivo.
            */
            guardar_contexto(pid);
            enviar_entero(socket_ks, OP_PROCESO_EXIT);
            enviar_entero(socket_ks, pid);
            break;

        case CPU_MOTIVO_CONTINUAR:
            break;
    }
}

// -------------------------------------------------------------
// MAIN
// -------------------------------------------------------------

int main(int argc, char* argv[]) {

    signal(SIGPIPE, SIG_IGN);
    
    if (argc != 3) {
        printf("Uso: ./bin/cpu [archivo_config] [identificador]\n");
        return 1;
    }

    char* archivo_config = argv[1];
    char* identificador  = argv[2];

    t_config* config = config_create(archivo_config);

    if (config == NULL) {
        printf("Error: no se pudo abrir el archivo de configuracion\n");
        return 1;
    }

    char* log_level = config_get_string_value(config, "LOG_LEVEL");
    char* ks_ip     = config_get_string_value(config, "KERNEL_SCHEDULER_IP");
    int   ks_port   = config_get_int_value(config, "KERNEL_SCHEDULER_PORT");
    char* km_ip     = config_get_string_value(config, "KERNEL_MEMORY_IP");
    int   km_port   = config_get_int_value(config, "KERNEL_MEMORY_PORT");

    tam_max_segmento = (uint32_t) config_get_int_value(config, "SEGMENT_MAX_SIZE");

    /*
        Evita que falle log_create si la carpeta logs no existe.
    */
    mkdir("logs", 0777);

    char nombre_log[100];
    snprintf(nombre_log, sizeof(nombre_log), "logs/%s.log", identificador);

    logger = log_create(nombre_log, "CPU", true, log_level_from_string(log_level));

    if (logger == NULL) {
        printf("Error: no se pudo crear el logger\n");
        config_destroy(config);
        return 1;
    }

    log_info(logger, "## Iniciando CPU: %s", identificador);

    // ---------------------------------------------------------
    // Conexion con Kernel Scheduler
    // ---------------------------------------------------------

    socket_ks = conectar(ks_ip, ks_port);

    if (socket_ks == -1) {
        log_error(logger, "## No se pudo conectar al Kernel Scheduler");
        log_destroy(logger);
        config_destroy(config);
        return 1;
    }

    if (!hacer_handshake_con_scheduler(identificador)) {
        log_destroy(logger);
        config_destroy(config);
        return 1;
    }

    // ---------------------------------------------------------
    // Conexion con Kernel Memory
    // ---------------------------------------------------------

    socket_km = conectar(km_ip, km_port);

    if (socket_km == -1) {
        log_error(logger, "## No se pudo conectar al Kernel Memory");
        log_destroy(logger);
        config_destroy(config);
        return 1;
    }

    if (!hacer_handshake_con_memoria(identificador)) {
        log_destroy(logger);
        config_destroy(config);
        return 1;
    }

    memoria_inicializar(atoi(identificador));
    actualizar_memory_sticks();

    // ---------------------------------------------------------
    // Loop principal
    // ---------------------------------------------------------

    log_info(logger, "## CPU lista, esperando procesos del Kernel Scheduler...");

    while (1) {
        int codigo = 0;

        if (!recibir_entero(socket_ks, &codigo)) {
            log_error(logger, "Error al recibir codigo del Kernel Scheduler");
            break;
        }

        if (codigo == OP_INTERRUMPIR_PROCESO) {
            int pid_interrumpido = 0;

            if (!recibir_entero(socket_ks, &pid_interrumpido)) {
                log_error(logger, "Error al recibir PID de interrupcion tardia");
                break;
            }

            log_warning(
                logger,
                "Interrupcion tardia descartada para PID %d",
                pid_interrumpido
            );

            continue;
        }

        if (codigo != OP_EJECUTAR_PROCESO) {
            log_warning(logger, "Codigo inesperado desde Kernel Scheduler: %d", codigo);
            continue;
        }

        int pid = 0;

        if (!recibir_entero(socket_ks, &pid)) {
            log_error(logger, "Error al recibir PID");
            break;
        }

        log_info(logger, "## Proceso PID %d asignado para ejecucion", pid);

        if (!obtener_contexto(pid)) {
            log_error(logger, "PID: %d - No se pudo obtener el contexto", pid);
            avisar_fin_a_scheduler(pid, CPU_MOTIVO_ERROR);
            continue;
        }

        t_motivo_desalojo motivo = CPU_MOTIVO_CONTINUAR;

        while (motivo == CPU_MOTIVO_CONTINUAR) {
            motivo = ejecutar_ciclo_instruccion(pid);
        }

        avisar_fin_a_scheduler(pid, motivo);
    }
    memoria_liberar();

    log_destroy(logger);
    config_destroy(config);

    return 0;
}