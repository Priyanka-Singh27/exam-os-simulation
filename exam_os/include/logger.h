#ifndef LOGGER_H
#define LOGGER_H

#include "shared.h"

void logger_init();
void logger_shutdown();
void log_event(const char *level, const char *subsystem, const char *message);
void *logger_thread(void *arg);
void logger_write_report();

#endif // LOGGER_H