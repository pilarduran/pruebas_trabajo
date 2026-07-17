#include "contexto.h"
#include "cpu.h"

#include <commons/log.h>
#include <commons/collections/list.h>
#include <utils/protocolo.h>

#include <stdlib.h>
#include <stdint.h>
#include <sys/socket.h>

extern t_log* logger;
extern int socket_km;
extern t_registros_cpu registros;

bool enviar_entero(int socket, int valor);
bool recibir_entero(int socket, int* valor_recibido);

t_list* tabla_segmentos = NULL;

/*
    recv() no siempre garantiza que en una sola llamada lleguen todos los bytes.
    Por eso usamos esta funcion para recibir exactamente la cantidad pedida.
*/
static bool recibir_exactamente(int socket, void* buffer, int tamanio) {
    int recibidos_totales = 0;

    while (recibidos_totales < tamanio) {
        int recibidos = recv(
            socket,
            buffer + recibidos_totales,
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

/*
    Misma idea que recibir_exactamente, pero para enviar.
*/
static bool enviar_exactamente(int socket, void* buffer, int tamanio) {
    int enviados_totales = 0;

    while (enviados_totales < tamanio) {
        int enviados = send(
            socket,
            buffer + enviados_totales,
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

static void reiniciar_tabla_segmentos(void) {
    if (tabla_segmentos != NULL) {
        list_destroy_and_destroy_elements(tabla_segmentos, free);
    }

    tabla_segmentos = list_create();
}

bool obtener_contexto(uint32_t pid) {
    enviar_entero(socket_km, OP_OBTENER_CONTEXTO);
    enviar_entero(socket_km, (int) pid);

    /*
        Kernel Memory actual responde:

            OP_OK
            registros
            cantidad_segmentos
            segmentos...

        Si no leemos primero OP_OK, se corren todos los datos.
    */
    int respuesta = 0;

    if (!recibir_entero(socket_km, &respuesta)) {
        log_error(logger, "## PID: %d - Error al recibir respuesta de contexto", pid);
        return false;
    }

    if (respuesta != OP_OK) {
        log_error(logger, "## PID: %d - Kernel Memory respondio error al pedir contexto", pid);
        return false;
    }

    if (!recibir_exactamente(socket_km, &registros, sizeof(t_registros_cpu))) {
        log_error(logger, "## PID: %d - Error al recibir registros", pid);
        return false;
    }

    int cantidad_segmentos = 0;

    if (!recibir_entero(socket_km, &cantidad_segmentos)) {
        log_error(logger, "## PID: %d - Error al recibir cantidad de segmentos", pid);
        return false;
    }

    if (cantidad_segmentos < 0) {
        log_error(logger, "## PID: %d - Cantidad de segmentos invalida: %d", pid, cantidad_segmentos);
        return false;
    }

    reiniciar_tabla_segmentos();

    for (int i = 0; i < cantidad_segmentos; i++) {
        t_segmento* segmento = malloc(sizeof(t_segmento));

        if (segmento == NULL) {
            log_error(logger, "## PID: %d - No se pudo reservar memoria para segmento", pid);
            return false;
        }

        if (!recibir_entero(socket_km, &segmento->id)) {
            free(segmento);
            log_error(logger, "## PID: %d - Error al recibir id de segmento", pid);
            return false;
        }

        if (!recibir_exactamente(socket_km, &segmento->base, sizeof(uint32_t))) {
            free(segmento);
            log_error(logger, "## PID: %d - Error al recibir base de segmento", pid);
            return false;
        }

        if (!recibir_exactamente(socket_km, &segmento->limite, sizeof(uint32_t))) {
            free(segmento);
            log_error(logger, "## PID: %d - Error al recibir limite de segmento", pid);
            return false;
        }

        list_add(tabla_segmentos, segmento);
    }

    log_debug(
        logger,
        "## PID: %d - Contexto obtenido - PC: %d - Segmentos: %d",
        pid,
        registros.PC,
        cantidad_segmentos
    );

    return true;
}

bool guardar_contexto(uint32_t pid) {
    enviar_entero(socket_km, OP_GUARDAR_CONTEXTO);
    enviar_entero(socket_km, (int) pid);

    /*
        Kernel Memory actual espera:

            OP_GUARDAR_CONTEXTO
            PID
            registros
    */
    if (!enviar_exactamente(socket_km, &registros, sizeof(t_registros_cpu))) {
        log_error(logger, "## PID: %d - Error al enviar registros", pid);
        return false;
    }

    int respuesta = 0;

    if (!recibir_entero(socket_km, &respuesta)) {
        log_error(logger, "## PID: %d - Error al recibir respuesta de guardar contexto", pid);
        return false;
    }

    if (respuesta != OP_OK) {
        log_error(logger, "## PID: %d - Kernel Memory respondio error al guardar contexto", pid);
        return false;
    }

    return true;
}