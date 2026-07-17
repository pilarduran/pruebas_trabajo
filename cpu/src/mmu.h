#ifndef MMU_H
#define MMU_H 

#include <stdint.h> 
#include <stdbool.h> 

bool traducir_direccion(uint32_t pid, uint32_t direccion_logica, uint32_t tamanio_acceso, uint32_t* direccion_fisica);

#endif 


