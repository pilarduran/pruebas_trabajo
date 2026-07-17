#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utils/protocolo.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <commons/log.h>
#include <commons/config.h>

#include <signal.h>

// ── Protocolo de comunicación ─────────────────────────────────────────


// Variables globales del módulo
t_log*    logger = NULL;
t_config* config = NULL;

// ── Declaración de funciones ──────────────────────────────────────────
int  conectar_a_kernel_memory(char* ip, char* puerto);
bool hacer_handshake(int socket_km);
void liberar_recursos(int socket_km);

static bool enviar_entero(int socket, int valor) {
    return send(socket, &valor, sizeof(int), 0) == sizeof(int);
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    if (argc != 2) {
        printf("Uso: ./bin/swap [Archivo Config]\n");
        return EXIT_FAILURE;
    }

    char* path_config = argv[1];

    logger = log_create("swap.log", "SWAP", true, LOG_LEVEL_INFO);
    if (logger == NULL) {
        printf("Error: no se pudo crear el logger\n");
        return EXIT_FAILURE;
    }

    log_info(logger, "## Modulo SWAP iniciado");

    config = config_create(path_config);
    if (config == NULL) {
        log_error(logger, "No se pudo leer el archivo de configuracion: %s", path_config);
        liberar_recursos(-1);
        return EXIT_FAILURE;
    }

    if (!config_has_property(config, "IP_KERNEL_MEMORY")     ||
        !config_has_property(config, "PUERTO_KERNEL_MEMORY")  ||
        !config_has_property(config, "SWAP_FILE_PATH")        ||
        !config_has_property(config, "SWAP_FILE_SIZE")        ||
        !config_has_property(config, "BLOCK_SIZE")) {
        log_error(logger, "Faltan claves en el archivo de configuracion");
        liberar_recursos(-1);
        return EXIT_FAILURE;
    }

    char* ip_km     = config_get_string_value(config, "IP_KERNEL_MEMORY");
    char* puerto_km = config_get_string_value(config, "PUERTO_KERNEL_MEMORY");

    log_info(logger, "Configuracion cargada - KM en %s:%s", ip_km, puerto_km);

    // Conectarse al Kernel Memory
    int socket_km = conectar_a_kernel_memory(ip_km, puerto_km);
    if (socket_km == -1) {
        log_error(logger, "No se pudo conectar al Kernel Memory");
        liberar_recursos(-1);
        return EXIT_FAILURE;
    }

    // Hacer handshake
    if (!hacer_handshake(socket_km)) {
        log_error(logger, "Fallo el handshake con el Kernel Memory");
        liberar_recursos(socket_km);
        return EXIT_FAILURE;
    }

    // Log obligatorio del enunciado
    log_info(logger, "## Conectado a Kernel Memory");

    // Crear/abrir el archivo de SWAP con el tamanio configurado.
    // El enunciado asume que inicia siempre libre: no hace falta limpiarlo.
    int block_size = config_get_int_value(config, "BLOCK_SIZE");
    int swap_size  = config_get_int_value(config, "SWAP_FILE_SIZE");
    char* swap_path = config_get_string_value(config, "SWAP_FILE_PATH");

    if (block_size <= 0 || swap_size <= 0) {
        log_error(logger, "BLOCK_SIZE / SWAP_FILE_SIZE invalidos");
        liberar_recursos(socket_km);
        return EXIT_FAILURE;
    }

    FILE* archivo_swap = fopen(swap_path, "w+b");
    if (archivo_swap == NULL) {
        log_error(logger, "No se pudo crear el archivo de SWAP: %s", swap_path);
        liberar_recursos(socket_km);
        return EXIT_FAILURE;
    }
    if (ftruncate(fileno(archivo_swap), swap_size) != 0) {
        log_error(logger, "No se pudo dimensionar el archivo de SWAP");
        fclose(archivo_swap);
        liberar_recursos(socket_km);
        return EXIT_FAILURE;
    }
    log_info(logger, "Archivo de SWAP creado: %s (%d bytes, bloques de %d)", swap_path, swap_size, block_size);

    // Informar al KM el tamanio de bloque y el tamanio total del SWAP
    // (lo exige el enunciado al iniciar el modulo).
    send(socket_km, &block_size, sizeof(int), 0);
    send(socket_km, &swap_size,  sizeof(int), 0);

    int total_bloques = block_size > 0 ? swap_size / block_size : 0;
    char* buffer = malloc(block_size);

 while (1) {
        int op = 0;
        if (recv(socket_km, &op, sizeof(int), MSG_WAITALL) != sizeof(int)) {
            log_warning(logger, "Kernel Memory se desconecto. Cerrando SWAP.");
            break;
        }

        if (op == OP_SWAP_ESCRIBIR_BLOQUE) {
            int nro_bloque = 0;
            if (recv(socket_km, &nro_bloque, sizeof(int), MSG_WAITALL) != sizeof(int)) break;
            if (recv(socket_km, buffer, block_size, MSG_WAITALL) != block_size)      break;

            if (nro_bloque < 0 || nro_bloque >= total_bloques) {
                log_error(logger, "Escritura de bloque invalido: %d", nro_bloque);
                enviar_entero(socket_km, OP_ERROR);
                continue;
            }

            fseek(archivo_swap, (long)nro_bloque * block_size, SEEK_SET);
            fwrite(buffer, 1, block_size, archivo_swap);
            fflush(archivo_swap);

            log_info(logger, "## Escritura del bloque: %d", nro_bloque);
            enviar_entero(socket_km, OP_OK);

        } else if (op == OP_SWAP_LEER_BLOQUE) {
            int nro_bloque = 0;
            if (recv(socket_km, &nro_bloque, sizeof(int), MSG_WAITALL) != sizeof(int)) break;

            if (nro_bloque < 0 || nro_bloque >= total_bloques) {
                log_error(logger, "Lectura de bloque invalido: %d", nro_bloque);
                enviar_entero(socket_km, OP_ERROR);
                continue;
            }

            fseek(archivo_swap, (long)nro_bloque * block_size, SEEK_SET);
            size_t leidos = fread(buffer, 1, block_size, archivo_swap);
            memset(buffer + leidos, 0, block_size - leidos);

            log_info(logger, "## Lectura del bloque: %d", nro_bloque);
            enviar_entero(socket_km, OP_OK);
            send(socket_km, buffer, block_size, 0);

        } else {
            log_warning(logger, "Operacion desconocida: %d", op);
        }
    }


    free(buffer);
    fclose(archivo_swap);
    liberar_recursos(socket_km);
    return EXIT_SUCCESS;
}

// ── FUNCIÓN: conectar al Kernel Memory ───────────────────────────────
int conectar_a_kernel_memory(char* ip, char* puerto) {

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

// ── FUNCIÓN: handshake ────────────────────────────────────────────────
bool hacer_handshake(int socket_km) {

    int codigo = HANDSHAKE_SWAP;
    int bytes_enviados = send(socket_km, &codigo, sizeof(int), 0);
    if (bytes_enviados != sizeof(int)) {
        log_error(logger, "Error al enviar handshake");
        return false;
    }

    int respuesta = 0;
    int bytes_recibidos = recv(socket_km, &respuesta, sizeof(int), MSG_WAITALL);
    if (bytes_recibidos != sizeof(int)) {
        log_error(logger, "Error al recibir respuesta del handshake");
        return false;
    }

    if (respuesta != HANDSHAKE_OK) {
        log_error(logger, "Handshake rechazado por el Kernel Memory");
        return false;
    }

    return true;
}

// ── FUNCIÓN: liberar recursos ─────────────────────────────────────────
void liberar_recursos(int socket_km) {
    if (socket_km != -1) {
        close(socket_km);
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
