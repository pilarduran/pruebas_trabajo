#ifndef HANDLERS_H
#define HANDLERS_H

void* atender_cliente(void* arg);
void atender_ks(int fd);
void atender_cpu(int fd);
void atender_memory_stick(int fd);

#endif