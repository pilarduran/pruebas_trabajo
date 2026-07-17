#ifndef CONTEXTO_H
#define CONTEXTO_H 

#include <stdint.h> 
#include <stdbool.h> 
#include <commons/collections/list.h>

// Tabla de segmentos del proceso que la CPU tiene en ejecución. 
// Es reemplazada por completo cada vez que llega un PID nuevo. 
extern t_list* tabla_segmentos; 

bool obtener_contexto(uint32_t pid);
bool guardar_contexto(uint32_t pid);

#endif 

