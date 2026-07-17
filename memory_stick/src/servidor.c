#include "servidor.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>

int iniciar_servidor(char* puerto) {

    int err;
    struct addrinfo hints, *server_info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    err = getaddrinfo(NULL, puerto, &hints, &server_info);

    int fd_escucha = socket(server_info->ai_family, server_info->ai_socktype, server_info->ai_protocol);

    err = setsockopt(fd_escucha, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int));

    err = bind(fd_escucha, server_info->ai_addr, server_info->ai_addrlen);

    err = listen(fd_escucha, SOMAXCONN);

    freeaddrinfo(server_info);

    printf("Servidor escuchando en puerto %s\n", puerto);

    return fd_escucha;
}