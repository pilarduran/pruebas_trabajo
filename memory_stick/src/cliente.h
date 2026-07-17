#ifndef CLIENTE_H
#define CLIENTE_H

#include <stdint.h>

int conectar_a(char* ip, char* puerto, uint32_t tamanio, int puerto_escucha);

#endif 