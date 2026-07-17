#include "contexto.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <commons/log.h>

extern t_log* logger; // el logger real vive en main.c

char* scripts_basepath = NULL;
t_list* lista_contextos;

// inicializa la lista — llamar una vez al arrancar
void contextos_init() {
    lista_contextos = list_create();
}

// crea un contexto nuevo con todo en 0
t_contexto* contexto_crear(int pid, char* path) {
    t_contexto* ctx = malloc(sizeof(t_contexto));
    ctx->pid = pid;
    ctx->path_instrucciones = strdup(path);
    // todos los registros en 0
    memset(&ctx->registros, 0, sizeof(t_registros));
    
    // cargar instrucciones del archivo
    ctx->instrucciones = list_create();
    ctx->tabla_segmentos = list_create();
    // El path recibido es relativo a SCRIPTS_BASEPATH (config). Si no hay basepath configurado, se usa el path tal cual llego.
    char path_completo[512];
    if (scripts_basepath != NULL && strlen(scripts_basepath) > 0) {
        snprintf(path_completo, sizeof(path_completo), "%s/%s", scripts_basepath, path);
    } else {
        snprintf(path_completo, sizeof(path_completo), "%s", path);
    }
    FILE* archivo = fopen(path_completo, "r");
    if (archivo != NULL) {
        char linea[256];
        while (fgets(linea, sizeof(linea), archivo)) {
            linea[strcspn(linea, "\n")] = '\0';
            list_add(ctx->instrucciones, strdup(linea));
        }
        fclose(archivo);
    } else {
        log_warning(logger, "No se pudo abrir el archivo: %s", path_completo);
    }

    

    list_add(lista_contextos, ctx);
    return ctx;
}

// busca un contexto por PID
t_contexto* contexto_buscar(int pid) {
    for (int i = 0; i < list_size(lista_contextos); i++) {
        t_contexto* ctx = list_get(lista_contextos, i);
        if (ctx->pid == pid) {
            return ctx;
        }
    }
    return NULL; // no encontrado
}

// elimina un contexto por PID
void contexto_eliminar(int pid) {
    for (int i = 0; i < list_size(lista_contextos); i++) {
        t_contexto* ctx = list_get(lista_contextos, i);
        if (ctx->pid == pid) {
            list_remove(lista_contextos, i);
            free(ctx->path_instrucciones);
            list_destroy_and_destroy_elements(ctx->instrucciones, free);
            list_destroy_and_destroy_elements(ctx->tabla_segmentos, free); 
            free(ctx);
            return;
        }
    }
}

void contexto_guardar(int pid, t_registros registros) {
    t_contexto* ctx = contexto_buscar(pid);
    if (ctx != NULL) {
        ctx->registros = registros;
    }
}