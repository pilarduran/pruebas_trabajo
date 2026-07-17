#ifndef SERVIDOR_H
#define SERVIDOR_H

int iniciar_servidor(char* puerto);
int esperar_cliente(int socket_servidor);
int recibir_codigo(int socket_cliente);
void enviar_codigo(int socket_cliente, int codigo);

#endif