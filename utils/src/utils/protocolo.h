#ifndef PROTOCOLO_H
#define PROTOCOLO_H

#include <stdint.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

/*
    ============================================================
    PROTOCOLO COMPARTIDO — fuente única de verdad
    ============================================================
    Todos los módulos deben incluir este header en lugar de
    definir su propio enum. Así los códigos nunca se desincronisan.

    Incluir con:
        #include <utils/protocolo.h>
*/
static inline void socket_set_nodelay(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}
typedef enum
{
    // ── Handshakes ───────────────────────────────────────────
    HANDSHAKE_KERNEL_SCHEDULER        = 1,
    HANDSHAKE_CPU                     = 2,
    HANDSHAKE_MEMORY_STICK            = 3,
    HANDSHAKE_SWAP                    = 4,
    HANDSHAKE_IO                      = 5,
    HANDSHAKE_KS_NOTIFICACIONES       = 6,
    HANDSHAKE_OK                      = 100,
    HANDSHAKE_ERROR                   = 101,

    // ── CPU ↔ Kernel Memory ──────────────────────────────────
    OP_FETCH                          = 200,
    OP_OBTENER_CONTEXTO               = 201,
    OP_GUARDAR_CONTEXTO               = 202,

    // ── CPU / Kernel Memory ↔ Memory Stick ─────────────────z─────────
    OP_LEER_MEMORIA                   = 210,
    OP_ESCRIBIR_MEMORIA               = 211,
    OP_LISTA_STICKS                   = 212,

    // ── Kernel Scheduler ↔ Kernel Memory ────────────────────
    OP_CREAR_PROCESO                  = 300,
    OP_FINALIZAR_PROCESO              = 301,
    OP_SUSPENDER_PROCESO              = 302,
    OP_DESSUSPENDER_PROCESO           = 303,
    OP_MEMORIA_DISPONIBLE             = 304,
    OP_COMPACTAR                      = 305,
    OP_FIN_COMPACTACION               = 306,
    OP_CREAR_SEGMENTO                 = 307,
    OP_ELIMINAR_SEGMENTO              = 308,
    OP_LEER_DATOS                     = 309,
    OP_ESCRIBIR_DATOS                 = 310,
    OP_MEMORIA_LISTA                  = 400,
    OP_MEMORIA_CORRUPTA               = 401,

    // ── Kernel Memory ↔ SWAP ─────────────────────────────────
    // Las operaciones son siempre de a 1 bloque (BLOCK_SIZE bytes).
    OP_SWAP_LEER_BLOQUE               = 320,
    OP_SWAP_ESCRIBIR_BLOQUE           = 321,

    // ── Kernel Scheduler ↔ CPU ───────────────────────────────
    OP_EJECUTAR_PROCESO               = 600,
    OP_INTERRUMPIR_PROCESO            = 601,
    OP_PROCESO_EXIT                   = 602,
    OP_PROCESO_SYSCALL                = 603,
    OP_PROCESO_FIN_QUANTUM            = 604,
    OP_MUTEX_CREATE                   = 608,
    OP_MUTEX_LOCK                     = 609,
    OP_MUTEX_UNLOCK                   = 610,
    OP_INIT_PROC                      = 611,
    OP_MEM_ALLOC                      = 612,
    OP_MEM_FREE                       = 613,  
    OP_PROCESO_SEG_FAULT              = 614,

    // ── Kernel Scheduler ↔ IO ────────────────────────────────
    OP_IO_SLEEP                       = 605,
    OP_IO_STDIN                       = 606,
    OP_IO_STDOUT                      = 607,

    // ── Respuestas genéricas ─────────────────────────────────
    OP_OK                             = 500,
    OP_ERROR                          = 501,
    OP_BLOQUEADO                      = 502,

} op_code;

// Segmento de un proceso.
typedef struct
{
    int id;
    uint32_t base;   // dirección física global donde arranca (no relativa a un stick).
    uint32_t limite; // tamaño del segmento en bytes.
} t_segmento;

#endif /* PROTOCOLO_H */
