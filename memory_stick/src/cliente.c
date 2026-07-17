#include "cliente.h"
#include <string.h>   
#include <unistd.h>  
#include <netdb.h>    
#include <sys/socket.h> 
#include <stdint.h>   
#include <stdio.h>
#include <utils/protocolo.h>

int conectar_a(char* ip, char* puerto, uint32_t tamanio, int puerto_escucha) {

    int err;
    struct addrinfo hints, *server_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    err = getaddrinfo(ip, puerto, &hints, &server_info);
    if (err != 0) {
        printf("Error en getaddrinfo: %s\n", gai_strerror(err));
        return -1;
    }

    int fd_conexion = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);
    if (fd_conexion == -1) {
        printf("Error: no se pudo crear el socket.\n");
        freeaddrinfo(server_info);
        return -1;
    }

    err = connect(fd_conexion, server_info->ai_addr, server_info->ai_addrlen);
    if (err == -1) {
        printf("Error: no se pudo conectar a %s:%s\n", ip, puerto);
        close(fd_conexion);
        freeaddrinfo(server_info);
        return -1;
    }

    socket_set_nodelay(fd_conexion);
    freeaddrinfo(server_info);


    // Le aviso al Kernel Memory que es el Memory Stick
    int32_t handshake = HANDSHAKE_MEMORY_STICK;
    int32_t resultado = 0;

    send(fd_conexion, &handshake, sizeof(int32_t), 0);
    recv(fd_conexion, &resultado, sizeof(int32_t), MSG_WAITALL);

    
   if (resultado == HANDSHAKE_OK) {
        // informa el tamaño del stick y el puerto donde escucha a las CPUs
        send(fd_conexion, &tamanio, sizeof(uint32_t), 0);
        send(fd_conexion, &puerto_escucha, sizeof(int), 0);
    } else {
        printf("Error: Kernel Memory rechazó la conexión\n");
        close(fd_conexion);
        return -1;
    }

    return fd_conexion;
}