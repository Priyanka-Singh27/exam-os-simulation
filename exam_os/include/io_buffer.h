#ifndef IO_BUFFER_H
#define IO_BUFFER_H

#include "shared.h"

void  io_buffer_init();
void  io_buffer_shutdown();
int   io_buffer_submit(int pid, int question_id, const char *answer, int is_partial);
void *io_buffer_thread(void *arg);

#endif // IO_BUFFER_H