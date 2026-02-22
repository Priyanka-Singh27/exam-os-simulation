#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "shared.h"

void  scheduler_init();
void *scheduler_thread(void *arg);
void  scheduler_add_process(PCB process);
void  scheduler_terminate_process(int pid);

#endif // SCHEDULER_H