#ifndef INSTRUCCIONES_H
#define INSTRUCCIONES_H

#include <stdint.h>
#include <stdbool.h>

// INSTRUCCIONES SIN MEMORIA (Check 2)

void ejecutar_NOOP(void);
// No Operation: no hace nada, solo consume un ciclo de instruccion

void ejecutar_SET(char* registro, uint32_t valor);
// SET: asigna valor a registro. Ejemplo: SET AX 10

void ejecutar_SUM(char* registro_destino, char* registroASumar);
// SUM: suma dos registros. Ejemplo: SUM AX(donde se guarda el resultado) BX(registro que suma) -> AX = AX + BX

void ejecutar_SUB(char* registro_destino, char* registroARestar);
// SUB: resta dos registros. Ejemplo SUB AX BX

bool ejecutar_JNZ(char* registro, uint32_t numero_instruccion);
// Jump if Not Zero (salto condicional): si retorna true, se hizo el salto -> NO incrementar PC. Si retorna false -> incrementar PC. Ejemplo: JNZ AX 5 -> Si AX !=0, el PC salta a la instruccion 5

void ejecutar_EXIT(void);
// Finalizar proceso: devuelve el proceso al Kernel Scheduler para que lo termine

#endif
