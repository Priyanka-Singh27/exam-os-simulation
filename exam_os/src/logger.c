#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "logger.h"

// ─── Internal log queue ──────────────────────────────────
static LogEntry   log_queue[MAX_LOG_QUEUE];
static int        q_head = 0, q_tail = 0, q_count = 0;
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t           q_ready;
static FILE           *log_file = NULL;
static int             logger_running = 1;

// ─── Get nanosecond timestamp ────────────────────────────
static long get_timestamp_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

void logger_init() {
    sem_init(&q_ready, 0, 0);
    log_file = fopen("output/system_log.txt", "w");
    if (!log_file) {
        fprintf(stderr, "WARNING: Could not open log file. Logging to stderr.\n");
        log_file = stderr;
    }
    fprintf(log_file, "=== EXAM OS SIMULATION LOG ===\n\n");
    fflush(log_file);
}

void logger_shutdown() {
    logger_running = 0;
    sem_post(&q_ready);  // wake thread so it can exit
}

// Called by any module — thread safe
void log_event(const char *level, const char *subsystem, const char *message) {
    pthread_mutex_lock(&q_lock);

    if (q_count < MAX_LOG_QUEUE) {
        LogEntry *e = &log_queue[q_tail];
        e->timestamp_ns = get_timestamp_ns();
        strncpy(e->level,     level,     sizeof(e->level)     - 1);
        strncpy(e->subsystem, subsystem, sizeof(e->subsystem) - 1);
        strncpy(e->message,   message,   sizeof(e->message)   - 1);

        q_tail = (q_tail + 1) % MAX_LOG_QUEUE;
        q_count++;
        sem_post(&q_ready);
    }
    // if queue full, silently drop (never block the caller)

    pthread_mutex_unlock(&q_lock);

    // Also update dashboard recent logs
    pthread_mutex_lock(&g_state.lock);
    int idx = g_state.log_index % 3;
    snprintf(g_state.recent_logs[idx], 255, "[%-9s] %-11s %s", level, subsystem, message);
    g_state.log_index++;
    pthread_mutex_unlock(&g_state.lock);
}

// Runs in its own thread — drains queue and writes to file
void *logger_thread(void *arg) {
    (void)arg;

    while (1) {
        sem_wait(&q_ready);

        if (!logger_running && q_count == 0) break;

        pthread_mutex_lock(&q_lock);
        if (q_count == 0) {
            pthread_mutex_unlock(&q_lock);
            continue;
        }

        LogEntry e = log_queue[q_head];
        q_head = (q_head + 1) % MAX_LOG_QUEUE;
        q_count--;
        pthread_mutex_unlock(&q_lock);

        // Write to file
        long ms = e.timestamp_ns / 1000000;
        fprintf(log_file, "[%8ld ms] [%-5s] [%-10s] %s\n",
                ms, e.level, e.subsystem, e.message);
        fflush(log_file);
    }

    if (log_file && log_file != stderr) fclose(log_file);
    return NULL;
}

// Called at simulation end
void logger_write_report() {
    FILE *f = fopen("output/summary.txt", "w");
    if (!f) return;

    pthread_mutex_lock(&g_state.lock);

    int total = g_state.page_faults + g_state.page_hits;
    float hit_rate = total > 0
        ? (float)g_state.page_hits / total * 100.0f
        : 0.0f;

    fprintf(f, "╔══════════════════════════════════════════╗\n");
    fprintf(f, "║       EXAM OS SIMULATION REPORT          ║\n");
    fprintf(f, "╠══════════════════════════════════════════╣\n");
    fprintf(f, "║ CPU                                      ║\n");
    fprintf(f, "║   Context Switches  : %-18d ║\n", g_state.context_switches);
    fprintf(f, "║   Completed Exams   : %-18d ║\n", g_state.completed_processes);
    fprintf(f, "║   Timeouts Fired    : %-18d ║\n", g_state.timeouts_fired);
    fprintf(f, "╠══════════════════════════════════════════╣\n");
    fprintf(f, "║ MEMORY                                   ║\n");
    fprintf(f, "║   Page Faults       : %-18d ║\n", g_state.page_faults);
    fprintf(f, "║   Page Hits         : %-18d ║\n", g_state.page_hits);
    fprintf(f, "║   Hit Rate          : %-17.1f%% ║\n", hit_rate);
    fprintf(f, "╠══════════════════════════════════════════╣\n");
    fprintf(f, "║ I/O BUFFER                               ║\n");
    fprintf(f, "║   Total Submissions : %-18d ║\n", g_state.total_submissions);
    fprintf(f, "║   Dropped           : %-18d ║\n", g_state.dropped_submissions);
    fprintf(f, "║   Flush Count       : %-18d ║\n", g_state.flush_count);
    fprintf(f, "╠══════════════════════════════════════════╣\n");
    fprintf(f, "║ INTERRUPTS                               ║\n");
    fprintf(f, "║   Overload Signals  : %-18d ║\n", g_state.overload_signals);
    fprintf(f, "╚══════════════════════════════════════════╝\n");

    pthread_mutex_unlock(&g_state.lock);
    fclose(f);

    // Print to terminal too
    printf("\n");
    system("cat output/summary.txt");
}