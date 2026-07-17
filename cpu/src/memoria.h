#ifndef MEMORIA_H
#define MEMORIA_H

#include <stdint.h>
#include <stdbool.h>

void memoria_inicializar(int id_cpu);
void memoria_liberar(void);

bool actualizar_memory_sticks(void);

bool leer_memoria(uint32_t direccion_fisica, uint32_t tamanio, void* destino);
bool escribir_memoria(uint32_t direccion_fisica, uint32_t tamanio, void* origen);

#endif