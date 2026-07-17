#ifndef CONTEXTO_H
#define CONTEXTO_H

#include <stdint.h>
#include <commons/collections/list.h>
#include <utils/protocolo.h>   // define t_segmento (compartido con la CPU)

extern char* scripts_basepath;

// registros de la CPU
typedef struct {
    uint32_t PC;
    uint8_t  AX;
    uint8_t  BX;
    uint8_t  CX;
    uint8_t  DX;
    uint32_t EAX;
    uint32_t EBX;
    uint32_t ECX;
    uint32_t EDX;
    uint32_t SI;
    uint32_t DI;
} t_registros;

// contexto completo de un proceso con tabla de segmentos 
typedef struct {
    int         pid;
    t_registros registros;
    char*       path_instrucciones;
    t_list*     instrucciones;
    t_list*     tabla_segmentos;   // elementos: t_segmento* (de protocolo.h)
} t_contexto;

// la lista global donde se guardan todos los contextos
extern t_list* lista_contextos;

// funciones
void       contextos_init();
t_contexto* contexto_crear(int pid, char* path);
t_contexto* contexto_buscar(int pid);
void        contexto_eliminar(int pid);
void contexto_guardar(int pid, t_registros registros);


#endif