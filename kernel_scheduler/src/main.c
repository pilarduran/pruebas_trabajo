#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <utils/protocolo.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <commons/log.h>
#include <commons/config.h>

#include <pthread.h>
#include <semaphore.h>
#include <commons/collections/list.h>
#include <time.h>

/*
    ============================================================
    VARIABLES GLOBALES
    ============================================================
    
*/

t_log* logger = NULL;
t_config* config = NULL;


char* algoritmo_planificacion = NULL;
int   rr_quantum  = 0;
int   proximo_pid = 0;
int   socket_memoria_global = -1;
int   socket_notificaciones_memoria = -1;



char** algoritmos_por_cola  = NULL;
int    cantidad_colas       = 0;
bool   desalojo_entre_colas = false;
int    timeout_suspension   = 0;

/*  ============================================================           
    ESTRUCTURAS                                  
    ============================================================
*/
typedef enum {
    NEW,
    READY,
    EXEC,
    BLOCK,
    SUSP_READY,
    SUSP_BLOCK,
    EXIT_STATE
} t_estado;

typedef struct {
    int socket;
    int id;
} t_cpu;

typedef struct {
    int  socket;
    int  tipo;
} t_io;

typedef struct {
    int  socket_cpu;
    int  pid;
} t_timer;

typedef struct {
    int      pid;
    int      prioridad;
    int      prioridad_original;
    t_estado estado;
    int      socket_cpu;
    t_cpu*   cpu;
    t_timer* timer;
    int      io_tiempo;
    int      io_direccion_fisica;
    int      io_tamanio;
    time_t   tiempo_suspension;
} t_proceso;

typedef struct {
    char*   nombre;
    int     pid_dueno;
    t_list* cola_espera;
} t_mutex;

//VARIABLES GLOBALES 


bool       compactando               = false;
t_proceso* proceso_esperando_memoria = NULL;
t_cpu*     cpu_esperando_memoria     = NULL;

bool bsod_activo            = false;
int  cpus_pendientes_bsod   = 0;

/*
    ============================================================
    LISTAS GLOBALES
    ============================================================
*/

t_list* lista_new;
t_list* lista_ready;
t_list** colas_ready;
t_list* lista_ready_invalida;
t_list* lista_exec;
t_list* lista_block;
t_list* lista_susp_ready;
t_list* lista_susp_block;
t_list* lista_cpus_libres;
t_list* lista_ios;
t_list* lista_mutex;

t_list* cola_espera_sleep;
t_list* cola_espera_stdin;
t_list* cola_espera_stdout;

/*
    ============================================================
    SEMAFOROS Y MUTEX
    ============================================================
*/

sem_t   sem_cola_sleep;
sem_t   sem_cola_stdin;
sem_t   sem_cola_stdout;

sem_t           sem_puede_planificar;
sem_t           sem_hay_procesos_ready;
sem_t           sem_hay_cpus_libres;
pthread_mutex_t mutex_listas = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_pedidos_memoria = PTHREAD_MUTEX_INITIALIZER;

/*
    ============================================================
    HEADERS DE FUNCIONES
    ============================================================
*/

bool        iniciar_logger(void);
bool        cargar_configuracion(const char* path_config);
void        inicializar_estructuras(void);

int         crear_conexion(const char* ip, const char* puerto);
int         iniciar_servidor(const char* puerto);
int         esperar_cliente(int socket_servidor);
bool        enviar_entero(int socket, int valor);
bool        recibir_entero(int socket, int* valor_recibido);
char*       recibir_string(int socket);
bool        enviar_string(int socket, const char* string);

bool        hacer_handshake_con_memoria(int socket_memoria, int codigo_handshake);
void        atender_conexiones_entrantes(int socket_servidor);
void*       procesar_cliente(void* arg);
void        atender_cpu(int socket_cpu);
void        desconectar_cpu(t_cpu* cpu);
void        atender_io(int socket_io);

void        crear_proceso_inicial(const char* path_proceso, int socket_memoria);
bool        encolar_en_ready(t_proceso* proceso);
bool        encolar_al_frente_de_ready(t_proceso* proceso);
void        mover_proceso_a_ready(t_proceso* proceso);
t_proceso*  obtener_proximo_proceso_ready(bool* necesita_timer);
void        mover_proceso_de_block_a_ready(t_proceso* proceso);
void*       planificador_corto_plazo(void* arg);
void*       hilo_quantum(void* arg);
void        cancelar_timer(t_proceso* proceso);
void*       hilo_suspension(void* arg);
void        arrancar_timer_suspension(t_proceso* proceso);
bool        comparar_prioridad_suspension(void* elem_a, void* elem_b);
void        intentar_dessuspender_procesos(void);
void*       hilo_escuchar_memoria(void* arg);
void        desalojar_todas_las_cpus(void);

void        manejar_proceso_exit(int socket_cpu);
void        manejar_seg_fault(int socket_cpu);
void        liberar_mutex_del_proceso(int pid);
void        manejar_fin_quantum(int socket_cpu);
void        manejar_bsod(void);
static void destruir_proceso(void* elemento);
void        verificar_fin_bsod(void);
void        manejar_syscall(int socket_cpu);
void        manejar_syscall_memoria(t_proceso* proceso, int tipo_syscall, int socket_cpu);

void        manejar_fin_io(t_proceso* proceso);
void        manejar_io_sleep(t_proceso* proceso, int socket_cpu);
void        manejar_io_stdin(t_proceso* proceso, int socket_cpu);
void        manejar_io_stdout(t_proceso* proceso, int socket_cpu);
void        ejecutar_io_sleep(t_io* io, t_proceso* proceso);
void        ejecutar_io_stdin(t_io* io, t_proceso* proceso);
void        ejecutar_io_stdout(t_io* io, t_proceso* proceso);
static t_list*  cola_de_io(int tipo);
static sem_t*   semaforo_de_io(int tipo);
static void     encolar_pedido_io(int tipo, t_proceso* proceso);

void        manejar_init_proc(int socket_cpu);
void        manejar_mutex_create(t_proceso* proceso, int socket_cpu);
void        aplicar_herencia(t_proceso* proceso, t_mutex* mutex);
void        devolver_prioridad(t_proceso* proceso);
void        manejar_mutex_lock(t_proceso* proceso, int socket_cpu);
int         mayor_prioridad_del_mutex(t_mutex* mutex);
void        manejar_mutex_unlock(t_proceso* proceso, int socket_cpu);
void        bloquear_proceso(t_proceso* proceso);

t_proceso*  buscar_proceso_en_exec(int pid);
t_proceso*  buscar_proceso_por_pid(int pid);
t_mutex*    buscar_mutex(char* nombre);

const char* nombre_estado(t_estado estado);
const char* nombre_syscall(int tipo);
void        liberar_recursos(int socket_pedidos, int socket_notificaciones, int socket_servidor);

/*
    ============================================================
    MAIN
    ============================================================
*/

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3) {
        printf("Uso correcto: %s [ARCHIVO_CONFIG] [PATH_PROCESO_INICIAL]\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char* path_config          = argv[1];
    const char* path_proceso_inicial = argv[2];

    if (!cargar_configuracion(path_config)) {
        log_error(logger, "No se pudo cargar el archivo de configuracion");
        return EXIT_FAILURE;
    }


    if (!iniciar_logger()) {
        printf("No se pudo iniciar el logger\n");
        return EXIT_FAILURE;
    }
    

    algoritmo_planificacion = config_get_string_value(config, "PLANIFICATION_ALGORITHM");
    rr_quantum              = config_get_int_value(config, "RR_QUANTUM");

    desalojo_entre_colas = strcmp(config_get_string_value(config, "QUEUE_PREEMPTION"), "TRUE") == 0;
    timeout_suspension   = config_get_int_value(config, "SUSPENSION_TIMEOUT");

    algoritmos_por_cola = config_get_array_value(config, "QUEUES_ALGORITHMS");
    cantidad_colas = 0;
    while (algoritmos_por_cola[cantidad_colas] != NULL) {
        cantidad_colas++;
    }
    inicializar_estructuras();

    char* ip_memory      = config_get_string_value(config, "IP_MEMORY");
    char* puerto_memory  = config_get_string_value(config, "PUERTO_MEMORY");
    char* puerto_escucha = config_get_string_value(config, "PUERTO_ESCUCHA");

    socket_memoria_global = crear_conexion(ip_memory, puerto_memory);
    if (socket_memoria_global == -1) {
        log_error(logger, "No se pudo conectar a Kernel Memory (pedidos)");
        return EXIT_FAILURE;
    }

    if (!hacer_handshake_con_memoria(socket_memoria_global, HANDSHAKE_KERNEL_SCHEDULER)) {
        log_error(logger, "Fallo el handshake de pedidos con Kernel Memory");
        return EXIT_FAILURE;
    }

    log_info(logger, "## Conectado a Kernel Memory (pedidos)");

     socket_notificaciones_memoria = crear_conexion(ip_memory, puerto_memory);
    if (socket_notificaciones_memoria == -1) {
        log_error(logger, "No se pudo conectar a Kernel Memory (notificaciones)");
        return EXIT_FAILURE;
    }

    if (!hacer_handshake_con_memoria(socket_notificaciones_memoria, HANDSHAKE_KS_NOTIFICACIONES)) {
        log_error(logger, "Fallo el handshake de notificaciones con Kernel Memory");
        return EXIT_FAILURE;
    }

    log_info(logger, "## Conectado a Kernel Memory (notificaciones)");

    log_info(logger, "## Conectado a Kernel Memory");


    crear_proceso_inicial(path_proceso_inicial, socket_memoria_global);

    pthread_t hilo_planificador;
    pthread_create(&hilo_planificador, NULL, planificador_corto_plazo, NULL);
    pthread_detach(hilo_planificador);

    pthread_t hilo_memoria;
    pthread_create(&hilo_memoria, NULL, hilo_escuchar_memoria, NULL);
    pthread_detach(hilo_memoria);

    int socket_servidor = iniciar_servidor(puerto_escucha);
    if (socket_servidor == -1) {
        log_error(logger, "No se pudo iniciar el servidor");
        return EXIT_FAILURE;
    }

    atender_conexiones_entrantes(socket_servidor);

    liberar_recursos(socket_memoria_global, socket_notificaciones_memoria, socket_servidor);
    return EXIT_SUCCESS;
}

/*
    ============================================================
    FUNCIONES DE INICIALIZACION
    ============================================================
*/

bool iniciar_logger(void) {
    /*
        log_create(path_archivo, nombre_programa, mostrar_en_consola, nivel)
    */
    char* log_level_str = config_get_string_value(config, "LOG_LEVEL");
    logger = log_create("kernel_scheduler.log", "KERNEL_SCHEDULER", true, log_level_from_string(log_level_str));
    return logger != NULL;
}

bool cargar_configuracion(const char* path_config) {
    config = config_create((char*) path_config);
    if (config == NULL) return false;

    if (!config_has_property(config, "LOG_LEVEL"))               return false;
    if (!config_has_property(config, "IP_MEMORY"))               return false;
    if (!config_has_property(config, "PUERTO_MEMORY"))           return false;
    if (!config_has_property(config, "PUERTO_ESCUCHA"))          return false;
    if (!config_has_property(config, "PLANIFICATION_ALGORITHM")) return false;
    if (!config_has_property(config, "RR_QUANTUM"))              return false;
    if (!config_has_property(config, "QUEUES_ALGORITHMS"))       return false;
    if (!config_has_property(config, "QUEUE_PREEMPTION"))        return false;
    if (!config_has_property(config, "SUSPENSION_TIMEOUT"))      return false;

    return true;
}

void inicializar_estructuras(void) {
    lista_new         = list_create();
    lista_ready       = list_create();
    lista_exec        = list_create();
    lista_block       = list_create();
    lista_susp_ready  = list_create();
    lista_susp_block  = list_create();
    lista_cpus_libres = list_create();
    lista_ios         = list_create();
    lista_mutex       = list_create();

    colas_ready = NULL;
    if (strcmp(algoritmo_planificacion, "CMN") == 0) {
        colas_ready = malloc(cantidad_colas * sizeof(t_list*));
        for (int i = 0; i < cantidad_colas; i++) {
            colas_ready[i] = list_create();
        }
    }
    lista_ready_invalida = list_create();

    cola_espera_sleep  = list_create();
    cola_espera_stdin  = list_create();
    cola_espera_stdout = list_create();


    sem_init(&sem_puede_planificar, 0, 1);
    sem_init(&sem_hay_procesos_ready, 0, 0);
    sem_init(&sem_hay_cpus_libres,    0, 0);
    sem_init(&sem_cola_sleep,  0, 0);
    sem_init(&sem_cola_stdin,  0, 0);
    sem_init(&sem_cola_stdout, 0, 0);

    pthread_mutex_init(&mutex_listas, NULL);
}

/*
    ============================================================
    SOCKET CLIENTE: CONECTARSE A MEMORY
    ============================================================
*/

int crear_conexion(const char* ip, const char* puerto) {
    struct addrinfo hints;
    struct addrinfo* server_info;
    int socket_cliente;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;       // IPv4 o IPv6
    hints.ai_socktype = SOCK_STREAM;   // TCP

    int resultado = getaddrinfo(ip, puerto, &hints, &server_info);
    if (resultado != 0) {
        if (logger != NULL) {
            log_error(logger, "getaddrinfo fallo al conectar con %s:%s", ip, puerto);
        }
        return -1;
    }

    socket_cliente = socket(
        server_info->ai_family,
        server_info->ai_socktype,
        server_info->ai_protocol
    );

    if (socket_cliente == -1) {
        if (logger != NULL) {
            log_error(logger, "No se pudo crear el socket cliente");
        }
        freeaddrinfo(server_info);
        return -1;
    }

    if (connect(socket_cliente, server_info->ai_addr, server_info->ai_addrlen) == -1) {
        if (logger != NULL) {
            log_error(logger, "No se pudo hacer connect a %s:%s", ip, puerto);
        }
        close(socket_cliente);
        freeaddrinfo(server_info);
        return -1;
    }

    freeaddrinfo(server_info);
    return socket_cliente;
}

/*
    ============================================================
    SOCKET SERVIDOR: ESCUCHAR CPU/IO
    ============================================================
*/

int iniciar_servidor(const char* puerto) {
    int socket_servidor;
    struct addrinfo hints;
    struct addrinfo* server_info;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; 

    int resultado = getaddrinfo(NULL, puerto, &hints, &server_info);
    if (resultado != 0) {
        if (logger != NULL) {
            log_error(logger, "getaddrinfo fallo al iniciar servidor en puerto %s", puerto);
        }
        return -1;
    }

    socket_servidor = socket(
        server_info->ai_family,
        server_info->ai_socktype,
        server_info->ai_protocol
    );

    if (socket_servidor == -1) {
        if (logger != NULL) {
            log_error(logger, "No se pudo crear el socket servidor");
        }
        freeaddrinfo(server_info);
        return -1;
    }


    setsockopt(socket_servidor, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

    if (bind(socket_servidor, server_info->ai_addr, server_info->ai_addrlen) == -1) {
        if (logger != NULL) {
            log_error(logger, "No se pudo bindear el servidor al puerto %s", puerto);
        }
        close(socket_servidor);
        freeaddrinfo(server_info);
        return -1;
    }

    if (listen(socket_servidor, SOMAXCONN) == -1) {
        if (logger != NULL) {
            log_error(logger, "No se pudo escuchar conexiones en el puerto %s", puerto);
        }
        close(socket_servidor);
        freeaddrinfo(server_info);
        return -1;
    }

    freeaddrinfo(server_info);
    return socket_servidor;
}

int esperar_cliente(int socket_servidor) {
    struct sockaddr_storage direccion_cliente;
    socklen_t tam_direccion = sizeof(direccion_cliente);

    int socket_cliente = accept(
        socket_servidor,
        (struct sockaddr*) &direccion_cliente,
        &tam_direccion
    );

    return socket_cliente;
}

/*
    ============================================================
    ENVIO / RECEPCION SIMPLE DE ENTEROS
    ============================================================
*/

bool enviar_entero(int socket, int valor) {
    int bytes_enviados = send(socket, &valor, sizeof(int), 0);
    return bytes_enviados == sizeof(int);
}

bool recibir_entero(int socket, int* valor_recibido) {
    int bytes_recibidos = recv(socket, valor_recibido, sizeof(int), MSG_WAITALL);
    return bytes_recibidos == sizeof(int);
}

char* recibir_string(int socket) {
    int tamanio = 0;
    recibir_entero(socket, &tamanio);
    char* string = malloc(tamanio + 1);
    recv(socket, string, tamanio, MSG_WAITALL);
    string[tamanio] = '\0';
    return string;
}

bool enviar_string(int socket, const char* string) {
    int tamanio = strlen(string);
    if (!enviar_entero(socket, tamanio)) return false;
    return send(socket, string, tamanio, 0) == tamanio;
}

/*
    ============================================================
    HANDSHAKE CON KERNEL MEMORY
    ============================================================
*/

bool hacer_handshake_con_memoria(int socket_memoria, int codigo_handshake) {
    if (!enviar_entero(socket_memoria, codigo_handshake)) {
        log_error(logger, "No se pudo enviar handshake a Kernel Memory");
        return false;
    }

    int respuesta = 0;
    if (!recibir_entero(socket_memoria, &respuesta)) {
        log_error(logger, "No se pudo recibir respuesta de Kernel Memory");
        return false;
    }

    if (respuesta != HANDSHAKE_OK) {
        log_error(logger, "Kernel Memory respondio algo inesperado: %d", respuesta);
        return false;
    }

    return true;
}

/*
    ============================================================
    LOOP DE CONEXIONES ENTRANTES
    ============================================================
*/

void atender_conexiones_entrantes(int socket_servidor) {
    while (1) {
        int socket_cliente = esperar_cliente(socket_servidor);

        if (socket_cliente == -1) {
            log_warning(logger, "Fallo un accept, sigo esperando conexiones");
            continue;
        }

        int* socket_ptr = malloc(sizeof(int));
        *socket_ptr = socket_cliente;

        pthread_t hilo;
        pthread_create(&hilo, NULL, procesar_cliente, socket_ptr);
        pthread_detach(hilo);
    }
}
/*
    ============================================================
    PROCESAR UN CLIENTE
    ============================================================
*/

void* procesar_cliente(void* arg) {

    int socket_cliente = *(int*)arg;

    free(arg);

    int codigo = 0;

    if (!recibir_entero(socket_cliente, &codigo)) {

        log_warning(logger, "No se pudo recibir el codigo inicial del cliente");

        close(socket_cliente);

        return NULL;

    }

    switch (codigo) {

        case HANDSHAKE_CPU:

            atender_cpu(socket_cliente);

            break;

        case HANDSHAKE_IO:

            atender_io(socket_cliente);

            break;

        default:

            log_warning(logger, "Codigo desconocido: %d", codigo);

            enviar_entero(socket_cliente, HANDSHAKE_ERROR);

            close(socket_cliente);

            break;

    }

    return NULL;

}

/*
    ============================================================
    FUNCIONES DE PLANIFICADOR
    ============================================================
*/
void crear_proceso_inicial(const char* path_proceso, int socket_memoria) {
    t_proceso* proceso = malloc(sizeof(t_proceso));
    proceso->pid        = proximo_pid++;
    proceso->prioridad  = 0;
    proceso->prioridad_original = proceso->prioridad;
    proceso->estado     = NEW;
    proceso->socket_cpu = -1;
    proceso->cpu        = NULL;
    proceso->timer      = NULL;

    log_info(logger, "## (%d) Se crea el proceso - Estado: NEW", proceso->pid);

    enviar_entero(socket_memoria, OP_CREAR_PROCESO);
    enviar_entero(socket_memoria, proceso->pid);
    enviar_string(socket_memoria, path_proceso);

    int respuesta = 0;
    recibir_entero(socket_memoria, &respuesta);

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_new, proceso);
    pthread_mutex_unlock(&mutex_listas);

    mover_proceso_a_ready(proceso);
}

bool encolar_en_ready(t_proceso* proceso) {
    if (strcmp(algoritmo_planificacion, "CMN") == 0) {
        if (proceso->prioridad < 0 || proceso->prioridad >= cantidad_colas) {
            log_warning(logger, "## (%d) Prioridad %d fuera de rango, no se planifica",
                        proceso->pid, proceso->prioridad);
            list_add(lista_ready_invalida, proceso);
            return false;
        }
        list_add(colas_ready[proceso->prioridad], proceso);
        if (desalojo_entre_colas) {
            t_proceso* peor = NULL;
            for (int i = 0; i < list_size(lista_exec); i++) {
                t_proceso* actual = list_get(lista_exec, i);
                if (peor == NULL || actual->prioridad > peor->prioridad) {
                    peor = actual;
                }
            }

            if (peor != NULL && peor->prioridad > proceso->prioridad) {
                log_info(logger, "## (%d) Prioridad: %d - Desalojado por cola mas prioritaria por el proceso %d con prioridad %d",
                          peor->pid, peor->prioridad, proceso->pid, proceso->prioridad);

                int socket_peor = peor->socket_cpu;
                int pid_peor    = peor->pid;

                pthread_mutex_unlock(&mutex_listas);
                enviar_entero(socket_peor, OP_INTERRUMPIR_PROCESO);
                enviar_entero(socket_peor, pid_peor);
                pthread_mutex_lock(&mutex_listas);
            }
        }
        return true;
    } else {
        list_add(lista_ready, proceso);
        return true;
    }
}

bool encolar_al_frente_de_ready(t_proceso* proceso) {
    if (strcmp(algoritmo_planificacion, "CMN") == 0) {
        if (proceso->prioridad < 0 || proceso->prioridad >= cantidad_colas) {
            list_add(lista_ready_invalida, proceso);
            return false;
        }
        list_add_in_index(colas_ready[proceso->prioridad], 0, proceso);
    } else {
        list_add_in_index(lista_ready, 0, proceso);
    }
    return true;
}

void mover_proceso_a_ready(t_proceso* proceso) {
    pthread_mutex_lock(&mutex_listas);
    t_estado estado_anterior = proceso->estado;
    list_remove_element(lista_new, proceso);
    proceso->estado = READY;
    bool planificable = encolar_en_ready(proceso);
    pthread_mutex_unlock(&mutex_listas);

    log_info(logger, "## (%d) Pasa del estado %s al estado READY",
             proceso->pid, nombre_estado(estado_anterior));

    if (planificable) {
        sem_post(&sem_hay_procesos_ready);
    }
}

void mover_proceso_de_block_a_ready(t_proceso* proceso) {
    pthread_mutex_lock(&mutex_listas);
    list_remove_element(lista_block, proceso);
    proceso->estado = READY;
    bool planificable = encolar_en_ready(proceso);
    pthread_mutex_unlock(&mutex_listas);

    log_info(logger, "## (%d) Pasa del estado BLOCK al estado READY", proceso->pid);

    if (planificable) {
        sem_post(&sem_hay_procesos_ready);
    }
}

void* planificador_corto_plazo(void* arg) {
    while (1) {
        sem_wait(&sem_hay_procesos_ready);
        sem_wait(&sem_hay_cpus_libres);
        sem_wait(&sem_puede_planificar);

        pthread_mutex_lock(&mutex_listas);
        bool necesita_timer;
        t_proceso* proceso = obtener_proximo_proceso_ready(&necesita_timer);

        if (proceso == NULL) {
            pthread_mutex_unlock(&mutex_listas);
            sem_post(&sem_hay_cpus_libres);
            sem_post(&sem_puede_planificar);
            log_warning(logger, "El planificador desperto sin procesos en READY");
            continue;
        }

        t_cpu*     cpu     = list_remove(lista_cpus_libres, 0);
        proceso->estado     = EXEC;
        proceso->cpu        = cpu;
        proceso->socket_cpu = cpu->socket;
        list_add(lista_exec, proceso);
        
        t_timer* timer    = NULL;
        if (necesita_timer) {
            timer = malloc(sizeof(t_timer));
            timer->socket_cpu = cpu->socket;
            timer->pid        = proceso->pid;
            proceso->timer    = timer;
        }
        pthread_mutex_unlock(&mutex_listas);

        log_info(logger, "## (%d) Pasa del estado READY al estado EXEC", proceso->pid);

        enviar_entero(cpu->socket, OP_EJECUTAR_PROCESO);
        enviar_entero(cpu->socket, proceso->pid);

        if (timer != NULL) {
            pthread_t hilo;
            pthread_create(&hilo, NULL, hilo_quantum, timer);
            pthread_detach(hilo);
        }

        sem_post(&sem_puede_planificar);
    }
    return NULL;
}

t_proceso* obtener_proximo_proceso_ready(bool* necesita_timer) {
    if (strcmp(algoritmo_planificacion, "CMN") == 0) {
        for (int i = 0; i < cantidad_colas; i++) {
            if (list_size(colas_ready[i]) > 0) {
                *necesita_timer = strcmp(algoritmos_por_cola[i], "RR") == 0;
                return list_remove(colas_ready[i], 0);
            }
        }
        return NULL;
    }

    *necesita_timer = strcmp(algoritmo_planificacion, "RR") == 0;
    return list_remove(lista_ready, 0);
}

void* hilo_quantum(void* arg) {
    t_timer* timer = (t_timer*)arg;

    usleep(rr_quantum * 1000);

    pthread_mutex_lock(&mutex_listas);
    t_proceso* proceso = buscar_proceso_por_pid(timer->pid);
    bool sigo_vigente = (proceso != NULL && proceso->timer == timer);
    if (sigo_vigente) {
        proceso->timer = NULL;
    }
    pthread_mutex_unlock(&mutex_listas);

    if (sigo_vigente) {
        enviar_entero(timer->socket_cpu, OP_INTERRUMPIR_PROCESO);
        enviar_entero(timer->socket_cpu, timer->pid);
    }

    free(timer);
    return NULL;
}

void cancelar_timer(t_proceso* proceso) {   
    proceso->timer = NULL;
}

void* hilo_suspension(void* arg) {
    t_timer* timer = (t_timer*) arg;

    usleep(timeout_suspension * 1000);


    pthread_mutex_lock(&mutex_listas);
    t_proceso* proceso = buscar_proceso_por_pid(timer->pid);
    bool debe_suspender = (proceso != NULL && proceso->timer == timer && proceso->estado == BLOCK);
    if (proceso != NULL && proceso->timer == timer) {
        proceso->timer = NULL;
    }
    if (debe_suspender) {
        list_remove_element(lista_block, proceso);
        proceso->estado = SUSP_BLOCK;
        list_add(lista_susp_block, proceso);
    }
    pthread_mutex_unlock(&mutex_listas);
    
    if (debe_suspender) {
        log_info(logger, "## (%d) Pasa del estado BLOCK al estado SUSP_BLOCK", proceso->pid);

        pthread_mutex_lock(&mutex_pedidos_memoria);
        enviar_entero(socket_memoria_global, OP_SUSPENDER_PROCESO);
        enviar_entero(socket_memoria_global, proceso->pid);

        int respuesta = 0;
        recibir_entero(socket_memoria_global, &respuesta);
        pthread_mutex_unlock(&mutex_pedidos_memoria);

        if(respuesta != OP_OK){
            log_warning(logger, "## (%d) Kernel Memory no puedo suspender el proceso", proceso->pid);
        }
    }

    free(timer);
    return NULL;
}

void arrancar_timer_suspension(t_proceso* proceso) {
    t_timer* timer    = malloc(sizeof(t_timer));
    timer->pid        = proceso->pid;
    timer->socket_cpu = -1;
    proceso->timer    = timer;

    pthread_t hilo;
    pthread_create(&hilo, NULL, hilo_suspension, timer);
    pthread_detach(hilo);
}

/*
    ============================================================
    MANEJO DE CPU
    ============================================================
*/

void atender_cpu(int socket_cpu) {
    int id_cpu = 0;
    recibir_entero(socket_cpu, &id_cpu);

    log_info(logger, "## CPU %d Conectada", id_cpu);
    enviar_entero(socket_cpu, HANDSHAKE_OK);

    t_cpu* cpu  = malloc(sizeof(t_cpu));
    cpu->socket = socket_cpu;
    cpu->id     = id_cpu;

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_cpus_libres, cpu);
    pthread_mutex_unlock(&mutex_listas);

    sem_post(&sem_hay_cpus_libres);

    while (1) {
        int codigo = 0;
        if (!recibir_entero(socket_cpu, &codigo)) {
            log_warning(logger, "CPU %d se desconecto", id_cpu);
            break;
        }

        switch (codigo) {
            case OP_PROCESO_EXIT:{
                pthread_mutex_lock(&mutex_listas);
                bool bsod = bsod_activo;
                pthread_mutex_unlock(&mutex_listas);
                if (bsod) {
                    verificar_fin_bsod();
                } else {
                    manejar_proceso_exit(socket_cpu);
                }
                break;
            }
            case OP_PROCESO_SYSCALL:
                manejar_syscall(socket_cpu);
                break;
            case OP_PROCESO_FIN_QUANTUM:{
                pthread_mutex_lock(&mutex_listas);
                bool bsod = bsod_activo;
                pthread_mutex_unlock(&mutex_listas);
                if (bsod) {
                    verificar_fin_bsod();
                } else {
                    manejar_fin_quantum(socket_cpu);
                }
                break;
            }
            case OP_PROCESO_SEG_FAULT:{
                pthread_mutex_lock(&mutex_listas);
                bool bsod = bsod_activo;
                pthread_mutex_unlock(&mutex_listas);
                if (bsod) {
                    verificar_fin_bsod();
                } else {
                    manejar_seg_fault(socket_cpu);
                }
                break;
            }
        }
    }

    desconectar_cpu(cpu);
    close(socket_cpu);
}

void desconectar_cpu(t_cpu* cpu) {
    pthread_mutex_lock(&mutex_listas);

    bool estaba_libre = list_remove_element(lista_cpus_libres, cpu);
    if (estaba_libre) {
        sem_trywait(&sem_hay_cpus_libres);
    }

    t_proceso* proceso = NULL;
    for (int i = 0; i < list_size(lista_exec); i++) {
        t_proceso* actual = list_get(lista_exec, i);
        if (actual->cpu == cpu) {
            proceso = actual;
            break;
        }
    }

    bool planificable = false;
    if (proceso != NULL) {
        cancelar_timer(proceso);
        list_remove_element(lista_exec, proceso);
        proceso->estado = READY;
        proceso->cpu    = NULL;
        planificable = encolar_en_ready(proceso);
    }

    pthread_mutex_unlock(&mutex_listas);

    if (proceso != NULL) {
        log_warning(logger, "## (%d) La CPU se desconecto durante su ejecucion, vuelve a READY", proceso->pid);
        if (planificable) sem_post(&sem_hay_procesos_ready);
    }

    free(cpu);
}

void manejar_proceso_exit(int socket_cpu) {
    int pid = 0;
    recibir_entero(socket_cpu, &pid);

    pthread_mutex_lock(&mutex_listas);
    t_proceso* proceso = buscar_proceso_en_exec(pid);
    if (proceso == NULL) {
        log_warning(logger, "No se encontro el proceso %d en EXEC", pid);
        pthread_mutex_unlock(&mutex_listas);
        return;
    }

    cancelar_timer(proceso);
    t_cpu* cpu = proceso->cpu;
    list_remove_element(lista_exec, proceso);
    proceso->estado = EXIT_STATE;
    pthread_mutex_unlock(&mutex_listas);

    log_info(logger, "## (%d) Pasa del estado EXEC al estado EXIT", pid);
    log_info(logger, "## (%d) finalizo su ejecucion con motivo de EXIT", pid);

    pthread_mutex_lock(&mutex_pedidos_memoria);
    enviar_entero(socket_memoria_global, OP_FINALIZAR_PROCESO);
    enviar_entero(socket_memoria_global, pid);

    int respuesta = 0;
    recibir_entero(socket_memoria_global, &respuesta);
    pthread_mutex_unlock(&mutex_pedidos_memoria);

    if(respuesta != OP_OK){
        log_warning(logger, "## (%d) Kernel Mmemory no pudo finalizar el proceso", pid);
    }

    liberar_mutex_del_proceso(pid);

    free(proceso);

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_cpus_libres, cpu);
    pthread_mutex_unlock(&mutex_listas);

    sem_post(&sem_hay_cpus_libres);
}

void manejar_seg_fault(int socket_cpu) {
    int pid = 0;
    recibir_entero(socket_cpu, &pid);

    pthread_mutex_lock(&mutex_listas);
    t_proceso* proceso = buscar_proceso_en_exec(pid);
    if (proceso == NULL) {
        log_warning(logger, "No se encontro el proceso %d en EXEC", pid);
        pthread_mutex_unlock(&mutex_listas);
        return;
    }

    cancelar_timer(proceso);
    t_cpu* cpu = proceso->cpu;
    list_remove_element(lista_exec, proceso);
    proceso->estado = EXIT_STATE;
    pthread_mutex_unlock(&mutex_listas);

    log_info(logger, "## (%d) Pasa del estado EXEC al estado EXIT", pid);
    log_info(logger, "## (%d) finalizo su ejecucion con motivo de SEG_FAULT", pid);

    pthread_mutex_lock(&mutex_pedidos_memoria);
    enviar_entero(socket_memoria_global, OP_FINALIZAR_PROCESO);
    enviar_entero(socket_memoria_global, pid);

    int respuesta = 0;
    recibir_entero(socket_memoria_global, &respuesta);
    pthread_mutex_unlock(&mutex_pedidos_memoria);

    if(respuesta != OP_OK){
        log_warning(logger, "## (%d) Kernel Memory no pudo finalizar el proceso", pid);
    }

    liberar_mutex_del_proceso(pid);

    free(proceso);

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_cpus_libres, cpu);
    pthread_mutex_unlock(&mutex_listas);

    sem_post(&sem_hay_cpus_libres);
}

void liberar_mutex_del_proceso(int pid) {
    pthread_mutex_lock(&mutex_listas);

    for (int i = 0; i < list_size(lista_mutex); i++) {
        t_mutex* mutex = list_get(lista_mutex, i);
        if (mutex->pid_dueno != pid) continue;

        if (list_size(mutex->cola_espera) == 0) {
            mutex->pid_dueno = -1;
            log_warning(logger, "## (%d) murio sin liberar el Mutex %s, queda libre", pid, mutex->nombre);
            continue;
        }

        t_proceso* siguiente = list_remove(mutex->cola_espera, 0);
        mutex->pid_dueno = siguiente->pid;

        if (strcmp(algoritmo_planificacion, "CMN") == 0) {
            aplicar_herencia(siguiente, mutex);
        }

        cancelar_timer(siguiente);
        log_warning(logger, "## (%d) murio sin liberar el Mutex %s, se lo cede a (%d)",
                    pid, mutex->nombre, siguiente->pid);

        if (siguiente->estado == BLOCK) {
            list_remove_element(lista_block, siguiente);
            siguiente->estado = READY;
            bool planificable = encolar_en_ready(siguiente);

            log_info(logger, "## (%d) Pasa del estado BLOCK al estado READY", siguiente->pid);
            if (planificable) sem_post(&sem_hay_procesos_ready);

        } else if (siguiente->estado == SUSP_BLOCK) {
            list_remove_element(lista_susp_block, siguiente);
            siguiente->estado = SUSP_READY;
            siguiente->tiempo_suspension = time(NULL);
            list_add(lista_susp_ready, siguiente);

            log_info(logger, "## (%d) Pasa del estado SUSP_BLOCK al estado SUSP_READY", siguiente->pid);

        } else {
            log_warning(logger, "## (%d) hereda un mutex de un proceso que murio, pero su estado era %s (inesperado)",
                        siguiente->pid, nombre_estado(siguiente->estado));
        }
    }

    pthread_mutex_unlock(&mutex_listas);
}

void manejar_fin_quantum(int socket_cpu) {
    int pid = 0;
    recibir_entero(socket_cpu, &pid);

    pthread_mutex_lock(&mutex_listas);
    t_proceso* proceso = buscar_proceso_en_exec(pid);
    if (proceso == NULL) {
        log_warning(logger, "No se encontro el proceso %d en EXEC", pid);
        pthread_mutex_unlock(&mutex_listas);
        return;
    }
    
    t_cpu* cpu = proceso->cpu;
    list_remove_element(lista_exec, proceso);
    proceso->estado  = READY;
    proceso->cpu     = NULL;

    bool planificable;
    if (compactando) {
        planificable = encolar_al_frente_de_ready(proceso);
    } else {
        planificable = encolar_en_ready(proceso);
    }
    pthread_mutex_unlock(&mutex_listas);

    log_info(logger, "## (%d) - Desalojado por fin de quantum", pid);
    log_info(logger, "## (%d) Pasa del estado EXEC al estado READY", pid);

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_cpus_libres, cpu);
    pthread_mutex_unlock(&mutex_listas);

    if (planificable) {
        sem_post(&sem_hay_procesos_ready);
    }
    sem_post(&sem_hay_cpus_libres);
}

void manejar_syscall_memoria(t_proceso* proceso, int tipo_syscall, int socket_cpu) {
    int id_segmento = 0;
    recibir_entero(socket_cpu, &id_segmento);

    if (tipo_syscall == OP_MEM_ALLOC) {
        int tamanio = 0;
        recibir_entero(socket_cpu, &tamanio);

        pthread_mutex_lock(&mutex_pedidos_memoria);
        enviar_entero(socket_memoria_global, OP_CREAR_SEGMENTO);
        enviar_entero(socket_memoria_global, proceso->pid);
        enviar_entero(socket_memoria_global, id_segmento);
        enviar_entero(socket_memoria_global, tamanio);

        int respuesta = 0;
        recibir_entero(socket_memoria_global, &respuesta);
        pthread_mutex_unlock(&mutex_pedidos_memoria);

        if (respuesta == OP_OK) {
            enviar_entero(socket_cpu, OP_OK);
        } else if (respuesta == OP_COMPACTAR) {
            enviar_entero(socket_cpu, OP_BLOQUEADO);

            sem_wait(&sem_puede_planificar);
            proceso_esperando_memoria = proceso;
            cpu_esperando_memoria     = proceso->cpu;

            pthread_mutex_lock(&mutex_listas);
            list_remove_element(lista_exec, proceso);
            proceso->estado = BLOCK;
            proceso->cpu    = NULL;
            list_add(lista_block, proceso);
            compactando = true;
            pthread_mutex_unlock(&mutex_listas);

            log_info(logger, "## Inicio de compactacion");

            desalojar_todas_las_cpus();

            pthread_mutex_lock(&mutex_pedidos_memoria);
            enviar_entero(socket_memoria_global, OP_OK);
            pthread_mutex_unlock(&mutex_pedidos_memoria);
        }else{
            enviar_entero(socket_cpu, OP_ERROR);
        }
    } else if (tipo_syscall == OP_MEM_FREE) {
        pthread_mutex_lock(&mutex_pedidos_memoria);
        enviar_entero(socket_memoria_global, OP_ELIMINAR_SEGMENTO);
        enviar_entero(socket_memoria_global, proceso->pid);
        enviar_entero(socket_memoria_global, id_segmento);

        int respuesta = 0;
        recibir_entero(socket_memoria_global, &respuesta);
        pthread_mutex_unlock(&mutex_pedidos_memoria);

        enviar_entero(socket_cpu, respuesta == OP_OK ? OP_OK : OP_ERROR);
    }
}

void bloquear_proceso(t_proceso* proceso) {
    pthread_mutex_lock(&mutex_listas);
    cancelar_timer(proceso);
    t_cpu* cpu = proceso->cpu;
    list_remove_element(lista_exec, proceso);
    proceso->estado = BLOCK;
    proceso->cpu    = NULL;
    list_add(lista_block, proceso);
    arrancar_timer_suspension(proceso);
    pthread_mutex_unlock(&mutex_listas);

    log_info(logger, "## (%d) Pasa del estado EXEC al estado BLOCK", proceso->pid);

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_cpus_libres, cpu);
    pthread_mutex_unlock(&mutex_listas);

    sem_post(&sem_hay_cpus_libres);
}

void manejar_syscall(int socket_cpu) {
    int pid = 0;
    recibir_entero(socket_cpu, &pid);

    int tipo_syscall = 0;
    recibir_entero(socket_cpu, &tipo_syscall);

    log_info(logger, "## (%d) - Solicito syscall: %s", pid, nombre_syscall(tipo_syscall));

    pthread_mutex_lock(&mutex_listas);
    t_proceso* proceso = buscar_proceso_en_exec(pid);
    if (proceso == NULL) {
        log_warning(logger, "No se encontro el proceso %d en EXEC", pid);
        pthread_mutex_unlock(&mutex_listas);
        return;
    }
    pthread_mutex_unlock(&mutex_listas);

    switch (tipo_syscall) {
        case OP_IO_SLEEP:
            bloquear_proceso(proceso);
            manejar_io_sleep(proceso, socket_cpu);
            break;
        case OP_IO_STDIN:
            bloquear_proceso(proceso);
            manejar_io_stdin(proceso, socket_cpu);
            break;
        case OP_IO_STDOUT:
            bloquear_proceso(proceso);
            manejar_io_stdout(proceso, socket_cpu);
            break;
        case OP_MUTEX_CREATE:
            manejar_mutex_create(proceso, socket_cpu);
            break;
        case OP_MUTEX_LOCK:
            manejar_mutex_lock(proceso, socket_cpu);
            break;
        case OP_MUTEX_UNLOCK:
            manejar_mutex_unlock(proceso, socket_cpu);
            break;
        case OP_INIT_PROC:
            manejar_init_proc(socket_cpu);
            break;
        case OP_MEM_ALLOC:
        case OP_MEM_FREE:
            manejar_syscall_memoria(proceso, tipo_syscall, socket_cpu);
            break;
        default:
            log_warning(logger, "## (%d) - Syscall desconocida: %d", pid, tipo_syscall);
            enviar_entero(socket_cpu, OP_ERROR);
            break;
    }
}

/*
    ============================================================
    MANEJO DE IO
    ============================================================
*/
void manejar_fin_io(t_proceso* proceso) {
    pthread_mutex_lock(&mutex_listas);
    cancelar_timer(proceso);
    t_estado estado_actual = proceso->estado;
    pthread_mutex_unlock(&mutex_listas);

    if (estado_actual == BLOCK) {
        log_info(logger, "## (%d) finalizo IO y pasa a READY", proceso->pid);
        mover_proceso_de_block_a_ready(proceso);
    } else if (estado_actual == SUSP_BLOCK) {
        log_info(logger, "## (%d) finalizo IO y pasa a SUSP_READY", proceso->pid);
        pthread_mutex_lock(&mutex_listas);
        list_remove_element(lista_susp_block, proceso);
        proceso->estado = SUSP_READY;
        proceso->tiempo_suspension = time(NULL);
        list_add(lista_susp_ready, proceso);
        pthread_mutex_unlock(&mutex_listas);

        // El proceso ya esta listo para volver: intentamos de-suspenderlo ahora.
        // Sin esto quedaria en SUSP_READY hasta que llegue un OP_MEMORIA_DISPONIBLE,
        // que en escenarios sin actividad de memoria puede no ocurrir nunca.
        intentar_dessuspender_procesos();
    }
}

void atender_io(int socket_io) {
    int tipo = 0;
    recibir_entero(socket_io, &tipo);

    enviar_entero(socket_io, HANDSHAKE_OK);

    sem_t*  semaforo = semaforo_de_io(tipo);
    t_list* cola     = cola_de_io(tipo);

    if (semaforo == NULL || cola == NULL) {
        log_warning(logger, "Tipo de IO desconocido: %d", tipo);
        close(socket_io);
        return;
    }

    t_io* io    = malloc(sizeof(t_io));
    io->socket  = socket_io;
    io->tipo    = tipo;

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_ios, io);
    pthread_mutex_unlock(&mutex_listas);

    while (1) {
        sem_wait(semaforo);

        pthread_mutex_lock(&mutex_listas);
        t_proceso* proceso = list_remove(cola, 0);
        pthread_mutex_unlock(&mutex_listas);

        if (proceso == NULL) continue;

        switch (tipo) {
            case OP_IO_SLEEP:  ejecutar_io_sleep(io, proceso);  break;
            case OP_IO_STDIN:  ejecutar_io_stdin(io, proceso);  break;
            case OP_IO_STDOUT: ejecutar_io_stdout(io, proceso); break;
        }
    }
}

static t_list* cola_de_io(int tipo) {
    if (tipo == OP_IO_SLEEP)  return cola_espera_sleep;
    if (tipo == OP_IO_STDIN)  return cola_espera_stdin;
    if (tipo == OP_IO_STDOUT) return cola_espera_stdout;
    return NULL;
}

static sem_t* semaforo_de_io(int tipo) {
    if (tipo == OP_IO_SLEEP)  return &sem_cola_sleep;
    if (tipo == OP_IO_STDIN)  return &sem_cola_stdin;
    if (tipo == OP_IO_STDOUT) return &sem_cola_stdout;
    return NULL;
}

static void encolar_pedido_io(int tipo, t_proceso* proceso) {
    pthread_mutex_lock(&mutex_listas);
    list_add(cola_de_io(tipo), proceso);
    pthread_mutex_unlock(&mutex_listas);

    sem_post(semaforo_de_io(tipo));
}

void manejar_io_sleep(t_proceso* proceso, int socket_cpu) {
    int tiempo = 0;
    recibir_entero(socket_cpu, &tiempo);
    proceso->io_tiempo = tiempo;

    encolar_pedido_io(OP_IO_SLEEP, proceso);
}

void ejecutar_io_sleep(t_io* io, t_proceso* proceso) {

    enviar_entero(io->socket, proceso->pid);
    enviar_entero(io->socket, proceso->io_tiempo);

    int respuesta = 0;
    recibir_entero(io->socket, &respuesta);

    if (respuesta == OP_OK) {
        manejar_fin_io(proceso);
    }
}

void manejar_io_stdin(t_proceso* proceso, int socket_cpu) {
    int tamanio = 0;
    int direccion_fisica = 0;
    recibir_entero(socket_cpu, &tamanio);
    recibir_entero(socket_cpu, &direccion_fisica);
    proceso->io_tamanio          = tamanio;
    proceso->io_direccion_fisica = direccion_fisica;

    encolar_pedido_io(OP_IO_STDIN, proceso);
}

void ejecutar_io_stdin(t_io* io, t_proceso* proceso) {

    enviar_entero(io->socket, proceso->pid);
    enviar_entero(io->socket, proceso->io_tamanio);

    char* buffer = malloc(proceso->io_tamanio + 1);
    memset(buffer, '\0', proceso->io_tamanio + 1);
    recv(io->socket, buffer, proceso->io_tamanio, MSG_WAITALL);
    
    pthread_mutex_lock(&mutex_pedidos_memoria);
    enviar_entero(socket_memoria_global, OP_ESCRIBIR_DATOS);
    enviar_entero(socket_memoria_global, proceso->pid);
    enviar_entero(socket_memoria_global, proceso->io_direccion_fisica);
    enviar_entero(socket_memoria_global, proceso->io_tamanio);
    send(socket_memoria_global, buffer, proceso->io_tamanio, 0);

    int respuesta = 0;
    recibir_entero(socket_memoria_global, &respuesta);
    pthread_mutex_unlock(&mutex_pedidos_memoria);

    free(buffer);

    if (respuesta == OP_OK) {
        manejar_fin_io(proceso);
    }

}

void manejar_io_stdout(t_proceso* proceso, int socket_cpu) {
    int direccion_fisica = 0;
    int tamanio = 0;
    recibir_entero(socket_cpu, &direccion_fisica);
    recibir_entero(socket_cpu, &tamanio);
    proceso->io_direccion_fisica = direccion_fisica;
    proceso->io_tamanio          = tamanio;

    encolar_pedido_io(OP_IO_STDOUT, proceso);
}

void ejecutar_io_stdout(t_io* io, t_proceso* proceso) {

    char* buffer = malloc(proceso->io_tamanio + 1);
    memset(buffer, '\0', proceso->io_tamanio + 1);

    pthread_mutex_lock(&mutex_pedidos_memoria);
    enviar_entero(socket_memoria_global, OP_LEER_DATOS);
    enviar_entero(socket_memoria_global, proceso->pid);
    enviar_entero(socket_memoria_global, proceso->io_direccion_fisica);
    enviar_entero(socket_memoria_global, proceso->io_tamanio);
    
    int respuesta_memoria = 0;
    recibir_entero(socket_memoria_global, &respuesta_memoria);
    if (respuesta_memoria == OP_OK) {
        recv(socket_memoria_global, buffer, proceso->io_tamanio, MSG_WAITALL);
    }
    pthread_mutex_unlock(&mutex_pedidos_memoria);

    if (respuesta_memoria != OP_OK) {
        log_warning(logger, "## (%d) Kernel Memory no pudo leer los datos para STDOUT", proceso->pid);
    }

    enviar_entero(io->socket, proceso->pid);
    enviar_entero(io->socket, proceso->io_tamanio);
    send(io->socket, buffer, proceso->io_tamanio, 0);

    free(buffer);

    int respuesta = 0;
    recibir_entero(io->socket, &respuesta);

    if (respuesta == OP_OK) {
        manejar_fin_io(proceso);
    }
}

bool comparar_prioridad_suspension(void* elem_a, void* elem_b) {
    t_proceso* a = (t_proceso*) elem_a;
    t_proceso* b = (t_proceso*) elem_b;

    if (a->prioridad != b->prioridad)
        return a->prioridad < b->prioridad;

    return a->tiempo_suspension < b->tiempo_suspension;
}

void intentar_dessuspender_procesos(void) {
    pthread_mutex_lock(&mutex_listas);
    list_sort(lista_susp_ready, comparar_prioridad_suspension);
    pthread_mutex_unlock(&mutex_listas);

    for (int i = 0; i < list_size(lista_susp_ready); i++) {
        pthread_mutex_lock(&mutex_listas);
        t_proceso* proceso = list_get(lista_susp_ready, i);
        pthread_mutex_unlock(&mutex_listas);

        pthread_mutex_lock(&mutex_pedidos_memoria);
        enviar_entero(socket_memoria_global, OP_DESSUSPENDER_PROCESO);
        enviar_entero(socket_memoria_global, proceso->pid);

        int respuesta = 0;
        recibir_entero(socket_memoria_global, &respuesta);
        pthread_mutex_unlock(&mutex_pedidos_memoria);

        if (respuesta == OP_OK) {
            pthread_mutex_lock(&mutex_listas);
            list_remove_element(lista_susp_ready, proceso);
            proceso->estado = READY;
            bool planificable = encolar_en_ready(proceso);
            pthread_mutex_unlock(&mutex_listas);

            log_info(logger, "## (%d) Pasa del estado SUSP_READY al estado READY",
                     proceso->pid);

            if (planificable) sem_post(&sem_hay_procesos_ready);
            i--;
        }
    }
}

void desalojar_todas_las_cpus(void) {
    pthread_mutex_lock(&mutex_listas);

    int cantidad = list_size(lista_exec);
    int* sockets = malloc(cantidad * sizeof(int));
    int* pids    = malloc(cantidad * sizeof(int));

    for (int i = 0; i < cantidad; i++) {
        t_proceso* proceso = list_get(lista_exec, i);
        sockets[i] = proceso->cpu->socket;
        pids[i]    = proceso->pid;
    }

    pthread_mutex_unlock(&mutex_listas);

    for (int i = 0; i < cantidad; i++) {
        enviar_entero(sockets[i], OP_INTERRUMPIR_PROCESO);
        enviar_entero(sockets[i], pids[i]);
    }

    free(sockets);
    free(pids);
}

void manejar_bsod(void) {
    log_info(logger, "## BSOD - Memoria corrupta detectada");

    pthread_mutex_lock(&mutex_listas);

    bsod_activo = true;
    cpus_pendientes_bsod = list_size(lista_exec);

    t_list* todas[] = {
        lista_new, lista_ready, lista_block,
        lista_susp_ready, lista_susp_block, lista_ready_invalida
    };

    for (int l = 0; l < 6; l++) {
        for (int i = 0; i < list_size(todas[l]); i++) {
            t_proceso* proceso = list_get(todas[l], i);
            log_info(logger, "## (%d) Pasa del estado %s al estado EXIT",
                     proceso->pid, nombre_estado(proceso->estado));
            log_info(logger, "## (%d) finalizo su ejecucion con motivo de BSOD", proceso->pid);
        }
    }

    if (strcmp(algoritmo_planificacion, "CMN") == 0) {
        for (int c = 0; c < cantidad_colas; c++) {
            for (int i = 0; i < list_size(colas_ready[c]); i++) {
                t_proceso* proceso = list_get(colas_ready[c], i);
                log_info(logger, "## (%d) Pasa del estado %s al estado EXIT",
                         proceso->pid, nombre_estado(proceso->estado));
                log_info(logger, "## (%d) finalizo su ejecucion con motivo de BSOD", proceso->pid);
            }
        }
    }

    list_clean_and_destroy_elements(lista_new, destruir_proceso);
    list_clean_and_destroy_elements(lista_ready, destruir_proceso);
    list_clean_and_destroy_elements(lista_block, destruir_proceso);
    list_clean_and_destroy_elements(lista_susp_ready, destruir_proceso);
    list_clean_and_destroy_elements(lista_susp_block, destruir_proceso);
    list_clean_and_destroy_elements(lista_ready_invalida, destruir_proceso);
    if (strcmp(algoritmo_planificacion, "CMN") == 0) {
        for (int i = 0; i < cantidad_colas; i++) {
            list_clean_and_destroy_elements(colas_ready[i], destruir_proceso);
        }
    }

    int cantidad_exec = list_size(lista_exec);
    int* sockets = malloc(cantidad_exec * sizeof(int));
    int* pids    = malloc(cantidad_exec * sizeof(int));

    for (int i = 0; i < cantidad_exec; i++) {
        t_proceso* proceso = list_get(lista_exec, i);
        log_info(logger, "## (%d) Pasa del estado EXEC al estado EXIT", proceso->pid);
        log_info(logger, "## (%d) finalizo su ejecucion con motivo de BSOD", proceso->pid);
        sockets[i] = proceso->cpu->socket;
        pids[i]    = proceso->pid;
    }
    list_clean_and_destroy_elements(lista_exec, destruir_proceso);

    pthread_mutex_unlock(&mutex_listas);

    for (int i = 0; i < cantidad_exec; i++) {
        enviar_entero(sockets[i], OP_INTERRUMPIR_PROCESO);
        enviar_entero(sockets[i], pids[i]);
    }

    free(sockets);
    free(pids);

    if (cantidad_exec == 0) {
        exit(EXIT_FAILURE);
    }
}

static void destruir_proceso(void* elemento) {
    free(elemento);
}

void verificar_fin_bsod(void) {
    pthread_mutex_lock(&mutex_listas);
    cpus_pendientes_bsod--;
    int pendientes = cpus_pendientes_bsod;
    pthread_mutex_unlock(&mutex_listas);

    if (pendientes == 0) {
        exit(EXIT_FAILURE);
    }
}

void* hilo_escuchar_memoria(void* arg) {
    while (1) {
        int op = 0;
        if (!recibir_entero(socket_notificaciones_memoria, &op)){
            break;
        }

        switch (op) {
            case OP_MEMORIA_DISPONIBLE:
                intentar_dessuspender_procesos();
                break;
            case OP_FIN_COMPACTACION:
                log_info(logger, "## Fin de compactacion");
                pthread_mutex_lock(&mutex_listas);
                if (proceso_esperando_memoria != NULL) {
                    proceso_esperando_memoria->estado = EXEC;
                    proceso_esperando_memoria->cpu    = cpu_esperando_memoria;
                    list_add(lista_exec, proceso_esperando_memoria);
                    list_remove_element(lista_block, proceso_esperando_memoria);
                }
                compactando = false;
                pthread_mutex_unlock(&mutex_listas);

                if (proceso_esperando_memoria != NULL) {
                    enviar_entero(cpu_esperando_memoria->socket, OP_EJECUTAR_PROCESO);
                    enviar_entero(cpu_esperando_memoria->socket, proceso_esperando_memoria->pid);
                    proceso_esperando_memoria = NULL;
                    cpu_esperando_memoria     = NULL;
                }

                sem_post(&sem_puede_planificar);
                break;
            case OP_MEMORIA_CORRUPTA:
                manejar_bsod();
                break;
            default:
                log_warning(logger, "Operacion desconocida de Kernel Memory: %d", op);
                break;
        }
    }
    return NULL;
}

/*
    ============================================================
    MANEJO DE NUEVO PROCESO
    ============================================================
*/

void manejar_init_proc(int socket_cpu) {
    char* path      = recibir_string(socket_cpu);
    int   prioridad = 0;
    recibir_entero(socket_cpu, &prioridad);

    t_proceso* proceso = malloc(sizeof(t_proceso));
    pthread_mutex_lock(&mutex_listas);
    proceso->pid                = proximo_pid++;
    pthread_mutex_unlock(&mutex_listas);
    proceso->prioridad          = prioridad;
    proceso->prioridad_original = prioridad;
    proceso->estado             = NEW;
    proceso->socket_cpu         = -1;
    proceso->cpu                = NULL;
    proceso->timer              = NULL;

    log_info(logger, "## (%d) Se crea el proceso - Estado: NEW", proceso->pid);

    pthread_mutex_lock(&mutex_pedidos_memoria);
    enviar_entero(socket_memoria_global, OP_CREAR_PROCESO);
    enviar_entero(socket_memoria_global, proceso->pid);
    enviar_string(socket_memoria_global, path);
    free(path);

    int respuesta = 0;                                       
    recibir_entero(socket_memoria_global, &respuesta);
    pthread_mutex_unlock(&mutex_pedidos_memoria);

    if (respuesta != OP_OK) {
        log_error(logger, "## (%d) Kernel Memory no pudo crear el proceso", proceso->pid);
        free(proceso);
        enviar_entero(socket_cpu, OP_ERROR);
        return;
    }

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_new, proceso);
    pthread_mutex_unlock(&mutex_listas);

    mover_proceso_a_ready(proceso);

    enviar_entero(socket_cpu, OP_OK);
}

/*
    ============================================================
    MUTEX DE LOS PROCESOS 
    ============================================================
*/

void manejar_mutex_create(t_proceso* proceso, int socket_cpu) {
    char* nombre = recibir_string(socket_cpu);

    pthread_mutex_lock(&mutex_listas);
    t_mutex* existente = buscar_mutex(nombre);
    if (existente != NULL) {
        pthread_mutex_unlock(&mutex_listas);
        log_warning(logger, "## (%d) Intento crear un Mutex que ya existe: %s", proceso->pid, nombre);
        free(nombre);
        enviar_entero(socket_cpu, OP_OK);
        return;
    }
    pthread_mutex_unlock(&mutex_listas);

    t_mutex* mutex     = malloc(sizeof(t_mutex));
    mutex->nombre      = nombre;
    mutex->pid_dueno   = -1;
    mutex->cola_espera = list_create();

    pthread_mutex_lock(&mutex_listas);
    list_add(lista_mutex, mutex);
    pthread_mutex_unlock(&mutex_listas);

    log_info(logger, "## (%d) Creo el Mutex %s", proceso->pid, mutex->nombre);

    enviar_entero(socket_cpu, OP_OK);
}

int mayor_prioridad_del_mutex(t_mutex* mutex) {
    int mejor = -1;
    for (int i = 0; i < list_size(mutex->cola_espera); i++) {
        t_proceso* esperando = list_get(mutex->cola_espera, i);
        if (mejor == -1 || esperando->prioridad < mejor) {
            mejor = esperando->prioridad;
        }
    }
    return mejor;
}

void aplicar_herencia(t_proceso* proceso, t_mutex* mutex) {
    int mejor = mayor_prioridad_del_mutex(mutex);
    if (mejor == -1 || mejor >= proceso->prioridad) return;

    log_info(logger, "## %d Cambio de prioridad: %d - %d",
             proceso->pid, proceso->prioridad, mejor);

    if (proceso->estado == READY) {
        list_remove_element(colas_ready[proceso->prioridad], proceso);
        proceso->prioridad = mejor;
        encolar_en_ready(proceso);
    } else {
        proceso->prioridad = mejor;
    }
}

void devolver_prioridad(t_proceso* proceso) {
    int mejor = proceso->prioridad_original;

    for (int i = 0; i < list_size(lista_mutex); i++) {
        t_mutex* mutex = list_get(lista_mutex, i);
        if (mutex->pid_dueno != proceso->pid) continue;

        int candidato = mayor_prioridad_del_mutex(mutex);
        if (candidato != -1 && candidato < mejor) {
            mejor = candidato;
        }
    }

    if (mejor == proceso->prioridad) return;

    log_info(logger, "## %d Cambio de prioridad: %d - %d",
             proceso->pid, proceso->prioridad, mejor);
    proceso->prioridad = mejor;
}

void manejar_mutex_lock(t_proceso* proceso, int socket_cpu) {
    char* nombre = recibir_string(socket_cpu);

    pthread_mutex_lock(&mutex_listas);
    t_mutex* mutex = buscar_mutex(nombre);
    free(nombre);

    if (mutex == NULL) {
        log_warning(logger, "No existe el mutex");
        pthread_mutex_unlock(&mutex_listas);
        enviar_entero(socket_cpu, OP_ERROR);
        return;
    }

    if (mutex->pid_dueno == -1) {
        mutex->pid_dueno = proceso->pid;
        pthread_mutex_unlock(&mutex_listas);

        log_info(logger, "## (%d) Toma el Mutex %s", proceso->pid, mutex->nombre);
        enviar_entero(socket_cpu, OP_OK);
    } else {
        list_add(mutex->cola_espera, proceso);

        t_proceso* dueno = buscar_proceso_por_pid(mutex->pid_dueno);
        if (dueno != NULL && strcmp(algoritmo_planificacion, "CMN") == 0) {
            aplicar_herencia(dueno, mutex);
        }

        pthread_mutex_unlock(&mutex_listas);

        enviar_entero(socket_cpu, OP_BLOQUEADO);
        bloquear_proceso(proceso);
    }
}

void manejar_mutex_unlock(t_proceso* proceso, int socket_cpu) {
    char* nombre = recibir_string(socket_cpu);

    pthread_mutex_lock(&mutex_listas);
    t_mutex* mutex = buscar_mutex(nombre);
    free(nombre);

    if (mutex == NULL) {
        log_warning(logger, "No existe el mutex");
        pthread_mutex_unlock(&mutex_listas);
        enviar_entero(socket_cpu, OP_ERROR);
        return;
    }

    log_info(logger, "## (%d) Libera el Mutex %s", proceso->pid, mutex->nombre);

    
    if (list_size(mutex->cola_espera) > 0) {
        t_proceso* siguiente = list_remove(mutex->cola_espera, 0);
        mutex->pid_dueno     = siguiente->pid;


        if (strcmp(algoritmo_planificacion, "CMN") == 0){
            devolver_prioridad(proceso);
            aplicar_herencia(siguiente, mutex);
        }
        t_estado estado_actual = siguiente->estado;
        cancelar_timer(siguiente);
        pthread_mutex_unlock(&mutex_listas);

        log_info(logger, "## (%d) Toma el Mutex %s", siguiente->pid, mutex->nombre);

        if (estado_actual == BLOCK) {
            mover_proceso_de_block_a_ready(siguiente);

        } else if (estado_actual == SUSP_BLOCK) {
            pthread_mutex_lock(&mutex_listas);
            list_remove_element(lista_susp_block, siguiente);
            siguiente->estado = SUSP_READY;
            siguiente->tiempo_suspension = time(NULL);
            list_add(lista_susp_ready, siguiente);
            pthread_mutex_unlock(&mutex_listas);

            log_info(logger, "## (%d) Pasa del estado SUSP_BLOCK al estado SUSP_READY", siguiente->pid);

        } else {
            log_warning(logger, "## (%d) tenia el mutex %s pero su estado era %s (inesperado)",
                        siguiente->pid, mutex->nombre, nombre_estado(estado_actual));
        }
    } else {
        mutex->pid_dueno = -1;

        if (strcmp(algoritmo_planificacion, "CMN") == 0)
            devolver_prioridad(proceso);

        pthread_mutex_unlock(&mutex_listas);
    }
    

    enviar_entero(socket_cpu, OP_OK);
}

/*
    ============================================================
    FUNCIONES AUXILIARES
    ============================================================
*/

t_proceso* buscar_proceso_en_exec(int pid) {
    for (int i = 0; i < list_size(lista_exec); i++) {
        t_proceso* proceso = list_get(lista_exec, i);
        if (proceso->pid == pid) return proceso;
    }
    return NULL;
}

t_proceso* buscar_proceso_por_pid(int pid) {
    t_list* listas[] = {
        lista_exec,
        lista_block,
        lista_susp_ready,
        lista_susp_block
    };

    for (int l = 0; l < 4; l++) {
        for (int i = 0; i < list_size(listas[l]); i++) {
            t_proceso* proceso = list_get(listas[l], i);
            if (proceso->pid == pid) {
                return proceso;
            }
        }
    }

    if (strcmp(algoritmo_planificacion, "CMN") == 0) {
        for (int c = 0; c < cantidad_colas; c++) {
            for (int i = 0; i < list_size(colas_ready[c]); i++) {
                t_proceso* proceso = list_get(colas_ready[c], i);
                if (proceso->pid == pid) {
                    return proceso;
                }
            }
        }
    } else {
        for (int i = 0; i < list_size(lista_ready); i++) {
            t_proceso* proceso = list_get(lista_ready, i);
            if (proceso->pid == pid) {
                return proceso;
            }
        }
    }

    return NULL;
}

t_mutex* buscar_mutex(char* nombre) {
    for (int i = 0; i < list_size(lista_mutex); i++) {
        t_mutex* mutex = list_get(lista_mutex, i);
        if (strcmp(mutex->nombre, nombre) == 0) return mutex;
    }
    return NULL;
}

const char* nombre_estado(t_estado estado) {
    switch (estado) {
        case NEW:        return "NEW";
        case READY:      return "READY";
        case EXEC:       return "EXEC";
        case BLOCK:      return "BLOCK";
        case SUSP_READY: return "SUSP_READY";
        case SUSP_BLOCK: return "SUSP_BLOCK";
        case EXIT_STATE: return "EXIT";
        default:         return "DESCONOCIDO";
    }
}

const char* nombre_syscall(int tipo) {
    switch (tipo) {
        case OP_IO_SLEEP:  return "SLEEP";
        case OP_IO_STDIN:  return "STDIN";
        case OP_IO_STDOUT: return "STDOUT";
        case OP_MUTEX_CREATE: return "MUTEX_CREATE";
        case OP_MUTEX_LOCK:   return "MUTEX_LOCK";
        case OP_MUTEX_UNLOCK: return "MUTEX_UNLOCK";
        case OP_MEM_ALLOC:    return "MEM_ALLOC";
        case OP_MEM_FREE:     return "MEM_FREE";
        case OP_INIT_PROC:    return "INIT_PROC";
        default:           return "DESCONOCIDA";

    }
}

void liberar_recursos(int socket_pedidos, int socket_notificaciones, int socket_servidor) {
    if (socket_pedidos  != -1) close(socket_pedidos);
    if (socket_notificaciones != -1) close(socket_notificaciones);
    if (socket_servidor != -1) close(socket_servidor);
    if (config != NULL) {
        config_destroy(config);
        config = NULL;
    }
    if (logger != NULL) {
        log_destroy(logger);
        logger = NULL;
    }
}