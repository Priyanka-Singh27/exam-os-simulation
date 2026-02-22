#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "shared.h"
#include "config.h"
#include "logger.h"
#include "scheduler.h"
#include "memory.h"
#include "io_buffer.h"
#include "interrupt.h"
#include "dashboard.h"

// ─── Global instances ─────────────────────────────────────
SystemState g_state;
IOBuffer    g_io_buffer;
Config      g_config;

// ─── Init global state ────────────────────────────────────
static void state_init() {
    memset(&g_state, 0, sizeof(SystemState));
    g_state.running_pid        = -1;
    g_state.simulation_running =  1;
    g_state.current_tick       =  0;
    for (int i = 0; i < 3; i++)
        strncpy(g_state.recent_logs[i], "--- no events yet ---", 255);
    pthread_mutex_init(&g_state.lock, NULL);
}

// ─── Simulation tick thread ───────────────────────────────
// Central clock — increments tick every TIME_TICK_MS
static void *tick_thread(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&g_state.lock);
        int running = g_state.simulation_running;
        if (running) g_state.current_tick++;
        pthread_mutex_unlock(&g_state.lock);

        if (!running) break;
        usleep(TIME_TICK_MS * 1000);
    }
    return NULL;
}

// ─── Print startup banner ─────────────────────────────────
static void print_banner() {
    printf("\n");
    printf("  ╔═══════════════════════════════════════════╗\n");
    printf("  ║      EXAM OS SIMULATION  v1.0             ║\n");
    printf("  ║  CPU Scheduling | Paging | I/O | Signals  ║\n");
    printf("  ╚═══════════════════════════════════════════╝\n\n");
}

int main(int argc, char *argv[]) {
    srand(time(NULL));

    print_banner();

    // ─── Load config ──────────────────────────────────────
    config_load_defaults(&g_config);
    config_parse_file(&g_config, "config.conf");
    config_parse_args(&g_config, argc, argv);
    config_print(&g_config);

    if (g_config.demo_mode)
        printf("\n  [DEMO MODE] Submission storm at tick 30\n");

    printf("\n  Starting simulation in 2 seconds...\n\n");
    sleep(2);

    // ─── Init all subsystems ──────────────────────────────
    state_init();
    logger_init();
    scheduler_init();
    memory_init();
    io_buffer_init();
    interrupt_init();
    dashboard_init();

    // ─── Spawn all threads ────────────────────────────────
    pthread_t t_tick, t_logger, t_scheduler,
              t_memory, t_io, t_interrupt, t_dashboard;

    pthread_create(&t_tick,      NULL, tick_thread,       NULL);
    pthread_create(&t_logger,    NULL, logger_thread,     NULL);
    pthread_create(&t_scheduler, NULL, scheduler_thread,  NULL);
    pthread_create(&t_memory,    NULL, memory_thread,     NULL);
    pthread_create(&t_io,        NULL, io_buffer_thread,  NULL);
    pthread_create(&t_interrupt, NULL, interrupt_thread,  NULL);
    pthread_create(&t_dashboard, NULL, dashboard_thread,  NULL);

    // ─── Run until exam_duration ticks or 'q' pressed ────
    while (1) {
        pthread_mutex_lock(&g_state.lock);
        int tick    = g_state.current_tick;
        int running = g_state.simulation_running;
        int done    = g_state.completed_processes;
        pthread_mutex_unlock(&g_state.lock);

        // End conditions
        if (!running) break;
        if (tick >= g_config.exam_duration) {
            pthread_mutex_lock(&g_state.lock);
            g_state.simulation_running = 0;
            pthread_mutex_unlock(&g_state.lock);
            break;
        }
        if (done >= g_config.num_students) {
            pthread_mutex_lock(&g_state.lock);
            g_state.simulation_running = 0;
            pthread_mutex_unlock(&g_state.lock);
            break;
        }

        usleep(TIME_TICK_MS * 1000);
    }

    // ─── Shutdown sequence ────────────────────────────────
    // Signal all threads to stop
    pthread_mutex_lock(&g_state.lock);
    g_state.simulation_running = 0;
    pthread_mutex_unlock(&g_state.lock);

    io_buffer_shutdown();
    logger_shutdown();
    dashboard_shutdown();

    // Wait for all threads
    pthread_join(t_dashboard, NULL);
    pthread_join(t_interrupt, NULL);
    pthread_join(t_io,        NULL);
    pthread_join(t_memory,    NULL);
    pthread_join(t_scheduler, NULL);
    pthread_join(t_logger,    NULL);
    pthread_join(t_tick,      NULL);

    // ─── Write final report ───────────────────────────────
    printf("\n  Simulation complete. Writing report...\n");
    logger_write_report();

    printf("\n  Output files:\n");
    printf("    output/system_log.txt   — full event log\n");
    printf("    output/submissions.txt  — all submissions\n");
    printf("    output/summary.txt      — final statistics\n\n");

    // ─── Cleanup ──────────────────────────────────────────
    pthread_mutex_destroy(&g_state.lock);
    pthread_mutex_destroy(&g_io_buffer.lock);
    sem_destroy(&g_io_buffer.empty_slots);
    sem_destroy(&g_io_buffer.filled_slots);

    return 0;
}