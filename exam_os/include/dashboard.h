#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "shared.h"

void  dashboard_init();
void  dashboard_shutdown();
void *dashboard_thread(void *arg);

#endif // DASHBOARD_H