#define _GNU_SOURCE
#include "handlers.h"
#include "contexto.h"
#include "servidor.h"
#include "memory_stick.h"
#include "memoria_usuario.h"
#include <utils/protocolo.h>
#include <commons/log.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>

#ifndef POLLRDHUP
#define POLLRDHUP POLLHUP
#endif

// Espera a que el otro extremo cierre la conexion SIN consumir bytes del
// socket. Es clave para los fds de stick y SWAP: sobre esos mismos fds el
// hilo que atiende al KS hace pedidos y espera respuestas, asi que aca no
// se puede hacer recv() (robaria las respuestas).
static void esperar_desconexion(int fd) {
    struct pollfd pfd = { .fd = fd, .events = POLLRDHUP };
    while (1) {
        int r = poll(&pfd, 1, -1);
        if (r < 0) continue;
        if (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) return;
    }
}

extern t_log* logger;
extern pthread_mutex_t mutex_contextos;
extern pthread_mutex_t mutex_memory_sticks;

int instruction_delay_ms = 0;   // demora antes de contestar un FETCH (del config)

// socket dedicado para avisarle cosas al KS (memoria disponible, compactación, etc.)
static int fd_ks_notificaciones = -1;
static pthread_mutex_t mutex_notificaciones = PTHREAD_MUTEX_INITIALIZER;

// le manda una notificación al KS (solo si el canal está conectado)
static void notificar_ks(int codigo) {
    pthread_mutex_lock(&mutex_notificaciones);
    if (fd_ks_notificaciones != -1) {
        enviar_codigo(fd_ks_notificaciones, codigo);
    }
    pthread_mutex_unlock(&mutex_notificaciones);
}

// IP del otro extremo de un socket (para informar los sticks a las CPUs)
static void obtener_ip_cliente(int fd, char* buffer, size_t tam) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    snprintf(buffer, tam, "127.0.0.1");
    if (getpeername(fd, (struct sockaddr*)&addr, &len) == 0) {
        if (addr.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in*)&addr)->sin_addr, buffer, tam);
        } else if (addr.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6*)&addr)->sin6_addr, buffer, tam);
        }
    }
}

void* atender_cliente(void* arg) {
    int fd = (int)(intptr_t)arg;

    int codigo = recibir_codigo(fd);

    if (codigo == HANDSHAKE_CPU) {
        int id_cpu = 0;
        recv(fd, &id_cpu, sizeof(int), MSG_WAITALL);
        enviar_codigo(fd, HANDSHAKE_OK);
        log_info(logger, "## CPU %d Conectada", id_cpu);
        atender_cpu(fd);

    } else if (codigo == HANDSHAKE_KERNEL_SCHEDULER) {
        enviar_codigo(fd, HANDSHAKE_OK);
        log_info(logger, "## Kernel Scheduler Conectado - FD del socket: %d", fd);
        atender_ks(fd);

    } else if (codigo == HANDSHAKE_KS_NOTIFICACIONES) {
        enviar_codigo(fd, HANDSHAKE_OK);
        pthread_mutex_lock(&mutex_notificaciones);
        fd_ks_notificaciones = fd;
        pthread_mutex_unlock(&mutex_notificaciones);
        log_info(logger, "Canal de notificaciones del Kernel Scheduler conectado - FD: %d", fd);

        // Este socket solo lo escribe el KM. Nos quedamos bloqueados para
        // detectar la desconexion del KS.
        while (recibir_codigo(fd) != -1) { }
        pthread_mutex_lock(&mutex_notificaciones);
        fd_ks_notificaciones = -1;
        pthread_mutex_unlock(&mutex_notificaciones);

    } else if (codigo == HANDSHAKE_MEMORY_STICK) {
        enviar_codigo(fd, HANDSHAKE_OK);

        uint32_t tamanio = 0;
        int puerto = 0;
        recv(fd, &tamanio, sizeof(uint32_t), MSG_WAITALL);
        recv(fd, &puerto,  sizeof(int),      MSG_WAITALL);

        char ip[64];
        obtener_ip_cliente(fd, ip, sizeof(ip));

        pthread_mutex_lock(&mutex_memory_sticks);
        memory_stick_agregar(fd, tamanio, ip, puerto);
        pthread_mutex_unlock(&mutex_memory_sticks);

        log_info(logger, "## Memory Stick de %u bytes Conectada", tamanio);

        // Se amplio la memoria total: avisar al KS que hay mas memoria.
        notificar_ks(OP_MEMORIA_DISPONIBLE);

        atender_memory_stick(fd);

    } else if (codigo == HANDSHAKE_SWAP) {
        enviar_codigo(fd, HANDSHAKE_OK);

        int block_size = 0;
        int swap_size  = 0;
        recv(fd, &block_size, sizeof(int), MSG_WAITALL);
        recv(fd, &swap_size,  sizeof(int), MSG_WAITALL);

        swap_registrar(fd, block_size, swap_size);
        log_info(logger, "## SWAP Conectado - Tamanio: %d bytes - Bloque: %d bytes", swap_size, block_size);

        // El fd del SWAP lo usa el hilo que atiende al KS (suspension /
        // des-suspension). Este hilo solo espera la desconexion, sin
        // consumir bytes del socket.
        esperar_desconexion(fd);
        swap_desregistrar();
        log_warning(logger, "SWAP desconectado");

    } else {
        log_warning(logger, "Cliente desconocido: %d", codigo);
        enviar_codigo(fd, HANDSHAKE_ERROR);
    }

    close(fd);
    return NULL;
}

void atender_ks(int fd) {
    while (1) {
        int op = recibir_codigo(fd);
        if (op == -1) break;   // el KS se desconectó

        if (op == OP_CREAR_PROCESO) {
            int pid;
            recv(fd, &pid, sizeof(int), MSG_WAITALL);
            // el path llega como [len][bytes] sin terminador
            int len;
            recv(fd, &len, sizeof(int), MSG_WAITALL);
            char* path = malloc(len + 1);
            recv(fd, path, len, MSG_WAITALL);
            path[len] = '\0';

            pthread_mutex_lock(&mutex_contextos);
            contexto_crear(pid, path);
            pthread_mutex_unlock(&mutex_contextos);

            log_info(logger, "## PID: %d - Proceso Creado", pid);
            free(path);
            enviar_codigo(fd, OP_OK);

        } else if (op == OP_FINALIZAR_PROCESO) {
            int pid;
            recv(fd, &pid, sizeof(int), MSG_WAITALL);

            proceso_liberar_memoria(pid);
            pthread_mutex_lock(&mutex_contextos);
            contexto_eliminar(pid);
            pthread_mutex_unlock(&mutex_contextos);

            // El KS espera confirmacion (manejar_proceso_exit): respondemos ANTES
            // de notificar, para que libere mutex_pedidos_memoria y el hilo de
            // notificaciones pueda de-suspender sin deadlock.
            enviar_codigo(fd, OP_OK);

            notificar_ks(OP_MEMORIA_DISPONIBLE);   // se liberó memoria

        } else if (op == OP_CREAR_SEGMENTO) {
            int pid, id_segmento, tamanio;
            recv(fd, &pid,         sizeof(int), MSG_WAITALL);
            recv(fd, &id_segmento, sizeof(int), MSG_WAITALL);
            recv(fd, &tamanio,     sizeof(int), MSG_WAITALL);

            t_resultado_crear resultado = segmento_crear(pid, id_segmento, tamanio);

            if (resultado == CREAR_SEG_OK) {
                log_info(logger, "## PID: %d - Segmento Creado %d - Tamaño: %d", pid, id_segmento, tamanio);
                enviar_codigo(fd, OP_OK);

            } else if (resultado == CREAR_SEG_COMPACTAR) {
                // hay espacio total pero no contiguo: el KS desaloja y compactamos
                enviar_codigo(fd, OP_COMPACTAR);

                int confirmacion = recibir_codigo(fd);
                if (confirmacion != OP_OK) {
                    log_error(logger, "El KS no confirmo el desalojo para compactar (%d)", confirmacion);
                    continue;
                }

                log_info(logger, "## Inicio de compactación");
                compactar();
                log_info(logger, "## Fin de compactación");

                resultado = segmento_crear(pid, id_segmento, tamanio);   // ahora entra
                if (resultado == CREAR_SEG_OK) {
                    log_info(logger, "## PID: %d - Segmento Creado %d - Tamaño: %d", pid, id_segmento, tamanio);
                } else {
                    log_error(logger, "PID: %d - No se pudo crear el segmento %d ni compactando", pid, id_segmento);
                }

                notificar_ks(OP_FIN_COMPACTACION);
                notificar_ks(OP_MEMORIA_DISPONIBLE);

            } else {
                enviar_codigo(fd, OP_ERROR);
            }

        } else if (op == OP_ELIMINAR_SEGMENTO) {
            int pid, id_segmento;
            recv(fd, &pid,         sizeof(int), MSG_WAITALL);
            recv(fd, &id_segmento, sizeof(int), MSG_WAITALL);

            bool ok = segmento_eliminar(pid, id_segmento);
            enviar_codigo(fd, ok ? OP_OK : OP_ERROR);
            if (ok) notificar_ks(OP_MEMORIA_DISPONIBLE);

        } else if (op == OP_SUSPENDER_PROCESO) {
            int pid;
            recv(fd, &pid, sizeof(int), MSG_WAITALL);

            bool ok = proceso_suspender(pid);
            // El KS (hilo_suspension) espera confirmacion: respondemos ANTES de
            // notificar para liberar mutex_pedidos_memoria y evitar deadlock.
            enviar_codigo(fd, ok ? OP_OK : OP_ERROR);
            if (ok) notificar_ks(OP_MEMORIA_DISPONIBLE);

        } else if (op == OP_DESSUSPENDER_PROCESO) {
            int pid;
            recv(fd, &pid, sizeof(int), MSG_WAITALL);

            bool ok = proceso_dessuspender(pid);
            enviar_codigo(fd, ok ? OP_OK : OP_ERROR);

        } else if (op == OP_ESCRIBIR_DATOS) {
            // STDIN: [pid][dir fisica][tamanio][contenido] -> [OP_OK]
            int pid, tamanio;
            uint32_t direccion_fisica;
            recv(fd, &pid,              sizeof(int),      MSG_WAITALL);
            recv(fd, &direccion_fisica, sizeof(uint32_t), MSG_WAITALL);
            recv(fd, &tamanio,          sizeof(int),      MSG_WAITALL);

            char* contenido = malloc(tamanio);
            recv(fd, contenido, tamanio, MSG_WAITALL);

            bool ok = memoria_fisica_escribir(direccion_fisica, tamanio, contenido);
            free(contenido);

            log_info(logger, "## PID: %d - Escritura - Dir. Física: %u - Tamaño: %d", pid, direccion_fisica, tamanio);
            enviar_codigo(fd, ok ? OP_OK : OP_ERROR);

        } else if (op == OP_LEER_DATOS) {
            // STDOUT: [pid][dir fisica][tamanio] -> [OP_OK][contenido]
            int pid, tamanio;
            uint32_t direccion_fisica;
            recv(fd, &pid,              sizeof(int),      MSG_WAITALL);
            recv(fd, &direccion_fisica, sizeof(uint32_t), MSG_WAITALL);
            recv(fd, &tamanio,          sizeof(int),      MSG_WAITALL);

            char* contenido = malloc(tamanio);
            bool ok = memoria_fisica_leer(direccion_fisica, tamanio, contenido);

            log_info(logger, "## PID: %d - Lectura - Dir. Física: %u - Tamaño: %d", pid, direccion_fisica, tamanio);

            if (ok) {
                enviar_codigo(fd, OP_OK);
                send(fd, contenido, tamanio, 0);
            } else {
                enviar_codigo(fd, OP_ERROR);
            }
            free(contenido);

        } else if (op == OP_MEMORIA_LISTA) {
            int memoria_libre = (int) memoria_libre_total();
            send(fd, &memoria_libre, sizeof(int), 0);

        } else {
            log_warning(logger, "Operacion desconocida del Kernel Scheduler: %d", op);
        }
    }
}

void atender_cpu(int fd) {
    while (1) {
        int op = recibir_codigo(fd);
        if (op == -1) break;

        if (op == OP_FETCH) {
            int pid, pc;
            recv(fd, &pid, sizeof(int), MSG_WAITALL);
            recv(fd, &pc,  sizeof(int), MSG_WAITALL);

            // tiempo configurable antes de contestar el pedido
            if (instruction_delay_ms > 0) usleep(instruction_delay_ms * 1000);

            pthread_mutex_lock(&mutex_contextos);
            t_contexto* ctx = contexto_buscar(pid);
            if (ctx == NULL || pc < 0 || pc >= list_size(ctx->instrucciones)) {
                pthread_mutex_unlock(&mutex_contextos);
                enviar_codigo(fd, OP_ERROR);
                continue;
            }
            char* instruccion = strdup(list_get(ctx->instrucciones, pc));
            pthread_mutex_unlock(&mutex_contextos);

            // respuesta: [OP_OK][len][bytes] (sin terminador)
            enviar_codigo(fd, OP_OK);
            int len = strlen(instruccion);
            send(fd, &len, sizeof(int), 0);
            send(fd, instruccion, len, 0);

            log_info(logger, "## PID: %d - Obtener instrucción: %d - Instrucción: %s", pid, pc, instruccion);
            free(instruccion);

        } else if (op == OP_OBTENER_CONTEXTO) {
            int pid;
            recv(fd, &pid, sizeof(int), MSG_WAITALL);

            pthread_mutex_lock(&mutex_contextos);
            t_contexto* ctx = contexto_buscar(pid);
            if (ctx == NULL) {
                pthread_mutex_unlock(&mutex_contextos);
                log_error(logger, "OP_OBTENER_CONTEXTO: no existe el PID %d", pid);
                enviar_codigo(fd, OP_ERROR);
                continue;
            }

            // respuesta: [OP_OK][registros][cantidad segmentos][id|base|limite...]
            enviar_codigo(fd, OP_OK);
            send(fd, &ctx->registros, sizeof(t_registros), 0);

            int cantidad = list_size(ctx->tabla_segmentos);
            send(fd, &cantidad, sizeof(int), 0);
            for (int i = 0; i < cantidad; i++) {
                t_segmento* seg = list_get(ctx->tabla_segmentos, i);
                send(fd, &seg->id,     sizeof(int),      0);
                send(fd, &seg->base,   sizeof(uint32_t), 0);
                send(fd, &seg->limite, sizeof(uint32_t), 0);
            }
            pthread_mutex_unlock(&mutex_contextos);

        } else if (op == OP_GUARDAR_CONTEXTO) {
            int pid;
            recv(fd, &pid, sizeof(int), MSG_WAITALL);

            t_registros registros;
            recv(fd, &registros, sizeof(t_registros), MSG_WAITALL);

            pthread_mutex_lock(&mutex_contextos);
            contexto_guardar(pid, registros);
            pthread_mutex_unlock(&mutex_contextos);

            enviar_codigo(fd, OP_OK);

        } else if (op == OP_LISTA_STICKS) {
            // respuesta: [cantidad] y por stick [len_ip][ip][puerto][tamanio]
            pthread_mutex_lock(&mutex_memory_sticks);
            int cantidad = list_size(lista_memory_sticks);
            send(fd, &cantidad, sizeof(int), 0);
            for (int i = 0; i < cantidad; i++) {
                t_memory_stick* stick = list_get(lista_memory_sticks, i);
                int len_ip = strlen(stick->ip);
                send(fd, &len_ip, sizeof(int), 0);
                send(fd, stick->ip, len_ip, 0);
                send(fd, &stick->puerto, sizeof(int), 0);
                send(fd, &stick->tamanio, sizeof(uint32_t), 0);
            }
            pthread_mutex_unlock(&mutex_memory_sticks);

        } else {
            log_warning(logger, "Operacion desconocida de CPU: %d", op);
        }
    }
}

void atender_memory_stick(int fd) {
    // Sobre este fd el hilo del KS hace las lecturas/escrituras fisicas
    // (memoria_fisica_*), asi que aca NO se puede hacer recv: solo se
    // espera la desconexion.
    esperar_desconexion(fd);

    // el Memory Stick se desconecto en medio de la ejecucion:
    // la memoria queda corrupta y el KS debe finalizar con BSOD.
    pthread_mutex_lock(&mutex_memory_sticks);
    memory_stick_eliminar(fd);
    pthread_mutex_unlock(&mutex_memory_sticks);

    log_error(logger, "## Memory Stick desconectado - Memoria corrupta - FD: %d", fd);
    notificar_ks(OP_MEMORIA_CORRUPTA);
}