#include "servidor.h"
#include <utils/protocolo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

int iniciar_servidor(char* puerto) {
    int socket_servidor;
    int opcion_reutilizar = 1;
    int resultado;

    struct addrinfo hints;
    struct addrinfo* servinfo;

    // limpia la estructura hints antes de configurarla.
    memset(&hints, 0, sizeof(hints));

    // AF_INET indica que usamos IPv4.
    hints.ai_family = AF_INET;

    // SOCK_STREAM indica que usamos TCP.
    hints.ai_socktype = SOCK_STREAM;

    // AI_PASSIVE permite bindear el socket a nuestra propia maquina.
    hints.ai_flags = AI_PASSIVE;

    // obtiene la informacion necesaria para crear el socket servidor.
    resultado = getaddrinfo(NULL, puerto, &hints, &servinfo);
    if (resultado != 0) {
        printf("Error en getaddrinfo: %s\n", gai_strerror(resultado));
        return -1;
    }

    // crea el socket servidor usando la informacion obtenida.
    socket_servidor = socket(
        servinfo->ai_family,
        servinfo->ai_socktype,
        servinfo->ai_protocol
    );

    if (socket_servidor == -1) {
        perror("Error al crear el socket servidor");
        freeaddrinfo(servinfo);
        return -1;
    }

    // permite reutilizar el puerto si el programa se cierra y se abre rapido.
    if (setsockopt(
            socket_servidor,
            SOL_SOCKET,
            SO_REUSEADDR,
            &opcion_reutilizar,
            sizeof(opcion_reutilizar)
        ) == -1) {
        perror("Error en setsockopt");
        close(socket_servidor);
        freeaddrinfo(servinfo);
        return -1;
    }

    // asocia el socket al puerto recibido por parametro.
    if (bind(socket_servidor, servinfo->ai_addr, servinfo->ai_addrlen) == -1) {
        perror("Error en bind");
        close(socket_servidor);
        freeaddrinfo(servinfo);
        return -1;
    }

    // deja el socket escuchando conexiones entrantes.
    if (listen(socket_servidor, SOMAXCONN) == -1) {
        perror("Error en listen");
        close(socket_servidor);
        freeaddrinfo(servinfo);
        return -1;
    }

    // libera la memoria reservada por getaddrinfo.
    freeaddrinfo(servinfo);

    printf("Servidor escuchando en puerto %s\n", puerto);

    return socket_servidor;
}

int esperar_cliente(int socket_servidor) {
    // acepta una conexion entrante y devuelve el file descriptor del cliente.
    int socket_cliente = accept(socket_servidor, NULL, NULL);

    if (socket_cliente == -1) {
        perror("Error en accept");
        return -1;
    }

    printf("Se conectó un cliente\n");

    return socket_cliente;
}

int recibir_codigo(int socket_cliente) {
    int codigo;

    // recibe un entero con el codigo de handshake del cliente.
    int bytes_recibidos = recv(socket_cliente, &codigo, sizeof(int), MSG_WAITALL);

    // si recv falla o el cliente se desconecta, devolvemos -1.
    if (bytes_recibidos <= 0) {
        return -1;
    }

    return codigo;
}

void enviar_codigo(int socket_cliente, int codigo) {
    // envia un entero como respuesta al cliente.
    int bytes_enviados = send(socket_cliente, &codigo, sizeof(int), 0);

    if (bytes_enviados == -1) {
        perror("Error en send");
    }
}