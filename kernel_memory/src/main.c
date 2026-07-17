#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <utils/protocolo.h>
#include <commons/log.h>
#include <commons/config.h>
#include "servidor.h"
#include "contexto.h"
#include "handlers.h"  
#include "memory_stick.h"
#include "huecos.h" 
#include "memoria_usuario.h"
#include <signal.h> 


// tiempo antes de contestar un pedido de instruccion (definido en handlers.c)
extern int instruction_delay_ms;

t_log* logger;
pthread_mutex_t mutex_contextos = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_memory_sticks = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        printf("Uso: %s [ARCHIVO_CONFIG]\n", argv[0]);
        return 1;
    }

    t_config* config = config_create(argv[1]);
    if (config == NULL) {
        printf("No se pudo leer el archivo de configuracion\n");
        return 1;
    }

    char* log_level_str = config_get_string_value(config, "LOG_LEVEL");
    char* puerto = config_get_string_value(config, "PUERTO_ESCUCHA");

     if (config_has_property(config, "SCRIPTS_BASEPATH")) {
        scripts_basepath = config_get_string_value(config, "SCRIPTS_BASEPATH");
    }
    
    segment_max_size     = config_get_int_value(config, "SEGMENT_MAX_SIZE");
    instruction_delay_ms = config_get_int_value(config, "INSTRUCTION_DELAY");
    compaction_delay_ms  = config_get_int_value(config, "COMPACTION_DELAY");
    allocation_best_fit  = strcmp(config_get_string_value(config, "ALLOCATION_STRATEGY"), "BEST") == 0;

    logger = log_create("kernel_memory.log", "KERNEL_MEMORY", true, log_level_from_string(log_level_str));
    if (logger == NULL) {
        printf("No se pudo crear el logger\n");
        config_destroy(config);
        return 1;
    }

    contextos_init();
    memory_sticks_init();
    huecos_init();
    memoria_usuario_init();

    log_info(logger, "Iniciando Kernel Memory");

    int server_fd = iniciar_servidor(puerto);
    if (server_fd == -1) {
        log_error(logger, "No se pudo iniciar el servidor");
        log_destroy(logger);
        config_destroy(config);
        return 1;
    }

    log_info(logger, "Escuchando en puerto %s", puerto);

    while (1) {
        int cliente_fd = esperar_cliente(server_fd);
        if (cliente_fd == -1) {
            log_warning(logger, "Error aceptando cliente");
            continue;
        }
        pthread_t hilo;
        pthread_create(&hilo, NULL, atender_cliente, (void*)(intptr_t)cliente_fd);
        pthread_detach(hilo);
    }

    close(server_fd);
    log_destroy(logger);
    config_destroy(config);
    return 0;
}