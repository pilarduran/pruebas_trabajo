#include "mmu.h"
#include "contexto.h"

#include <commons/log.h>
#include <commons/collections/list.h>
#include <utils/protocolo.h>

#include <stdint.h>
#include <stdbool.h>

extern t_log* logger;
extern uint32_t tam_max_segmento;

/*
    Busca en la tabla de segmentos del proceso actual
    el segmento cuyo ID coincida con el numero de segmento
    calculado a partir de la direccion logica.

    Importante:
    No usamos list_get(tabla_segmentos, id), porque el ID del segmento
    no necesariamente coincide con su posicion dentro de la lista.
*/
static t_segmento* buscar_segmento_por_id(int id_segmento) {
    for (int i = 0; i < list_size(tabla_segmentos); i++) {
        t_segmento* segmento = list_get(tabla_segmentos, i);

        if (segmento != NULL && segmento->id == id_segmento) {
            return segmento;
        }
    }

    return NULL;
}

bool traducir_direccion(
    uint32_t pid,
    uint32_t direccion_logica,
    uint32_t tamanio_acceso,
    uint32_t* direccion_fisica
) {
    if (tam_max_segmento == 0) {
        log_error(logger, "PID: %d - SEGMENT_MAX_SIZE invalido: 0", pid);
        return false;
    }

    int num_segmento = direccion_logica / tam_max_segmento;
    uint32_t desplazamiento = direccion_logica % tam_max_segmento;

    t_segmento* segmento = buscar_segmento_por_id(num_segmento);

    if (segmento == NULL) {
        log_error(
            logger,
            "PID: %d - Segmentation Fault - el segmento %d no existe",
            pid,
            num_segmento
        );
        return false;
    }

    /*
        Si el acceso pide tamanio 4 y el desplazamiento esta en el ultimo byte,
        se pasa del limite del segmento.

        Ejemplo:
            limite = 10
            desplazamiento = 8
            tamanio_acceso = 4

            8 + 4 = 12 > 10 => Segmentation Fault
    */
    if (desplazamiento + tamanio_acceso > segmento->limite) {
        log_error(
            logger,
            "PID: %d - Segmentation Fault - segmento %d - desplazamiento %d - tamanio acceso %d - limite %d",
            pid,
            num_segmento,
            desplazamiento,
            tamanio_acceso,
            segmento->limite
        );
        return false;
    }

    *direccion_fisica = segmento->base + desplazamiento;

    log_debug(
        logger,
        "PID: %d - MMU - DL: %d - Segmento: %d - Desplazamiento: %d - Base: %d - DF: %d",
        pid,
        direccion_logica,
        num_segmento,
        desplazamiento,
        segmento->base,
        *direccion_fisica
    );

    return true;
}