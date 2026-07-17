#include "huecos.h"
#include <stdlib.h>

t_list* lista_huecos;

void huecos_init() {
    lista_huecos = list_create();
}

// se llama cada vez que se conecta un Memory Stick nuevo. base_nueva es la direccion global donde arranca la memoria agregada (la memoria_total antes de agregar el stock ) 
void huecos_extender(uint32_t base_nueva, uint32_t bytes_nuevos) {
    int cant = list_size(lista_huecos);
    if (cant > 0) {
        t_hueco* ultimo = list_get(lista_huecos, cant - 1);
        // solo se fusiona si el ultimo hueco termina justo donde
        // arranca la memoria nueva
        if (ultimo->base + ultimo->tamanio == base_nueva) {
            ultimo->tamanio += bytes_nuevos;
            return;
        }
    }
    t_hueco* hueco = malloc(sizeof(t_hueco));
    hueco->base = base_nueva;
    hueco->tamanio = bytes_nuevos;
    list_add(lista_huecos, hueco);
}

// Best Fit: el hueco más chico donde entre
t_hueco* buscar_hueco_best_fit(uint32_t tamanio_requerido) {
    t_hueco* mejor = NULL;
    for (int i = 0; i < list_size(lista_huecos); i++) {
        t_hueco* h = list_get(lista_huecos, i);
        if (h->tamanio >= tamanio_requerido) {
            if (mejor == NULL || h->tamanio < mejor->tamanio) {
                mejor = h;
            }
        }
    }
    return mejor;
}

// Worst Fit: el hueco más grande donde entre
t_hueco* buscar_hueco_worst_fit(uint32_t tamanio_requerido) {
    t_hueco* peor = NULL;
    for (int i = 0; i < list_size(lista_huecos); i++) {
        t_hueco* h = list_get(lista_huecos, i);
        if (h->tamanio >= tamanio_requerido) {
            if (peor == NULL || h->tamanio > peor->tamanio) {
                peor = h;
            }
        }
    }
    return peor;
}

// ocupa una parte del hueco encontrado: lo achica o lo elimina si se usa entero
void hueco_ocupar(t_hueco* hueco, uint32_t tamanio_usado) {
    if (hueco->tamanio == tamanio_usado) {
        // se usó completo: hay que sacarlo de la lista
        for (int i = 0; i < list_size(lista_huecos); i++) {
            if (list_get(lista_huecos, i) == hueco) {
                list_remove(lista_huecos, i);
                free(hueco);
                return;
            }
        }
    } else {
        // sobra espacio: el hueco se corre hacia adelante
        hueco->base += tamanio_usado;
        hueco->tamanio -= tamanio_usado;
    }
}

// al eliminar un segmento, vuelve a aparecer ese espacio como libre
void hueco_liberar(uint32_t base, uint32_t tamanio) {
    t_hueco* nuevo = malloc(sizeof(t_hueco));
    nuevo->base = base;
    nuevo->tamanio = tamanio;
    list_add(lista_huecos, nuevo);
}
/*
    ────────────────────────────────────────────────────────────────
    PENDIENTE DE DISCUTIR CON EL GRUPO: hueco_liberar con COALESCING
    ────────────────────────────────────────────────────────────────
    El "coalescing" es fusionar huecos que quedan pegados (contiguos) en
    uno solo más grande. NO lo pide la consigna: es una optimización.

    Por qué serviría: si liberás dos segmentos pegados de 64 y NO los
    fusionás, quedan dos huecos de 64 sueltos. Si después alguien pide 128,
    no entra en ninguno -> se dispara una compactación innecesaria (mover
    todo). Con coalescing, esos dos 64 se fusionan en 128 y entra directo.

    Contra: es más código y hay que mantener la lista ordenada por base.

    Si el grupo decide usarlo, se reemplaza la función de arriba por esta:

    void hueco_liberar(uint32_t base, uint32_t tamanio) {
        // 1) insertar ordenado por base (primer hueco con base mayor)
        int pos = 0;
        while (pos < list_size(lista_huecos)) {
            t_hueco* h = list_get(lista_huecos, pos);
            if (h->base > base) break;
            pos++;
        }
        t_hueco* nuevo = malloc(sizeof(t_hueco));
        nuevo->base = base;
        nuevo->tamanio = tamanio;
        list_add_in_index(lista_huecos, pos, nuevo);

        // 2) fusionar con el SIGUIENTE si son contiguos
        if (pos + 1 < list_size(lista_huecos)) {
            t_hueco* siguiente = list_get(lista_huecos, pos + 1);
            if (nuevo->base + nuevo->tamanio == siguiente->base) {
                nuevo->tamanio += siguiente->tamanio;
                list_remove(lista_huecos, pos + 1);
                free(siguiente);
            }
        }
        // 3) fusionar con el ANTERIOR si son contiguos
        if (pos > 0) {
            t_hueco* anterior = list_get(lista_huecos, pos - 1);
            if (anterior->base + anterior->tamanio == nuevo->base) {
                anterior->tamanio += nuevo->tamanio;
                list_remove(lista_huecos, pos);
                free(nuevo);
            }
        }
    }
    ────────────────────────────────────────────────────────────────
*/

// suma de todos los huecos: espacio libre total
uint32_t huecos_total_libre(void) {
    uint32_t total = 0;
    for (int i = 0; i < list_size(lista_huecos); i++) {
        t_hueco* h = list_get(lista_huecos, i);
        total += h->tamanio;
    }
    return total;
}

// tras compactar, todo lo libre queda en un unico hueco al final
void huecos_reconstruir(uint32_t base_libre, uint32_t memoria_total) {
    list_clean_and_destroy_elements(lista_huecos, free);
    if (base_libre < memoria_total) {
        t_hueco* hueco = malloc(sizeof(t_hueco));
        hueco->base = base_libre;
        hueco->tamanio = memoria_total - base_libre;
        list_add(lista_huecos, hueco);
    }
}
