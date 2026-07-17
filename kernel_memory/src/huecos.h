#ifndef HUECOS_H
#define HUECOS_H

#include <stdint.h>
#include <commons/collections/list.h>

typedef struct {
    uint32_t base;
    uint32_t tamanio;
} t_hueco;

extern t_list* lista_huecos;

void huecos_init();
void huecos_extender(uint32_t base_nueva, uint32_t bytes_nuevos);
uint32_t huecos_total_libre(void);
void huecos_reconstruir(uint32_t base_libre, uint32_t memoria_total);
t_hueco* buscar_hueco_best_fit(uint32_t tamanio_requerido);
t_hueco* buscar_hueco_worst_fit(uint32_t tamanio_requerido);
void hueco_ocupar(t_hueco* hueco, uint32_t tamanio_usado);
void hueco_liberar(uint32_t base, uint32_t tamanio);

#endif