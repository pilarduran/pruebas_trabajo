#ifndef MEMORIA_USUARIO_H
#define MEMORIA_USUARIO_H

#include <stdint.h>
#include <stdbool.h>

// parametros de configuracion (los setea main.c al leer el config)
extern int  segment_max_size;
extern bool allocation_best_fit;    // true = BEST, false = WORST
extern int  compaction_delay_ms;

typedef enum {
    CREAR_SEG_OK,
    CREAR_SEG_COMPACTAR,   // hay espacio total pero no contiguo
    CREAR_SEG_ERROR        // no hay espacio ni compactando / pid invalido / excede maximo
} t_resultado_crear;

void memoria_usuario_init(void);


//--segmentos 
t_resultado_crear segmento_crear(int pid, int id_segmento, uint32_t tamanio);
bool segmento_eliminar(int pid, int id_segmento);

// libera huecos y bloques de swap de TODOS los segmentos del proceso
// (para la finalizacion). No borra el contexto.
void proceso_liberar_memoria(int pid);

// --Compactacion
// reordena todos los segmentos al principio de la memoria y actualiza las tablas de segmentos. Asume que el KS ya desalojo las CPUs.
bool compactar(void);

// ---swap
bool proceso_suspender(int pid);
// false si no entran todos los segmentos sin disparar compactacion
bool proceso_dessuspender(int pid);
uint32_t memoria_libre_total(void);
void swap_registrar(int fd, int block_size, int swap_size);
void swap_desregistrar(void);

#endif