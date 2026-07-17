#ifndef MEMORY_STICK_H
#define MEMORY_STICK_H

#include <stdint.h>
#include <commons/collections/list.h>
#include <stdbool.h>

typedef struct {
    int      fd;            // socket para pedirle lectura/escritura
    uint32_t tamanio;        // tamaño que informó al conectarse
    uint32_t base_global;    // dirección física global donde "arranca" este stick
    char     ip[64];        // IP desde donde se conectó (para informar a las CPUs)
    int      puerto;        // puerto de escucha que informó al conectarse
} t_memory_stick;

extern t_list*  lista_memory_sticks;
extern uint32_t memoria_total;

void memory_sticks_init();
void memory_stick_agregar(int fd, uint32_t tamanio, const char* ip, int puerto);
void memory_stick_eliminar(int fd);
t_memory_stick* memory_stick_localizar(uint32_t dir_fisica_global, uint32_t* offset_out);

// lectura/escritura con direccion fisica GLOBAL, repartida entre sticks
bool memoria_fisica_leer(uint32_t dir_global, uint32_t tamanio, void* destino);
bool memoria_fisica_escribir(uint32_t dir_global, uint32_t tamanio, const void* origen);

#endif