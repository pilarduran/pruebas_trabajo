#ifndef CPU_H
#define CPU_H

#include <stdint.h>

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
} t_registros_cpu;

extern t_registros_cpu registros;

// Funciones para manejar los registros 
void inicializar_registros(void);
uint32_t obtener_valor_registro(char* nombre_registro);
void setear_valor_registro(char*nombre_registro, uint32_t valor);

#endif