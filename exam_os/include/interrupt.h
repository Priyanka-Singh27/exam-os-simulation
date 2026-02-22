#ifndef INTERRUPT_H
#define INTERRUPT_H

#include "shared.h"

// Interrupt IDs
#define INT_EXAM_TIMEOUT    0
#define INT_OVERLOAD        1
#define INT_PAGE_FAULT      2
#define INT_SUBMIT_COMPLETE 3

void  interrupt_init();
void  interrupt_raise(int interrupt_id, int pid);
void *interrupt_thread(void *arg);

#endif // INTERRUPT_H