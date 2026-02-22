#ifndef MEMORY_H
#define MEMORY_H

#include "shared.h"

void  memory_init();
void *memory_thread(void *arg);
int   memory_access(int pid, int virtual_page);
void  memory_free_process(int pid);

#endif // MEMORY_H