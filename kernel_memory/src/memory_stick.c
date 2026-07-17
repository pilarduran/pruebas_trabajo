#include "memory_stick.h"
#include <stdlib.h>
#include "huecos.h" 
#include <utils/protocolo.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <commons/log.h>

extern t_log* logger;
extern pthread_mutex_t mutex_memory_sticks;

t_list*  lista_memory_sticks;
uint32_t memoria_total;

void memory_sticks_init() {
    lista_memory_sticks = list_create();
    memoria_total = 0;
}

void memory_stick_agregar(int fd, uint32_t tamanio, const char* ip, int puerto) {
    t_memory_stick* stick = malloc(sizeof(t_memory_stick));
    stick->fd = fd;
    stick->tamanio = tamanio;
    stick->base_global = memoria_total;
    snprintf(stick->ip, sizeof(stick->ip), "%s", ip);
    stick->puerto = puerto;

    list_add(lista_memory_sticks, stick);
    memoria_total += tamanio;

    huecos_extender(stick->base_global, tamanio);
}

void memory_stick_eliminar(int fd) {
    for (int i = 0; i < list_size(lista_memory_sticks); i++) {
        t_memory_stick* stick = list_get(lista_memory_sticks, i);
        if (stick->fd == fd) {
            list_remove(lista_memory_sticks, i);
            free(stick);
            return;
        }
    }
}

t_memory_stick* memory_stick_localizar(uint32_t dir_fisica_global, uint32_t* offset_out) {
    uint32_t acumulado = 0;
    for (int i = 0; i < list_size(lista_memory_sticks); i++) {
        t_memory_stick* stick = list_get(lista_memory_sticks, i);
        if (dir_fisica_global < acumulado + stick->tamanio) {
            *offset_out = dir_fisica_global - acumulado;
            return stick;
        }
        acumulado += stick->tamanio;
    }
    return NULL;
}



/*
    Lectura/escritura fisica repartida entre sticks.
    Trabajan con direcciones fisicas GLOBALES y parten la operacion cuando
    cruza el limite de un stick, consolidando el resultado.
    Protocolo con el stick (direcciones LOCALES al stick):
      lectura:   [OP_LEER_MEMORIA][offset u32][tam u32] -> [OP_OK][bytes] | [OP_ERROR]
      escritura: [OP_ESCRIBIR_MEMORIA][offset u32][tam u32][bytes] -> [OP_OK]
*/

static bool enviar_int(int fd, int valor) {
    return send(fd, &valor, sizeof(int), 0) == sizeof(int);
}

static bool recibir_int(int fd, int* valor) {
    return recv(fd, valor, sizeof(int), MSG_WAITALL) == sizeof(int);
}

bool memoria_fisica_leer(uint32_t dir_global, uint32_t tamanio, void* destino) {
    uint32_t leidos = 0;
    while (leidos < tamanio) {
        uint32_t offset = 0;
        // copiar fd y tamanio BAJO el lock; no volver a tocar `stick` despues
        pthread_mutex_lock(&mutex_memory_sticks);
        t_memory_stick* stick = memory_stick_localizar(dir_global + leidos, &offset);
        int      fd_stick  = stick ? stick->fd      : -1;
        uint32_t tam_stick = stick ? stick->tamanio : 0;
        pthread_mutex_unlock(&mutex_memory_sticks);

        if (stick == NULL) {
            log_error(logger, "Lectura fisica fuera de rango: dir %u", dir_global + leidos);
            return false;
        }

        uint32_t restante_op    = tamanio - leidos;
        uint32_t restante_stick = tam_stick - offset;
        uint32_t a_leer = restante_op < restante_stick ? restante_op : restante_stick;

        enviar_int(fd_stick, OP_LEER_MEMORIA);
        send(fd_stick, &offset, sizeof(uint32_t), 0);
        send(fd_stick, &a_leer, sizeof(uint32_t), 0);

        int status = 0;
        if (!recibir_int(fd_stick, &status) || status != OP_OK) {
            log_error(logger, "El Memory Stick no pudo leer (offset %u, tam %u)", offset, a_leer);
            return false;
        }
        if (recv(fd_stick, (char*)destino + leidos, a_leer, MSG_WAITALL) != (ssize_t)a_leer) {
            return false;
        }

        leidos += a_leer;
    }
    return true;
}
bool memoria_fisica_escribir(uint32_t dir_global, uint32_t tamanio, const void* origen) {
    uint32_t escritos = 0;
    while (escritos < tamanio) {
        uint32_t offset = 0;
        pthread_mutex_lock(&mutex_memory_sticks);
        t_memory_stick* stick = memory_stick_localizar(dir_global + escritos, &offset);
        int      fd_stick  = stick ? stick->fd      : -1;
        uint32_t tam_stick = stick ? stick->tamanio : 0;
        pthread_mutex_unlock(&mutex_memory_sticks);

        if (stick == NULL) {
            log_error(logger, "Escritura fisica fuera de rango: dir %u", dir_global + escritos);
            return false;
        }

        uint32_t restante_op    = tamanio - escritos;
        uint32_t restante_stick = tam_stick - offset;
        uint32_t a_escribir = restante_op < restante_stick ? restante_op : restante_stick;

        enviar_int(fd_stick, OP_ESCRIBIR_MEMORIA);
        send(fd_stick, &offset, sizeof(uint32_t), 0);
        send(fd_stick, &a_escribir, sizeof(uint32_t), 0);
        send(fd_stick, (const char*)origen + escritos, a_escribir, 0);

        int status = 0;
        if (!recibir_int(fd_stick, &status) || status != OP_OK) {
            log_error(logger, "El Memory Stick no pudo escribir (offset %u, tam %u)", offset, a_escribir);
            return false;
        }

        escritos += a_escribir;
    }
    return true;
}