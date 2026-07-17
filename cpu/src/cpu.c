#include "cpu.h"
#include <string.h>
#include <commons/log.h>

t_registros_cpu registros;

void inicializar_registros(void){
    registros.PC = 0;
    registros.AX = 0;
    registros.BX = 0;
    registros.CX = 0;
    registros.DX = 0;
    registros.EAX = 0;
    registros.EBX = 0;
    registros.ECX = 0;
    registros.EDX = 0;
    registros.SI = 0;
    registros.DI = 0;
}

uint32_t obtener_valor_registro(char* nombre_registro) {
    if (strcmp(nombre_registro, "PC") == 0) {
        return registros.PC;
    }
    else if (strcmp(nombre_registro, "AX") == 0) {
        return registros.AX;
    }
    else if (strcmp(nombre_registro, "BX") == 0) {
        return registros.BX;
    }
    else if (strcmp(nombre_registro, "CX") == 0) {
        return registros.CX;
    }
    else if (strcmp(nombre_registro, "DX") == 0) {
        return registros.DX;
    }
    else if (strcmp(nombre_registro, "EAX") == 0) {
        return registros.EAX;
    }
    else if (strcmp(nombre_registro, "EBX") == 0) {
        return registros.EBX;
    }
    else if (strcmp(nombre_registro, "ECX") == 0) {
        return registros.ECX;
    }
    else if (strcmp(nombre_registro, "EDX") == 0) {
        return registros.EDX;
    }
    else if (strcmp(nombre_registro, "SI") == 0) {
        return registros.SI;
    }
    else if (strcmp(nombre_registro, "DI") == 0) {
        return registros.DI;
    }
    
    return 0;  // Si el nombre no es válido
}

void setear_valor_registro(char* nombre_registro, uint32_t valor) {
    if (strcmp(nombre_registro, "PC") == 0) {
        registros.PC = valor;
    }
    else if (strcmp(nombre_registro, "AX") == 0) {
        registros.AX = (uint8_t)valor;  // Cast a 1 byte 
    }
    else if (strcmp(nombre_registro, "BX") == 0) {
        registros.BX = (uint8_t)valor;
    }
    else if (strcmp(nombre_registro, "CX") == 0) {
        registros.CX = (uint8_t)valor;
    }
    else if (strcmp(nombre_registro, "DX") == 0) {
        registros.DX = (uint8_t)valor;
    }
    else if (strcmp(nombre_registro, "EAX") == 0) {
        registros.EAX = valor;
    }
    else if (strcmp(nombre_registro, "EBX") == 0) {
        registros.EBX = valor;
    }
    else if (strcmp(nombre_registro, "ECX") == 0) {
        registros.ECX = valor;
    }
    else if (strcmp(nombre_registro, "EDX") == 0) {
        registros.EDX = valor;
    }
    else if (strcmp(nombre_registro, "SI") == 0) {
        registros.SI = valor;
    }
    else if (strcmp(nombre_registro, "DI") == 0) {
        registros.DI = valor;
    }
}

