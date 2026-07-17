#include "ciclo_instruccion.h"
#include "cpu.h"
#include "instrucciones.h"
#include "contexto.h"
#include "mmu.h"
#include "memoria.h"

#include <commons/log.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <utils/protocolo.h>

// Funciones de serializacion implementadas en main.c
bool enviar_entero(int socket, int valor);
bool recibir_entero(int socket, int* valor_recibido);
bool enviar_string(int socket, const char* string);

extern t_log* logger;
extern t_registros_cpu registros;
extern int socket_km;
extern int socket_ks;

static int interrupcion_pendiente_pid = -1;

/*
    Esta función sirve para mirar si un socket tiene datos disponibles
    sin quedarnos bloqueados esperando.
*/
static bool socket_tiene_datos(int socket, int timeout_ms) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(socket, &fds);

    struct timeval timeout;
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    int resultado = select(socket + 1, &fds, NULL, NULL, &timeout);

    return resultado > 0 && FD_ISSET(socket, &fds);
}

static bool recibir_exactamente_cpu(int socket_fd, void* buffer, uint32_t tamanio) {
    uint32_t recibidos_totales = 0;

    while (recibidos_totales < tamanio) {
        int recibidos = recv(
            socket_fd,
            (char*) buffer + recibidos_totales,
            tamanio - recibidos_totales,
            0
        );

        if (recibidos <= 0) {
            return false;
        }

        recibidos_totales += recibidos;
    }

    return true;
}

/*
    Kernel Memory actual responde FETCH así:

        [OP_OK][largo][bytes instrucción]

    o, si falla:

        [OP_ERROR]

    Por eso NO podemos usar recibir_string(socket_km) directamente,
    porque recibir_string espera que lo primero sea el largo del string.
*/
static char* recibir_instruccion_desde_memoria(void) {
    int respuesta = 0;

    if (!recibir_entero(socket_km, &respuesta)) {
        log_error(logger, "No se pudo recibir respuesta de FETCH desde Kernel Memory");
        return NULL;
    }

    if (respuesta != OP_OK) {
        log_error(logger, "Kernel Memory respondio error en FETCH");
        return NULL;
    }

    int tamanio = 0;

    if (!recibir_entero(socket_km, &tamanio)) {
        log_error(logger, "No se pudo recibir tamanio de instruccion");
        return NULL;
    }

    if (tamanio <= 0 || tamanio > 10000) {
        log_error(logger, "Tamanio de instruccion invalido: %d", tamanio);
        return NULL;
    }

    char* instruccion = malloc(tamanio + 1);

    if (instruccion == NULL) {
        log_error(logger, "No se pudo reservar memoria para la instruccion");
        return NULL;
    }

    if (!recibir_exactamente_cpu(socket_km, instruccion, tamanio)) {
        free(instruccion);
        log_error(logger, "No se pudo recibir la instruccion completa");
        return NULL;
    }

    instruccion[tamanio] = '\0';

    return instruccion;
}

static void enviar_syscall_a_scheduler(uint32_t pid, int tipo_syscall) {
    enviar_entero(socket_ks, OP_PROCESO_SYSCALL);
    enviar_entero(socket_ks, (int) pid);
    enviar_entero(socket_ks, tipo_syscall);
}

static bool recibir_respuesta_scheduler(int* respuesta) {
    while (true) {
        int codigo = 0;

        if (!recibir_entero(socket_ks, &codigo)) {
            return false;
        }

        if (codigo == OP_INTERRUMPIR_PROCESO) {
            int pid_interrumpido = 0;

            if (!recibir_entero(socket_ks, &pid_interrumpido)) {
                return false;
            }

            interrupcion_pendiente_pid = pid_interrumpido;
            continue;
        }

        *respuesta = codigo;
        return true;
    }
}

static uint32_t tamanio_registro(char* nombre_registro) {
    if (nombre_registro == NULL) {
        return 0;
    }

    if (
        strcmp(nombre_registro, "AX") == 0 ||
        strcmp(nombre_registro, "BX") == 0 ||
        strcmp(nombre_registro, "CX") == 0 ||
        strcmp(nombre_registro, "DX") == 0
    ) {
        return 1;
    }

    if (
        strcmp(nombre_registro, "EAX") == 0 ||
        strcmp(nombre_registro, "EBX") == 0 ||
        strcmp(nombre_registro, "ECX") == 0 ||
        strcmp(nombre_registro, "EDX") == 0 ||
        strcmp(nombre_registro, "SI") == 0 ||
        strcmp(nombre_registro, "DI") == 0 ||
        strcmp(nombre_registro, "PC") == 0
    ) {
        return 4;
    }

    return 0;
}

static t_motivo_desalojo ejecutar_mov_in(uint32_t pid, char* registro_destino) {
    if (registro_destino == NULL) {
        log_error(logger, "MOV_IN sin registro destino");
        return CPU_MOTIVO_ERROR;
    }

    uint32_t tamanio = tamanio_registro(registro_destino);

    if (tamanio == 0) {
        log_error(logger, "MOV_IN con registro invalido: %s", registro_destino);
        return CPU_MOTIVO_ERROR;
    }

    uint32_t direccion_logica = registros.SI;
    uint32_t direccion_fisica = 0;

    if (!traducir_direccion(pid, direccion_logica, tamanio, &direccion_fisica)) {
        return CPU_MOTIVO_SEG_FAULT;
    }

    uint32_t valor = 0;

    if (!leer_memoria(direccion_fisica, tamanio, &valor)) {
        return CPU_MOTIVO_ERROR;
    }

    setear_valor_registro(registro_destino, valor);

    log_info(
        logger,
        "PID: %d - Acción: LEER - Dirección Física: %d - Valor: %d",
        pid,
        direccion_fisica,
        valor
    );

    registros.PC++;
    return CPU_MOTIVO_CONTINUAR;
}


static t_motivo_desalojo ejecutar_mov_out(uint32_t pid, char* registro_origen) {
    if (registro_origen == NULL) {
        log_error(logger, "MOV_OUT sin registro origen");
        return CPU_MOTIVO_ERROR;
    }

    uint32_t tamanio = tamanio_registro(registro_origen);

    if (tamanio == 0) {
        log_error(logger, "MOV_OUT con registro invalido: %s", registro_origen);
        return CPU_MOTIVO_ERROR;
    }

    uint32_t direccion_logica = registros.DI;
    uint32_t direccion_fisica = 0;

    if (!traducir_direccion(pid, direccion_logica, tamanio, &direccion_fisica)) {
        return CPU_MOTIVO_SEG_FAULT;
    }

    uint32_t valor = obtener_valor_registro(registro_origen);

    if (!escribir_memoria(direccion_fisica, tamanio, &valor)) {
        return CPU_MOTIVO_ERROR;
    }

    log_info(
        logger,
        "PID: %d - Acción: ESCRIBIR - Dirección Física: %d - Valor: %d",
        pid,
        direccion_fisica,
        valor
    );

    registros.PC++;
    return CPU_MOTIVO_CONTINUAR;
}

static t_motivo_desalojo ejecutar_copy_mem(uint32_t pid, char* registro_tamanio) {
    if (registro_tamanio == NULL) {
        log_error(logger, "COPY_MEM sin registro tamanio");
        return CPU_MOTIVO_ERROR;
    }

    if (tamanio_registro(registro_tamanio) == 0) {
        log_error(logger, "COPY_MEM con registro invalido: %s", registro_tamanio);
        return CPU_MOTIVO_ERROR;
    }

    uint32_t tamanio = obtener_valor_registro(registro_tamanio);

    if (tamanio == 0) {
        registros.PC++;
        return CPU_MOTIVO_CONTINUAR;
    }

    uint32_t direccion_logica_origen = registros.SI;
    uint32_t direccion_logica_destino = registros.DI;

    uint32_t direccion_fisica_origen = 0;
    uint32_t direccion_fisica_destino = 0;

    if (!traducir_direccion(pid, direccion_logica_origen, tamanio, &direccion_fisica_origen)) {
        return CPU_MOTIVO_SEG_FAULT;
    }

    if (!traducir_direccion(pid, direccion_logica_destino, tamanio, &direccion_fisica_destino)) {
        return CPU_MOTIVO_SEG_FAULT;
    }

    void* buffer = malloc(tamanio);

    if (buffer == NULL) {
        log_error(logger, "COPY_MEM no pudo reservar buffer de %d bytes", tamanio);
        return CPU_MOTIVO_ERROR;
    }

    if (!leer_memoria(direccion_fisica_origen, tamanio, buffer)) {
        free(buffer);
        return CPU_MOTIVO_ERROR;
    }

    if (!escribir_memoria(direccion_fisica_destino, tamanio, buffer)) {
        free(buffer);
        return CPU_MOTIVO_ERROR;
    }

    free(buffer);

    log_info(
        logger,
        "PID: %d - Acción: COPY_MEM - Origen DF: %d - Destino DF: %d - Tamaño: %d",
        pid,
        direccion_fisica_origen,
        direccion_fisica_destino,
        tamanio
    );

    registros.PC++;
    return CPU_MOTIVO_CONTINUAR;
}

/*
    SLEEP es bloqueante.

    CPU debe:
    1. Incrementar PC.
    2. Guardar contexto.
    3. Avisar syscall al Scheduler.
    4. Mandar tiempo.
    5. Dejar de ejecutar el proceso.

    El Scheduler se encarga de pasar EXEC -> BLOCK.
*/
static t_motivo_desalojo ejecutar_syscall_sleep(uint32_t pid, char* tiempo_str) {
    if (tiempo_str == NULL) {
        log_error(logger, "SLEEP sin parametro tiempo");
        return CPU_MOTIVO_ERROR;
    }
    registros.PC++;

    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    int tiempo = atoi(tiempo_str);

    enviar_syscall_a_scheduler(pid, OP_IO_SLEEP);
    enviar_entero(socket_ks, tiempo);

    return CPU_MOTIVO_SYSCALL_BLOQUEANTE;
}

/*
    INIT_PROC no bloquea al proceso actual.
    Crea un nuevo proceso y el proceso actual puede seguir ejecutando.
*/
static t_motivo_desalojo ejecutar_syscall_init_proc(uint32_t pid, char* path, char* prioridad_str) {
    if (path == NULL || prioridad_str == NULL) {
        log_error(logger, "INIT_PROC con parametros invalidos");
        return CPU_MOTIVO_ERROR;
    }

    registros.PC++;
    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    int prioridad = atoi(prioridad_str);

    enviar_syscall_a_scheduler(pid, OP_INIT_PROC);
    enviar_string(socket_ks, path);
    enviar_entero(socket_ks, prioridad);

    int respuesta = 0;

    if (!recibir_respuesta_scheduler(&respuesta)) {
        log_error(logger, "No se recibio respuesta de INIT_PROC");
        return CPU_MOTIVO_ERROR;
    }

    if (respuesta != OP_OK) {
        log_error(logger, "INIT_PROC respondio error");
        return CPU_MOTIVO_ERROR;
    }

    return CPU_MOTIVO_CONTINUAR;
}

/*
    MUTEX_CREATE no bloquea.
    El Scheduler crea el mutex y responde OP_OK.
*/
static t_motivo_desalojo ejecutar_syscall_mutex_create(uint32_t pid, char* nombre_mutex) {
    if (nombre_mutex == NULL) {
        log_error(logger, "MUTEX_CREATE sin nombre");
        return CPU_MOTIVO_ERROR;
    }
    registros.PC++;
    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    enviar_syscall_a_scheduler(pid, OP_MUTEX_CREATE);
    enviar_string(socket_ks, nombre_mutex);

    int respuesta = 0;

    if (!recibir_respuesta_scheduler(&respuesta)) {
        log_error(logger, "No se recibio respuesta de MUTEX_CREATE");
        return CPU_MOTIVO_ERROR;
    }

    if (respuesta != OP_OK) {
        log_error(logger, "MUTEX_CREATE respondio error");
        return CPU_MOTIVO_ERROR;
    }

    return CPU_MOTIVO_CONTINUAR;
}

/*
    MUTEX_LOCK puede o no bloquear.

    Caso A:
    - El mutex está libre.
    - Scheduler responde OP_OK.
    - CPU sigue ejecutando.

    Caso B:
    - El mutex está ocupado.
    - Scheduler bloquea el proceso.
    - Scheduler NO responde OP_OK.
    - CPU deja de ejecutar este PID.
*/
static t_motivo_desalojo ejecutar_syscall_mutex_lock(uint32_t pid, char* nombre_mutex) {
    if (nombre_mutex == NULL) {
        log_error(logger, "MUTEX_LOCK sin nombre");
        return CPU_MOTIVO_ERROR;
    }

    registros.PC++;
    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    enviar_syscall_a_scheduler(pid, OP_MUTEX_LOCK);
    enviar_string(socket_ks, nombre_mutex);

    int respuesta = 0;

    /*
        Si el mutex estaba libre, el Scheduler responde rápido OP_OK.

        Si no llega respuesta, interpretamos que el Scheduler bloqueó
        al proceso porque el mutex estaba ocupado.
    */
    if (!recibir_respuesta_scheduler(&respuesta)) {
        log_error(logger, "No se recibio respuesta de MUTEX_LOCK");
        return CPU_MOTIVO_ERROR;
    }

    if (respuesta == OP_OK) {
        return CPU_MOTIVO_CONTINUAR;
    }

    if (respuesta == OP_BLOQUEADO) {
        return CPU_MOTIVO_SYSCALL_BLOQUEANTE;
    }

    log_error(logger, "MUTEX_LOCK respondio error");
    return CPU_MOTIVO_ERROR;
}

/*
    MUTEX_UNLOCK no bloquea.
    Puede despertar a otro proceso, pero el proceso actual sigue ejecutando.
*/
static t_motivo_desalojo ejecutar_syscall_mutex_unlock(uint32_t pid, char* nombre_mutex) {
    if (nombre_mutex == NULL) {
        log_error(logger, "MUTEX_UNLOCK sin nombre");
        return CPU_MOTIVO_ERROR;
    }
    registros.PC++;
    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    enviar_syscall_a_scheduler(pid, OP_MUTEX_UNLOCK);
    enviar_string(socket_ks, nombre_mutex);

    int respuesta = 0;

    if (!recibir_respuesta_scheduler(&respuesta)) {
        log_error(logger, "No se recibio respuesta de MUTEX_UNLOCK");
        return CPU_MOTIVO_ERROR;
    }

    if (respuesta != OP_OK) {
        log_error(logger, "MUTEX_UNLOCK respondio error");
        return CPU_MOTIVO_ERROR;
    }

    return CPU_MOTIVO_CONTINUAR;
}

static t_motivo_desalojo ejecutar_syscall_mem_alloc(uint32_t pid, char* id_segmento_str, char* tamanio_str) {
    if (id_segmento_str == NULL || tamanio_str == NULL) {
        log_error(logger, "MEM_ALLOC con parametros invalidos");
        return CPU_MOTIVO_ERROR;
    }

    int id_segmento = atoi(id_segmento_str);
    int tamanio = atoi(tamanio_str);

    registros.PC++;

    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    enviar_syscall_a_scheduler(pid, OP_MEM_ALLOC);
    enviar_entero(socket_ks, id_segmento);
    enviar_entero(socket_ks, tamanio);

    int respuesta = 0;

    /*
        En el caso normal, Scheduler responde OP_OK.

        Si no responde, en el estado actual del Scheduler puede ser porque
        el pedido disparó compactación y el proceso fue bloqueado.
    */
    if (!recibir_respuesta_scheduler(&respuesta)) {
        log_error(logger, "No se recibio respuesta de MEM_ALLOC");
        return CPU_MOTIVO_ERROR;
    }

    if (respuesta == OP_BLOQUEADO) {
        return CPU_MOTIVO_SYSCALL_BLOQUEANTE;
    }

    if (respuesta != OP_OK) {
        log_error(logger, "MEM_ALLOC respondio error");
        return CPU_MOTIVO_ERROR;
    }

    /*
        Kernel Memory ya creó el segmento.
        La CPU necesita actualizar su tabla de segmentos local.
    */
    if (!obtener_contexto(pid)) {
        log_error(logger, "MEM_ALLOC: no se pudo refrescar el contexto del PID %d", pid);
        return CPU_MOTIVO_ERROR;
    }

    return CPU_MOTIVO_CONTINUAR;
}

static t_motivo_desalojo ejecutar_syscall_mem_free(uint32_t pid, char* id_segmento_str) {
    if (id_segmento_str == NULL) {
        log_error(logger, "MEM_FREE sin id de segmento");
        return CPU_MOTIVO_ERROR;
    }

    int id_segmento = atoi(id_segmento_str);

    registros.PC++;
    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    enviar_syscall_a_scheduler(pid, OP_MEM_FREE);
    enviar_entero(socket_ks, id_segmento);

    int respuesta = 0;

    if (!recibir_respuesta_scheduler(&respuesta)) {
        log_error(logger, "No se recibio respuesta de MEM_FREE");
        return CPU_MOTIVO_ERROR;
    }

    if (respuesta != OP_OK) {
        log_error(logger, "MEM_FREE respondio error");
        return CPU_MOTIVO_ERROR;
    }

    if (!obtener_contexto(pid)) {
        log_error(logger, "MEM_FREE: no se pudo refrescar el contexto del PID %d", pid);
        return CPU_MOTIVO_ERROR;
    }

    return CPU_MOTIVO_CONTINUAR;
}

static t_motivo_desalojo ejecutar_syscall_stdin(uint32_t pid, char* registro_direccion, char* registro_tamanio) {
    if (registro_direccion == NULL || registro_tamanio == NULL) {
        log_error(logger, "STDIN con parametros invalidos");
        return CPU_MOTIVO_ERROR;
    }

    if (tamanio_registro(registro_direccion) == 0 || tamanio_registro(registro_tamanio) == 0) {
        log_error(
            logger,
            "STDIN con registros invalidos: direccion=%s tamanio=%s",
            registro_direccion,
            registro_tamanio
        );
        return CPU_MOTIVO_ERROR;
    }

    uint32_t direccion_logica = obtener_valor_registro(registro_direccion);
    uint32_t tamanio = obtener_valor_registro(registro_tamanio);

    if (tamanio == 0) {
        registros.PC++;
        return CPU_MOTIVO_CONTINUAR;
    }

    uint32_t direccion_fisica = 0;

    if (!traducir_direccion(pid, direccion_logica, tamanio, &direccion_fisica)) {
        return CPU_MOTIVO_SEG_FAULT;
    }

    registros.PC++;
    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    enviar_syscall_a_scheduler(pid, OP_IO_STDIN);
    enviar_entero(socket_ks, (int) tamanio);
    enviar_entero(socket_ks, (int) direccion_fisica);

    log_info(
        logger,
        "PID: %d - STDIN - Dir. Física: %d - Tamaño: %d",
        pid,
        direccion_fisica,
        tamanio
    );

    return CPU_MOTIVO_SYSCALL_BLOQUEANTE;
}

static t_motivo_desalojo ejecutar_syscall_stdout(uint32_t pid, char* registro_direccion, char* registro_tamanio) {
    if (registro_direccion == NULL || registro_tamanio == NULL) {
        log_error(logger, "STDOUT con parametros invalidos");
        return CPU_MOTIVO_ERROR;
    }

    if (tamanio_registro(registro_direccion) == 0 || tamanio_registro(registro_tamanio) == 0) {
        log_error(
            logger,
            "STDOUT con registros invalidos: direccion=%s tamanio=%s",
            registro_direccion,
            registro_tamanio
        );
        return CPU_MOTIVO_ERROR;
    }

    uint32_t direccion_logica = obtener_valor_registro(registro_direccion);
    uint32_t tamanio = obtener_valor_registro(registro_tamanio);

    if (tamanio == 0) {
        registros.PC++;
        return CPU_MOTIVO_CONTINUAR;
    }

    uint32_t direccion_fisica = 0;

    if (!traducir_direccion(pid, direccion_logica, tamanio, &direccion_fisica)) {
        return CPU_MOTIVO_SEG_FAULT;
    }

    registros.PC++;
    if (!guardar_contexto(pid)) {
        return CPU_MOTIVO_ERROR;
    }

    enviar_syscall_a_scheduler(pid, OP_IO_STDOUT);
    enviar_entero(socket_ks, (int) direccion_fisica);
    enviar_entero(socket_ks, (int) tamanio);

    log_info(
        logger,
        "PID: %d - STDOUT - Dir. Física: %d - Tamaño: %d",
        pid,
        direccion_fisica,
        tamanio
    );

    return CPU_MOTIVO_SYSCALL_BLOQUEANTE;
}

t_motivo_desalojo ejecutar_ciclo_instruccion(uint32_t pid) {
    // FETCH
    char* instruccion = fetch(pid);

    if (instruccion == NULL) {
        log_error(logger, "PID: %d - Error en FETCH", pid);
        return CPU_MOTIVO_ERROR;
    }

    // DECODE
    char* operacion = NULL;
    char* param1 = NULL;
    char* param2 = NULL;

    decode(instruccion, &operacion, &param1, &param2);

    // EXECUTE
    t_motivo_desalojo motivo = execute(pid, operacion, param1, param2);

    free(instruccion);

    /*
        Si la instrucción hizo que el proceso deje CPU, no chequeamos interrupción.

        Ejemplos:
        - EXIT
        - SLEEP
        - MUTEX_LOCK bloqueante
        - Error
    */
    if (motivo != CPU_MOTIVO_CONTINUAR) {
        return motivo;
    }

    // CHECK INTERRUPT
    if (check_interrupt(pid)) {
        log_info(logger, "## Interrupción recibida");
        log_debug(logger, "## PID: %d - Interrupción recibida", pid);
        return CPU_MOTIVO_FIN_QUANTUM;
    }

    return CPU_MOTIVO_CONTINUAR;
}

char* fetch(uint32_t pid) {
    enviar_entero(socket_km, OP_FETCH);
    enviar_entero(socket_km, (int) pid);
    enviar_entero(socket_km, (int) registros.PC);

    char* instruccion = recibir_instruccion_desde_memoria();

    log_info(logger, "## PID: %d - FETCH - Program Counter: %d", pid, registros.PC);

    return instruccion;
}

void decode(char* instruccion, char** operacion, char** param1, char** param2) {
    /*
        strtok modifica el string original.
        Está bien porque instruccion fue reservada dinámicamente por fetch()
        y se libera después de execute().
    */
    *operacion = strtok(instruccion, " \t\n");
    *param1 = strtok(NULL, " \t\n");
    *param2 = strtok(NULL, " \t\n");
}

t_motivo_desalojo execute(uint32_t pid, char* op, char* p1, char* p2) {
    if (op == NULL) {
        return CPU_MOTIVO_ERROR;
    }

    log_info(
        logger,
        "## PID: %d - Ejecutando: %s - %s %s",
        pid,
        op,
        p1 != NULL ? p1 : "",
        p2 != NULL ? p2 : ""
    );

    /*
        INSTRUCCIONES COMUNES
    */

    if (strcmp(op, "NOOP") == 0) {
        ejecutar_NOOP();
        registros.PC++;
        return CPU_MOTIVO_CONTINUAR;
    }

    if (strcmp(op, "SET") == 0) {
        if (p1 == NULL || p2 == NULL) {
            log_error(logger, "SET con parametros invalidos");
            return CPU_MOTIVO_ERROR;
        }

        ejecutar_SET(p1, atoi(p2));

        /*
            Caso especial:
            Si hago SET PC 0, no puedo hacer PC++ después,
            porque estaría pisando el salto.

            Ejemplo:
                SET PC 0

            Debe dejar el PC en 0, no en 1.
        */
        if (strcmp(p1, "PC") != 0) {
            registros.PC++;
        }

        return CPU_MOTIVO_CONTINUAR;
    }

    if (strcmp(op, "SUM") == 0) {
        if (p1 == NULL || p2 == NULL) {
            log_error(logger, "SUM con parametros invalidos");
            return CPU_MOTIVO_ERROR;
        }

        ejecutar_SUM(p1, p2);
        registros.PC++;
        return CPU_MOTIVO_CONTINUAR;
    }

    if (strcmp(op, "SUB") == 0) {
        if (p1 == NULL || p2 == NULL) {
            log_error(logger, "SUB con parametros invalidos");
            return CPU_MOTIVO_ERROR;
        }

        ejecutar_SUB(p1, p2);
        registros.PC++;
        return CPU_MOTIVO_CONTINUAR;
    }

    if (strcmp(op, "JNZ") == 0) {
        if (p1 == NULL || p2 == NULL) {
            log_error(logger, "JNZ con parametros invalidos");
            return CPU_MOTIVO_ERROR;
        }

        bool salto = ejecutar_JNZ(p1, atoi(p2));

        if (!salto) {
            registros.PC++;
        }

        return CPU_MOTIVO_CONTINUAR;
    }

    /*
        SYSCALLS QUE ADAPTAMOS AL SCHEDULER ACTUAL
    */

    if (strcmp(op, "SLEEP") == 0) {
        return ejecutar_syscall_sleep(pid, p1);
    }

    if (strcmp(op, "INIT_PROC") == 0) {
        return ejecutar_syscall_init_proc(pid, p1, p2);
    }

    if (strcmp(op, "MUTEX_CREATE") == 0) {
        return ejecutar_syscall_mutex_create(pid, p1);
    }

    if (strcmp(op, "MUTEX_LOCK") == 0) {
        return ejecutar_syscall_mutex_lock(pid, p1);
    }

    if (strcmp(op, "MUTEX_UNLOCK") == 0) {
        return ejecutar_syscall_mutex_unlock(pid, p1);
    }

    if (strcmp(op, "MEM_ALLOC") == 0) {
        return ejecutar_syscall_mem_alloc(pid, p1, p2);
    }

    if (strcmp(op, "MEM_FREE") == 0) {
        return ejecutar_syscall_mem_free(pid, p1);
    }

    if (strcmp(op, "MOV_IN") == 0) {
        return ejecutar_mov_in(pid, p1);
    }

    if (strcmp(op, "MOV_OUT") == 0) {
        return ejecutar_mov_out(pid, p1);
    }

    if (strcmp(op, "COPY_MEM") == 0) {
        return ejecutar_copy_mem(pid, p1);
    }

    if (strcmp(op, "STDIN") == 0) {
        return ejecutar_syscall_stdin(pid, p1, p2);
    }

    if (strcmp(op, "STDOUT") == 0) {
        return ejecutar_syscall_stdout(pid, p1, p2);
    }

    if (strcmp(op, "EXIT") == 0) {
        ejecutar_EXIT();
        return CPU_MOTIVO_EXIT;
    }

    log_error(logger, "Instruccion desconocida: %s", op);
    return CPU_MOTIVO_ERROR;
}

bool check_interrupt(uint32_t pid) {
    /*
        Scheduler manda interrupción así:

            OP_INTERRUMPIR_PROCESO
            PID

        No hacemos recv bloqueante porque si no hay interrupción,
        CPU quedaría frenada después de cada instrucción.
    */
    if (interrupcion_pendiente_pid != -1) {
        int pid_interrumpido = interrupcion_pendiente_pid;
        interrupcion_pendiente_pid = -1;

        if ((uint32_t) pid_interrumpido == pid) {
            return true;
        }

        log_warning(
            logger,
            "Interrupcion pendiente para PID %d, pero CPU ejecuta PID %d",
            pid_interrumpido,
            pid
        );

        return false;
    }
    if (!socket_tiene_datos(socket_ks, 0)) {
        return false;
    }

    int codigo = 0;

    if (!recibir_entero(socket_ks, &codigo)) {
        log_error(logger, "Error al recibir codigo de interrupcion");
        return false;
    }

    if (codigo != OP_INTERRUMPIR_PROCESO) {
        log_warning(logger, "Codigo inesperado durante check_interrupt: %d", codigo);
        return false;
    }

    int pid_interrumpido = 0;

    if (!recibir_entero(socket_ks, &pid_interrumpido)) {
        log_error(logger, "Error al recibir PID de interrupcion");
        return false;
    }

    if ((uint32_t) pid_interrumpido != pid) {
        log_warning(
            logger,
            "Interrupcion recibida para PID %d, pero CPU ejecuta PID %d",
            pid_interrumpido,
            pid
        );
        return false;
    }

    return true;
}