#include "instrucciones.h"
#include "cpu.h"
#include <commons/log.h>
#include <string.h>


void ejecutar_NOOP(void){
    // No hace nada. El log se hace en ciclo_instruccion
}

void ejecutar_SET(char* registro, uint32_t valor){
    setear_valor_registro(registro, valor);
}

void ejecutar_SUM(char* registro_destino, char* registroASumar){
    uint32_t valorDestino = obtener_valor_registro(registro_destino);
    uint32_t valorASumar = obtener_valor_registro(registroASumar);

    uint32_t resultado = valorDestino + valorASumar;

    setear_valor_registro(registro_destino, resultado);
}

void ejecutar_SUB(char* registro_destino, char* registroARestar){
    uint32_t valorDestino = obtener_valor_registro(registro_destino);
    uint32_t valorARestar = obtener_valor_registro(registroARestar);

    uint32_t resultado = valorDestino - valorARestar;

    setear_valor_registro(registro_destino, resultado);
}

bool ejecutar_JNZ(char* registro, uint32_t numero_instruccion){
    uint32_t valor = obtener_valor_registro(registro);

    if(valor !=0){
        registros.PC = numero_instruccion;
        return true;
    }
    
    return false; // No ocurrio un salto
}

void ejecutar_EXIT(void){
    // Marca que el proceso finaliza. El manejo esta en ciclo_instruccion.c
}


