#include "memoria_usuario.h"
#include "contexto.h"
#include "huecos.h"
#include "memory_stick.h"
#include <utils/protocolo.h>
#include <commons/log.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>   // para send/recv al swap

static int   fd_swap            = -1;    // socket al módulo SWAP (-1 = no conectado)
static int   swap_block_size    = 0;     // tamaño de cada bloque (ej: 64)
static int   swap_total_bloques = 0;     // cuántos bloques hay en total
static bool* swap_bloques_usados = NULL; // "mapa de bits": true = ocupado, false = libre

// dónde quedó UN segmento en el swap
typedef struct {
    int      id;                // id del segmento
    uint32_t limite;            // su tamaño real (para descartar el relleno al volver)
    int      cantidad_bloques;  // cuántos bloques ocupa
    int*     bloques;           // los números de bloque que usa
} t_segmento_swap;

// un proceso suspendido y sus segmentos en swap
typedef struct {
    int     pid;
    t_list* segmentos;   // lista de t_segmento_swap*
} t_proceso_swap;

static t_list* procesos_en_swap = NULL;   // todos los procesos suspendidos

extern t_log* logger;
extern pthread_mutex_t mutex_contextos;
extern pthread_mutex_t mutex_memory_sticks;

// parametros de configuracion
int  segment_max_size    = 0;
bool allocation_best_fit = true;
int  compaction_delay_ms = 0;

void memoria_usuario_init(void) {
    procesos_en_swap = list_create();
}

// el modulo swap se conecta e informa su tamaño de bloque y total 
void swap_registrar(int fd, int block_size, int swap_size) {
    pthread_mutex_lock(&mutex_contextos);
    fd_swap = fd;
    swap_block_size = block_size;
    swap_total_bloques = block_size > 0 ? swap_size / block_size : 0;
    free(swap_bloques_usados);
    swap_bloques_usados = calloc(swap_total_bloques, sizeof(bool));
    pthread_mutex_unlock(&mutex_contextos);
}

void swap_desregistrar(void) {
    pthread_mutex_lock(&mutex_contextos);
    fd_swap = -1;
    pthread_mutex_unlock(&mutex_contextos);
}

//cuenta cuantos bloques libres quedan 
static int swap_bloques_libres(void) {
    int libres = 0;
    for (int i = 0; i < swap_total_bloques; i++) {
        if (!swap_bloques_usados[i]) libres++;
    }
    return libres;
}

static bool enviar_int(int fd, int valor) {
    return send(fd, &valor, sizeof(int), 0) == sizeof(int);
}

static bool recibir_int(int fd, int* valor) {
    return recv(fd, valor, sizeof(int), MSG_WAITALL) == sizeof(int);
}

// escribe un bloque completo en el swap: [op][nro][BLOCK_SIZE bytes] -> [OP_OK]
static bool swap_escribir_bloque(int nro_bloque, const void* contenido) {
    enviar_int(fd_swap, OP_SWAP_ESCRIBIR_BLOQUE);
    enviar_int(fd_swap, nro_bloque);
    send(fd_swap, contenido, swap_block_size, 0);

    int respuesta = 0;
    return recibir_int(fd_swap, &respuesta) && respuesta == OP_OK;
}

// lee un bloque completo del swap: [op][nro] -> [OP_OK][BLOCK_SIZE bytes]
static bool swap_leer_bloque(int nro_bloque, void* destino) {
    enviar_int(fd_swap, OP_SWAP_LEER_BLOQUE);
    enviar_int(fd_swap, nro_bloque);

    int status = 0;
    if (!recibir_int(fd_swap, &status) || status != OP_OK) return false;
    return recv(fd_swap, destino, swap_block_size, MSG_WAITALL) == (ssize_t)swap_block_size;
}

//busca un proceso supednido por PID 
static t_proceso_swap* buscar_proceso_swap(int pid) {
    for (int i = 0; i < list_size(procesos_en_swap); i++) {
        t_proceso_swap* ps = list_get(procesos_en_swap, i);
        if (ps->pid == pid) return ps;
    }
    return NULL;
}


// elige el algoritmo de huecos según el config
static t_hueco* buscar_hueco(uint32_t tamanio) {
    return allocation_best_fit
        ? buscar_hueco_best_fit(tamanio)
        : buscar_hueco_worst_fit(tamanio);
}

// busca un segmento por su ID dentro de la tabla de un proceso
static t_segmento* buscar_segmento(t_contexto* ctx, int id_segmento) {
    for (int i = 0; i < list_size(ctx->tabla_segmentos); i++) {
        t_segmento* seg = list_get(ctx->tabla_segmentos, i);
        if (seg->id == id_segmento) return seg;
    }
    return NULL;
}

//Libera un t_segmento_swap, su array de bloque + el struct 
static void destruir_segmento_swap(void* elem) {
    t_segmento_swap* ss = elem;
    free(ss->bloques);
    free(ss);
}


t_resultado_crear segmento_crear(int pid, int id_segmento, uint32_t tamanio) {
    // 1. ¿el segmento excede el tamaño máximo permitido?
    if (segment_max_size > 0 && tamanio > (uint32_t)segment_max_size) {
        log_error(logger, "PID: %d - Segmento %d de %u bytes excede SEGMENT_MAX_SIZE (%d)",
                  pid, id_segmento, tamanio, segment_max_size);
        return CREAR_SEG_ERROR;
    }

    pthread_mutex_lock(&mutex_contextos);
    t_contexto* ctx = contexto_buscar(pid);          // 2. buscar el proceso
    if (ctx == NULL) {
        pthread_mutex_unlock(&mutex_contextos);
        return CREAR_SEG_ERROR;
    }

    pthread_mutex_lock(&mutex_memory_sticks);
    t_hueco* hueco = buscar_hueco(tamanio);          // 3. Best/Worst Fit
    if (hueco == NULL) {
        // 4. no hay hueco contiguo. ¿Alcanzaría el espacio total (compactando)?
        bool alcanza_compactando = huecos_total_libre() >= tamanio;
        pthread_mutex_unlock(&mutex_memory_sticks);
        pthread_mutex_unlock(&mutex_contextos);
        return alcanza_compactando ? CREAR_SEG_COMPACTAR : CREAR_SEG_ERROR;
    }

    uint32_t base = hueco->base;
    hueco_ocupar(hueco, tamanio);                    // 5. reservar el espacio
    pthread_mutex_unlock(&mutex_memory_sticks);

    t_segmento* seg = malloc(sizeof(t_segmento));    // 6. anotar el segmento
    seg->id = id_segmento;
    seg->base = base;
    seg->limite = tamanio;
    list_add(ctx->tabla_segmentos, seg);
    pthread_mutex_unlock(&mutex_contextos);

    return CREAR_SEG_OK;
}

bool segmento_eliminar(int pid, int id_segmento) {
    pthread_mutex_lock(&mutex_contextos);
    t_contexto* ctx = contexto_buscar(pid);
    if (ctx == NULL) { pthread_mutex_unlock(&mutex_contextos); return false; }

    t_segmento* seg = buscar_segmento(ctx, id_segmento);
    if (seg == NULL) {
        pthread_mutex_unlock(&mutex_contextos);
        log_warning(logger, "PID: %d - No existe el segmento %d para eliminar", pid, id_segmento);
        return false;
    }

    list_remove_element(ctx->tabla_segmentos, seg);   // sacar de la tabla

    pthread_mutex_lock(&mutex_memory_sticks);
    hueco_liberar(seg->base, seg->limite);            // devolver el espacio como libre
    pthread_mutex_unlock(&mutex_memory_sticks);

    free(seg);
    pthread_mutex_unlock(&mutex_contextos);
    return true;
}

uint32_t memoria_libre_total(void) {
    pthread_mutex_lock(&mutex_memory_sticks);
    uint32_t libre = huecos_total_libre();
    pthread_mutex_unlock(&mutex_memory_sticks);
    return libre;
}

// para list_sort: devuelve true si 'a' va ANTES que 'b' (menor base primero)
static bool comparar_segmentos_por_base(void* a, void* b) {
    return ((t_segmento*)a)->base < ((t_segmento*)b)->base;
}

bool compactar(void) {
    pthread_mutex_lock(&mutex_contextos);

    // 1. juntar TODOS los segmentos de TODOS los procesos en una lista
    t_list* todos = list_create();
    for (int i = 0; i < list_size(lista_contextos); i++) {
        t_contexto* ctx = list_get(lista_contextos, i);
        list_add_all(todos, ctx->tabla_segmentos);
    }
    // 2. ordenarlos por dirección física (base), de menor a mayor
    list_sort(todos, comparar_segmentos_por_base);

    // 3. moverlos "aplastándolos" hacia el principio
    uint32_t destino = 0;
    bool ok = true;
    for (int i = 0; i < list_size(todos) && ok; i++){
        t_segmento* seg = list_get(todos, i);
        if (seg->base != destino) {                          // si ya está en su lugar, no lo muevo
            char* buffer = malloc(seg->limite);
           if (buffer == NULL) {
                log_error(logger, "Compactacion: no se pudo reservar el buffer del segmento %d", seg->id);
                ok = false;
                break;
            }
            // si falla la lectura o la escritura NO actualizamos seg->base:
            // el segmento se queda donde estaba y abortamos la compactacion
            if (!memoria_fisica_leer(seg->base, seg->limite, buffer) ||
                !memoria_fisica_escribir(destino, seg->limite, buffer)) {
                log_error(logger, "Compactacion: fallo de I/O moviendo el segmento %d", seg->id);
                free(buffer);
                ok = false;
                break;
            }
            free(buffer);
            seg->base = destino;                             // actualizo el mapa (la tabla)
        }
        destino += seg->limite;                              // el próximo va pegado a este
    }
    list_destroy(todos);

    if (ok) {
        pthread_mutex_lock(&mutex_memory_sticks);
        huecos_reconstruir(destino, memoria_total);
        pthread_mutex_unlock(&mutex_memory_sticks);
    
    }
    pthread_mutex_unlock(&mutex_contextos);

    // tiempo configurable antes de dar por finalizada la compactacion
    usleep(compaction_delay_ms * 1000);
    return ok;
}
bool proceso_suspender(int pid) {
    if (fd_swap == -1) {
        log_error(logger, "PID: %d - No hay SWAP conectado para suspender", pid);
        return false;
    }

    pthread_mutex_lock(&mutex_contextos);
    t_contexto* ctx = contexto_buscar(pid);
    if (ctx == NULL) {
        pthread_mutex_unlock(&mutex_contextos);
        return false;
    }

    if (buscar_proceso_swap(pid) != NULL) {
        // ya estaba suspendido
        pthread_mutex_unlock(&mutex_contextos);
        return true;
    }

    // verificar que alcancen los bloques ANTES de mover nada,
    // para no dejar al proceso a medio suspender
    int bloques_necesarios = 0;
    for (int i = 0; i < list_size(ctx->tabla_segmentos); i++) {
        t_segmento* seg = list_get(ctx->tabla_segmentos, i);
        bloques_necesarios += (seg->limite + swap_block_size - 1) / swap_block_size;
    }
    if (bloques_necesarios > swap_bloques_libres()) {
        pthread_mutex_unlock(&mutex_contextos);
        log_error(logger, "PID: %d - No hay bloques de SWAP suficientes (%d necesarios)", pid, bloques_necesarios);
        return false;
    }

    t_proceso_swap* ps = malloc(sizeof(t_proceso_swap));
    ps->pid = pid;
    ps->segmentos = list_create();

    // ===================================================================
    // FASE 1: copiar TODOS los segmentos al SWAP SIN tocar la tabla ni la
    // RAM. Recorremos con list_get (no list_remove): si algo falla a mitad
    // de camino, el proceso queda intacto en memoria y hacemos rollback de
    // los bloques de SWAP ya tomados.
    // ===================================================================
    bool copia_ok = true;

    for (int i = 0; i < list_size(ctx->tabla_segmentos) && copia_ok; i++) {
        t_segmento* seg = list_get(ctx->tabla_segmentos, i);

        t_segmento_swap* ss = malloc(sizeof(t_segmento_swap));
        ss->id = seg->id;
        ss->limite = seg->limite;
        ss->cantidad_bloques = (seg->limite + swap_block_size - 1) / swap_block_size;
        ss->bloques = malloc(ss->cantidad_bloques * sizeof(int));

        // leer el segmento completo de los sticks (la RAM sigue intacta)
        char* buffer = calloc(ss->cantidad_bloques, swap_block_size);
        if (!memoria_fisica_leer(seg->base, seg->limite, buffer)) {
            log_error(logger, "PID: %d - Fallo la lectura fisica al suspender el segmento %d", pid, seg->id);
            copia_ok = false;
        }

        // escribirlo bloque a bloque al SWAP (el ultimo va con padding)
        int asignados = 0;
        for (int b = 0; copia_ok && b < swap_total_bloques && asignados < ss->cantidad_bloques; b++) {
            if (!swap_bloques_usados[b]) {
                swap_bloques_usados[b] = true;
                ss->bloques[asignados] = b;
                if (!swap_escribir_bloque(b, buffer + asignados * swap_block_size)) {
                    log_error(logger, "PID: %d - Fallo la escritura en SWAP al suspender", pid);
                    asignados++;          // incluir el bloque fallido en el des-marcado
                    copia_ok = false;
                    break;
                }
                asignados++;
            }
        }
        free(buffer);

        if (copia_ok) {
            list_add(ps->segmentos, ss);
        } else {
            // este segmento no se pudo copiar: des-marcar sus bloques y descartarlo
            for (int k = 0; k < asignados; k++) {
                swap_bloques_usados[ss->bloques[k]] = false;
            }
            free(ss->bloques);
            free(ss);
        }
    }

    if (!copia_ok) {
        // ROLLBACK: liberar los bloques de los segmentos que SI se habian
        // copiado bien. La tabla de segmentos y la RAM siguen intactas:
        // el proceso NO se suspende.
        for (int i = 0; i < list_size(ps->segmentos); i++) {
            t_segmento_swap* ss = list_get(ps->segmentos, i);
            for (int b = 0; b < ss->cantidad_bloques; b++) {
                swap_bloques_usados[ss->bloques[b]] = false;
            }
        }
        list_destroy_and_destroy_elements(ps->segmentos, destruir_segmento_swap);
        free(ps);
        pthread_mutex_unlock(&mutex_contextos);
        log_error(logger, "PID: %d - Suspension abortada, el proceso sigue en memoria", pid);
        return false;
    }

    // ===================================================================
    // FASE 2: todo el swap-out salio bien -> recien AHORA liberamos la RAM.
    // ===================================================================
    pthread_mutex_lock(&mutex_memory_sticks);
    for (int i = 0; i < list_size(ctx->tabla_segmentos); i++) {
        t_segmento* seg = list_get(ctx->tabla_segmentos, i);
        hueco_liberar(seg->base, seg->limite);
    }
    pthread_mutex_unlock(&mutex_memory_sticks);
    list_clean_and_destroy_elements(ctx->tabla_segmentos, free);

    list_add(procesos_en_swap, ps);
    pthread_mutex_unlock(&mutex_contextos);

    log_info(logger, "PID: %d - Proceso suspendido (%d bloques de SWAP)", pid, bloques_necesarios);
    return true;
}

bool proceso_dessuspender(int pid) {

    pthread_mutex_lock(&mutex_contextos);

    t_proceso_swap* ps = buscar_proceso_swap(pid);
    if (ps == NULL) {
        // el proceso no tenia segmentos en swap (p. ej. nunca hizo MEM_ALLOC)
        pthread_mutex_unlock(&mutex_contextos);
        return true;
    }

    t_contexto* ctx = contexto_buscar(pid);
    if (ctx == NULL) {
        pthread_mutex_unlock(&mutex_contextos);
        return false;
    }

    int cantidad = list_size(ps->segmentos);

    // asignacion tentativa: todos los segmentos tienen que entrar
    // SIN disparar una compactacion; si alguno no entra, rollback.
    uint32_t* bases = malloc(cantidad * sizeof(uint32_t));
    pthread_mutex_lock(&mutex_memory_sticks);
    for (int i = 0; i < cantidad; i++) {
        t_segmento_swap* ss = list_get(ps->segmentos, i);
        t_hueco* hueco = buscar_hueco(ss->limite);
        if (hueco == NULL) {
            // rollback de lo asignado tentativamente
            for (int j = 0; j < i; j++) {
                t_segmento_swap* previo = list_get(ps->segmentos, j);
                hueco_liberar(bases[j], previo->limite);
            }
            pthread_mutex_unlock(&mutex_memory_sticks);
            pthread_mutex_unlock(&mutex_contextos);
            free(bases);
            return false;
        }
        bases[i] = hueco->base;
        hueco_ocupar(hueco, ss->limite);
    }
    pthread_mutex_unlock(&mutex_memory_sticks);

    // restaurar los datos desde SWAP y regenerar la tabla de segmentos
    for (int i = 0; i < cantidad; i++) {
        t_segmento_swap* ss = list_get(ps->segmentos, i);

        char* buffer = calloc(ss->cantidad_bloques, swap_block_size);
        for (int b = 0; b < ss->cantidad_bloques; b++) {
            swap_leer_bloque(ss->bloques[b], buffer + b * swap_block_size);
            swap_bloques_usados[ss->bloques[b]] = false;
        }
        memoria_fisica_escribir(bases[i], ss->limite, buffer);
        free(buffer);

        t_segmento* seg = malloc(sizeof(t_segmento));
        seg->id = ss->id;
        seg->base = bases[i];
        seg->limite = ss->limite;
        list_add(ctx->tabla_segmentos, seg);
    }
    free(bases);

    list_remove_element(procesos_en_swap, ps);
    list_destroy_and_destroy_elements(ps->segmentos, destruir_segmento_swap);
    free(ps);

    pthread_mutex_unlock(&mutex_contextos);

    log_info(logger, "PID: %d - Proceso des-suspendido", pid);
    return true;
}

void proceso_liberar_memoria(int pid) {
    pthread_mutex_lock(&mutex_contextos);

    // segmentos en memoria principal
    t_contexto* ctx = contexto_buscar(pid);
    if (ctx != NULL) {
        pthread_mutex_lock(&mutex_memory_sticks);
        for (int i = 0; i < list_size(ctx->tabla_segmentos); i++) {
            t_segmento* seg = list_get(ctx->tabla_segmentos, i);
            hueco_liberar(seg->base, seg->limite);
        }
        pthread_mutex_unlock(&mutex_memory_sticks);
        list_clean_and_destroy_elements(ctx->tabla_segmentos, free);
    }

    // segmentos que hubieran quedado en SWAP
    t_proceso_swap* ps = buscar_proceso_swap(pid);
    if (ps != NULL) {
        for (int i = 0; i < list_size(ps->segmentos); i++) {
            t_segmento_swap* ss = list_get(ps->segmentos, i);
            for (int b = 0; b < ss->cantidad_bloques; b++) {
                swap_bloques_usados[ss->bloques[b]] = false;
            }
        }
        list_remove_element(procesos_en_swap, ps);
        list_destroy_and_destroy_elements(ps->segmentos, destruir_segmento_swap);
        free(ps);
    }

    pthread_mutex_unlock(&mutex_contextos);
}
