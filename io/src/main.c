#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <utils/protocolo.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <commons/log.h>
#include <commons/config.h>

// ── Protocolo de comunicación ─────────────────────────────────────────


// Variables globales del módulo
t_log*    logger = NULL;
t_config* config = NULL;
char      nombre_log_global[64];

// ── Declaración de funciones ──────────────────────────────────────────
bool enviar_entero(int socket, int valor);
bool recibir_entero(int socket, int* dest);

bool iniciar_logger(const char* tipo);
bool cargar_configuracion(const char* path_config);
int  conectar_a_kernel_scheduler(char* ip, char* puerto);
bool hacer_handshake(int socket_ks, int tipo_codigo);
int  obtener_tipo_codigo(const char* tipo);
void liberar_recursos(int socket_ks);

void bucle_sleep(int socket_ks);
void bucle_stdin(int socket_ks);
void bucle_stdout(int socket_ks);

// ─ Main ──────────────────────────────────────────

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);

    if (argc != 3) {
        printf("Uso: ./bin/io [Archivo Config] [Tipo]\n");
        printf("Tipos posibles: STDIN, STDOUT, SLEEP\n");
        return EXIT_FAILURE;
    }

    char* path_config = argv[1];
    char* tipo        = argv[2];

    if (strcmp(tipo, "STDIN")  != 0 &&
        strcmp(tipo, "STDOUT") != 0 &&
        strcmp(tipo, "SLEEP")  != 0) {
        printf("Tipo invalido: %s. Debe ser STDIN, STDOUT o SLEEP\n", tipo);
        return EXIT_FAILURE;
    }

    if (!iniciar_logger(tipo)) {
        printf("No se pudo iniciar el logger\n");
        return EXIT_FAILURE;
    }

    if (!cargar_configuracion(path_config)) {
        liberar_recursos(-1);
        return EXIT_FAILURE;
    }

    int tipo_codigo = obtener_tipo_codigo(tipo);

    char* ip_ks     = config_get_string_value(config, "IP_KERNEL_SCHEDULER");
    char* puerto_ks = config_get_string_value(config, "PUERTO_KERNEL_SCHEDULER");

    int socket_ks = conectar_a_kernel_scheduler(ip_ks, puerto_ks);
    if (socket_ks == -1) {
        liberar_recursos(-1);
        return EXIT_FAILURE;
    }

    if (!hacer_handshake(socket_ks, tipo_codigo)) {
        liberar_recursos(socket_ks);
        return EXIT_FAILURE;
    }

    log_info(logger, "## Conectado a Kernel Scheduler");

    if (tipo_codigo == OP_IO_SLEEP) {
        bucle_sleep(socket_ks);
    } else if (tipo_codigo == OP_IO_STDIN) {
        bucle_stdin(socket_ks);
    } else {
        bucle_stdout(socket_ks);
    }

    log_info(logger, "Kernel Scheduler se desconecto. Cerrando IO.");
    liberar_recursos(socket_ks);
    return EXIT_SUCCESS;
}

// ── Funciones Aux ──────────────────────────────────────────

bool iniciar_logger(const char* tipo) {
    sprintf(nombre_log_global, "io_%s.log", tipo);
    logger = log_create(nombre_log_global, "IO", true, LOG_LEVEL_INFO);
    return logger != NULL;
}

bool cargar_configuracion(const char* path_config) {
    config = config_create((char*) path_config);
    if (config == NULL) {
        log_error(logger, "No se pudo leer el archivo de configuracion: %s", path_config);
        return false;
    }

    if (!config_has_property(config, "IP_KERNEL_SCHEDULER")    ||
        !config_has_property(config, "PUERTO_KERNEL_SCHEDULER") ||
        !config_has_property(config, "LOG_LEVEL")) {
        log_error(logger, "Faltan claves en el archivo de configuracion");
        return false;
    }

    char* nivel_log = config_get_string_value(config, "LOG_LEVEL");
    log_destroy(logger);
    logger = log_create(nombre_log_global, "IO", true, log_level_from_string(nivel_log));

    return true;
} 

bool enviar_entero(int socket, int valor) {
    return send(socket, &valor, sizeof(int), 0) == sizeof(int);
}

bool recibir_entero(int socket, int* dest) {
    return recv(socket, dest, sizeof(int), MSG_WAITALL) == sizeof(int);
}


int conectar_a_kernel_scheduler(char* ip, char* puerto) {

    struct addrinfo hints;
    struct addrinfo* server_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int resultado = getaddrinfo(ip, puerto, &hints, &server_info);
    if (resultado != 0) {
        log_error(logger, "getaddrinfo fallo: %s", gai_strerror(resultado));
        return -1;
    }

    int socket_fd = socket(
        server_info->ai_family,
        server_info->ai_socktype,
        server_info->ai_protocol
    );

    if (socket_fd == -1) {
        log_error(logger, "No se pudo crear el socket");
        freeaddrinfo(server_info);
        return -1;
    }

    resultado = connect(socket_fd, server_info->ai_addr, server_info->ai_addrlen);
    if (resultado == -1) {
        log_error(logger, "No se pudo conectar a %s:%s", ip, puerto);
        close(socket_fd);
        freeaddrinfo(server_info);
        return -1;
    }

    freeaddrinfo(server_info);
    return socket_fd;
}

bool hacer_handshake(int socket_ks, int tipo_codigo) {

    if (!enviar_entero(socket_ks, HANDSHAKE_IO)) {
        log_error(logger, "Error al enviar HANDSHAKE_IO");
        return false;
    }

    if (!enviar_entero(socket_ks, tipo_codigo)) {
        log_error(logger, "Error al enviar tipo de IO");
        return false;
    }

    int respuesta = 0;
    if (!recibir_entero(socket_ks, &respuesta)) {
        log_error(logger, "Error al recibir respuesta del handshake");
        return false;
    }

    if (respuesta != HANDSHAKE_OK) {
        log_error(logger, "Handshake rechazado por el Kernel Scheduler (codigo: %d)", respuesta);
        return false;
    }

    return true;
}

int obtener_tipo_codigo(const char* tipo) {
    if (strcmp(tipo, "SLEEP")  == 0) return OP_IO_SLEEP;
    if (strcmp(tipo, "STDIN")  == 0) return OP_IO_STDIN;
    if (strcmp(tipo, "STDOUT") == 0) return OP_IO_STDOUT;
    return -1;
}

void bucle_sleep(int socket_ks) {
    while (1) {
        int pid = 0;
        if (!recibir_entero(socket_ks, &pid)) break;

        int tiempo = 0;
        if (!recibir_entero(socket_ks, &tiempo)) break;

        log_info(logger, "## PID: %d - Inicio de IO", pid);
        log_info(logger, "## PID: %d - Haciendo sleep por %d milisegundos.", pid, tiempo);

        usleep((useconds_t) tiempo * 1000);

        log_info(logger, "## PID: %d - Fin de IO", pid);

        enviar_entero(socket_ks, OP_OK);
    }
}

void bucle_stdin(int socket_ks) {
    while (1) {
        int pid = 0;
        if (!recibir_entero(socket_ks, &pid)) break;

        int tamanio = 0;
        if (!recibir_entero(socket_ks, &tamanio)) break;

        log_info(logger, "## PID: %d - Inicio de IO", pid);
        log_info(logger, "## PID: %d - Ingrese %d caracteres:", pid, tamanio);

        char* buffer = malloc(tamanio + 1);
        memset(buffer, '\0', tamanio + 1);

        if (fgets(buffer, tamanio + 1, stdin) != NULL) {
            int longitud = strlen(buffer);
            if (longitud > 0 && buffer[longitud - 1] == '\n') {
                buffer[longitud - 1] = '\0';
            }
        }

        log_info(logger, "## PID: %d - Fin de IO", pid);

        send(socket_ks, buffer, tamanio, 0);
        free(buffer);
    }
}

void bucle_stdout(int socket_ks) {
    while (1) {
        int pid = 0;
        if (!recibir_entero(socket_ks, &pid)) break;

        int tamanio = 0;
        if (!recibir_entero(socket_ks, &tamanio)) break;

        char* buffer = malloc(tamanio + 1);
        memset(buffer, '\0', tamanio + 1);
        recv(socket_ks, buffer, tamanio, MSG_WAITALL);

        log_info(logger, "## PID: %d - Inicio de IO", pid);
        log_info(logger, "## PID: %d - %s", pid, buffer);
        log_info(logger, "## PID: %d - Fin de IO", pid);

        free(buffer);
        enviar_entero(socket_ks, OP_OK);
    }
}

void liberar_recursos(int socket_ks) {
    if (socket_ks != -1) {
        close(socket_ks);
    }
    if (config != NULL) {
        config_destroy(config);
        config = NULL;
    }
    if (logger != NULL) {
        log_destroy(logger);
        logger = NULL;
    }
}


