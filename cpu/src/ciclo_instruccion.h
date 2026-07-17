#ifndef CICLO_INSTRUCCION_H
#define CICLO_INSTRUCCION_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CPU_MOTIVO_CONTINUAR,
    CPU_MOTIVO_EXIT,
    CPU_MOTIVO_SYSCALL_BLOQUEANTE,
    CPU_MOTIVO_FIN_QUANTUM,
    CPU_MOTIVO_SEG_FAULT,
    CPU_MOTIVO_ERROR
} t_motivo_desalojo;

t_motivo_desalojo ejecutar_ciclo_instruccion(uint32_t pid);

char* fetch(uint32_t pid);

void decode(char* instruccion, char** operacion, char** param1, char** param2);

t_motivo_desalojo execute(uint32_t pid, char* operacion, char* param1, char* param2);

bool check_interrupt(uint32_t pid);

#endif