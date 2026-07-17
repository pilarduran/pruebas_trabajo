#include "memoria.h"

#include <commons/log.h>
#include <commons/collections/list.h>
#include <utils/protocolo.h>

#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

extern t_log* logger;
extern int socket_km;

bool enviar_entero(int socket, int valor);
bool recibir_entero(int socket, int* valor_recibido);
int conectar(char* ip, int puerto);

typedef struct {
    int socket;
    char* ip;
    int puerto;
    uint32_t base_global;
    uint32_t tamanio;
} t_stick_cpu;

static t_list* sticks_cpu = NULL;
static int id_cpu_global = -1;

static bool enviar_exactamente(int socket, const void* buffer, uint32_t tamanio) {
    uint32_t enviados_totales = 0;

    while (enviados_totales < tamanio) {
        int enviados = send(
            socket,
            (char*) buffer + enviados_totales,
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

static bool recibir_exactamente(int socket, void* buffer, uint32_t tamanio) {
    uint32_t recibidos_totales = 0;

    while (recibidos_totales < tamanio) {
        int recibidos = recv(
            socket,
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

static void destruir_stick(void* elemento) {
    t_stick_cpu* stick = elemento;

    if (stick == NULL) {
        return;
    }

    if (stick->socket != -1) {
        close(stick->socket);
    }

    free(stick->ip);
    free(stick);
}

static void reemplazar_lista_sticks(t_list* nueva_lista) {
    if (sticks_cpu != NULL) {
        list_destroy_and_destroy_elements(sticks_cpu, destruir_stick);
    }

    sticks_cpu = nueva_lista;
}

void memoria_inicializar(int id_cpu) {
    id_cpu_global = id_cpu;

    if (sticks_cpu == NULL) {
        sticks_cpu = list_create();
    }
}

void memoria_liberar(void) {
    if (sticks_cpu != NULL) {
        list_destroy_and_destroy_elements(sticks_cpu, destruir_stick);
        sticks_cpu = NULL;
    }
}

static bool hacer_handshake_stick(t_stick_cpu* stick) {
    enviar_entero(stick->socket, HANDSHAKE_CPU);
    enviar_entero(stick->socket, id_cpu_global);

    int respuesta = 0;

    if (!recibir_entero(stick->socket, &respuesta)) {
        return false;
    }

    return respuesta == HANDSHAKE_OK;
}

/*
    Kernel Memory responde OP_LISTA_STICKS así:

        cantidad

        por cada stick:
            len_ip
            ip
            puerto
            tamanio

    Como todavía no manda base_global, CPU reconstruye la base acumulando
    los tamaños en el orden en que Kernel Memory devuelve la lista.
*/
bool actualizar_memory_sticks(void) {
    enviar_entero(socket_km, OP_LISTA_STICKS);

    int cantidad = 0;

    if (!recibir_entero(socket_km, &cantidad)) {
        log_error(logger, "No se pudo recibir cantidad de Memory Sticks desde Kernel Memory");
        return false;
    }

    if (cantidad < 0) {
        log_error(logger, "Cantidad invalida de Memory Sticks: %d", cantidad);
        return false;
    }

    t_list* nueva_lista = list_create();
    uint32_t base_actual = 0;

    for (int i = 0; i < cantidad; i++) {
        int len_ip = 0;

        if (!recibir_entero(socket_km, &len_ip)) {
            log_error(logger, "No se pudo recibir longitud de IP de Memory Stick");
            list_destroy_and_destroy_elements(nueva_lista, destruir_stick);
            return false;
        }

        if (len_ip <= 0 || len_ip > 255) {
            log_error(logger, "Longitud de IP invalida para Memory Stick: %d", len_ip);
            list_destroy_and_destroy_elements(nueva_lista, destruir_stick);
            return false;
        }

        char* ip = malloc(len_ip + 1);

        if (ip == NULL) {
            log_error(logger, "No se pudo reservar memoria para IP de Memory Stick");
            list_destroy_and_destroy_elements(nueva_lista, destruir_stick);
            return false;
        }

        if (!recibir_exactamente(socket_km, ip, len_ip)) {
            free(ip);
            log_error(logger, "No se pudo recibir IP de Memory Stick");
            list_destroy_and_destroy_elements(nueva_lista, destruir_stick);
            return false;
        }

        ip[len_ip] = '\0';

        int puerto = 0;
        uint32_t tamanio = 0;

        if (!recibir_entero(socket_km, &puerto)) {
            free(ip);
            log_error(logger, "No se pudo recibir puerto de Memory Stick");
            list_destroy_and_destroy_elements(nueva_lista, destruir_stick);
            return false;
        }

        if (!recibir_exactamente(socket_km, &tamanio, sizeof(uint32_t))) {
            free(ip);
            log_error(logger, "No se pudo recibir tamanio de Memory Stick");
            list_destroy_and_destroy_elements(nueva_lista, destruir_stick);
            return false;
        }

        t_stick_cpu* stick = malloc(sizeof(t_stick_cpu));

        if (stick == NULL) {
            free(ip);
            log_error(logger, "No se pudo reservar estructura de Memory Stick en CPU");
            list_destroy_and_destroy_elements(nueva_lista, destruir_stick);
            return false;
        }

        stick->ip = ip;
        stick->puerto = puerto;
        stick->tamanio = tamanio;
        stick->base_global = base_actual;
        stick->socket = conectar(ip, puerto);

        /*
            Avanzamos la base aunque este stick no conecte,
            porque la base de los sticks siguientes depende del tamaño
            global informado por Kernel Memory.
        */
        base_actual += tamanio;

        if (stick->socket == -1) {
            log_warning(
                logger,
                "No se pudo conectar al Memory Stick %s:%d",
                ip,
                puerto
            );
            destruir_stick(stick);
            continue;
        }

        if (!hacer_handshake_stick(stick)) {
            log_warning(
                logger,
                "Handshake rechazado por Memory Stick %s:%d",
                ip,
                puerto
            );
            destruir_stick(stick);
            continue;
        }

        log_info(
            logger,
            "## Memory Stick conocido por CPU - IP: %s - Puerto: %d - Base: %d - Tamaño: %d",
            stick->ip,
            stick->puerto,
            stick->base_global,
            stick->tamanio
        );

        list_add(nueva_lista, stick);
    }

    reemplazar_lista_sticks(nueva_lista);

    log_info(
        logger,
        "## CPU actualizó lista de Memory Sticks - Cantidad conectada: %d",
        list_size(sticks_cpu)
    );

    return true;
}

static t_stick_cpu* buscar_stick_por_direccion(uint32_t direccion_fisica, uint32_t* offset) {
    if (sticks_cpu == NULL) {
        return NULL;
    }

    for (int i = 0; i < list_size(sticks_cpu); i++) {
        t_stick_cpu* stick = list_get(sticks_cpu, i);

        uint32_t inicio = stick->base_global;
        uint32_t fin = stick->base_global + stick->tamanio;

        if (direccion_fisica >= inicio && direccion_fisica < fin) {
            *offset = direccion_fisica - stick->base_global;
            return stick;
        }
    }

    return NULL;
}

static t_stick_cpu* buscar_o_refrescar_stick(uint32_t direccion_fisica, uint32_t* offset) {
    t_stick_cpu* stick = buscar_stick_por_direccion(direccion_fisica, offset);

    if (stick != NULL) {
        return stick;
    }

    /*
        Si no encontramos el stick, puede ser porque se conectó uno nuevo
        después de que CPU había pedido la lista.
    */
    if (!actualizar_memory_sticks()) {
        return NULL;
    }

    return buscar_stick_por_direccion(direccion_fisica, offset);
}

bool leer_memoria(uint32_t direccion_fisica, uint32_t tamanio, void* destino) {
    if (tamanio == 0) {
        return true;
    }

    if (sticks_cpu == NULL || list_size(sticks_cpu) == 0) {
        if (!actualizar_memory_sticks()) {
            return false;
        }
    }

    uint32_t leidos = 0;

    while (leidos < tamanio) {
        uint32_t offset = 0;
        t_stick_cpu* stick = buscar_o_refrescar_stick(direccion_fisica + leidos, &offset);

        if (stick == NULL) {
            log_error(
                logger,
                "No se encontro Memory Stick para DF %d",
                direccion_fisica + leidos
            );
            return false;
        }

        uint32_t restante_operacion = tamanio - leidos;
        uint32_t restante_stick = stick->tamanio - offset;
        uint32_t a_leer = restante_operacion < restante_stick ? restante_operacion : restante_stick;

        enviar_entero(stick->socket, OP_LEER_MEMORIA);

        if (!enviar_exactamente(stick->socket, &offset, sizeof(uint32_t))) {
            log_error(logger, "Error enviando offset de lectura a Memory Stick");
            return false;
        }

        if (!enviar_exactamente(stick->socket, &a_leer, sizeof(uint32_t))) {
            log_error(logger, "Error enviando tamanio de lectura a Memory Stick");
            return false;
        }

        int respuesta = 0;

        if (!recibir_entero(stick->socket, &respuesta)) {
            log_error(logger, "No se pudo recibir respuesta de lectura de Memory Stick");
            return false;
        }

        if (respuesta != OP_OK) {
            log_error(
                logger,
                "Memory Stick respondio error al leer - DF: %d - Offset: %d - Tamaño: %d",
                direccion_fisica + leidos,
                offset,
                a_leer
            );
            return false;
        }

        if (!recibir_exactamente(stick->socket, (char*) destino + leidos, a_leer)) {
            log_error(logger, "No se pudieron recibir bytes de lectura desde Memory Stick");
            return false;
        }

        leidos += a_leer;
    }

    log_debug(
        logger,
        "Lectura de memoria consolidada OK - DF: %d - Tamaño: %d",
        direccion_fisica,
        tamanio
    );

    return true;
}

bool escribir_memoria(uint32_t direccion_fisica, uint32_t tamanio, void* origen) {
    if (tamanio == 0) {
        return true;
    }

    if (sticks_cpu == NULL || list_size(sticks_cpu) == 0) {
        if (!actualizar_memory_sticks()) {
            return false;
        }
    }

    uint32_t escritos = 0;

    while (escritos < tamanio) {
        uint32_t offset = 0;
        t_stick_cpu* stick = buscar_o_refrescar_stick(direccion_fisica + escritos, &offset);

        if (stick == NULL) {
            log_error(
                logger,
                "No se encontro Memory Stick para DF %d",
                direccion_fisica + escritos
            );
            return false;
        }

        uint32_t restante_operacion = tamanio - escritos;
        uint32_t restante_stick = stick->tamanio - offset;
        uint32_t a_escribir = restante_operacion < restante_stick ? restante_operacion : restante_stick;

        enviar_entero(stick->socket, OP_ESCRIBIR_MEMORIA);

        if (!enviar_exactamente(stick->socket, &offset, sizeof(uint32_t))) {
            log_error(logger, "Error enviando offset de escritura a Memory Stick");
            return false;
        }

        if (!enviar_exactamente(stick->socket, &a_escribir, sizeof(uint32_t))) {
            log_error(logger, "Error enviando tamanio de escritura a Memory Stick");
            return false;
        }

        if (!enviar_exactamente(stick->socket, (char*) origen + escritos, a_escribir)) {
            log_error(logger, "Error enviando bytes a escribir a Memory Stick");
            return false;
        }

        int respuesta = 0;

        if (!recibir_entero(stick->socket, &respuesta)) {
            log_error(logger, "No se pudo recibir respuesta de escritura de Memory Stick");
            return false;
        }

        if (respuesta != OP_OK) {
            log_error(
                logger,
                "Memory Stick respondio error al escribir - DF: %d - Offset: %d - Tamaño: %d",
                direccion_fisica + escritos,
                offset,
                a_escribir
            );
            return false;
        }

        escritos += a_escribir;
    }

    log_debug(
        logger,
        "Escritura de memoria consolidada OK - DF: %d - Tamaño: %d",
        direccion_fisica,
        tamanio
    );

    return true;
}