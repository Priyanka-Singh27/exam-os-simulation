#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "io_buffer.h"
#include "logger.h"

#define FLUSH_THRESHOLD 0.80  // flush when 80% full

static FILE *disk_file   = NULL;
static int   io_running  = 1;

// ─── Timestamp ────────────────────────────────────────────
static long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// ─── Init ─────────────────────────────────────────────────
void io_buffer_init() {
    g_io_buffer.head  = 0;
    g_io_buffer.tail  = 0;
    g_io_buffer.count = 0;

    sem_init(&g_io_buffer.empty_slots, 0, BUFFER_CAPACITY);
    sem_init(&g_io_buffer.filled_slots, 0, 0);
    pthread_mutex_init(&g_io_buffer.lock, NULL);

    disk_file = fopen("output/submissions.txt", "w");
    if (!disk_file) {
        fprintf(stderr, "WARNING: Could not open submissions file\n");
        disk_file = stderr;
    }
    fprintf(disk_file, "=== EXAM SUBMISSIONS ===\n\n");
    fflush(disk_file);

    log_event("INFO", "IO", "I/O buffer initialized");
}

void io_buffer_shutdown() {
    io_running = 0;
    sem_post(&g_io_buffer.filled_slots); // wake flusher thread to exit
}

// ─── Producer: called by exam processes ──────────────────
int io_buffer_submit(int pid, int question_id,
                     const char *answer, int is_partial) {
    // Non-blocking try — if buffer full, drop submission
    int rc = sem_trywait(&g_io_buffer.empty_slots);
    if (rc != 0) {
        pthread_mutex_lock(&g_state.lock);
        g_state.dropped_submissions++;
        pthread_mutex_unlock(&g_state.lock);

        char msg[128];
        snprintf(msg, sizeof(msg),
                 "DROP: PID %d Q%d — buffer full!", pid, question_id);
        log_event("ERROR", "IO", msg);
        return -1;
    }

    pthread_mutex_lock(&g_io_buffer.lock);

    Submission *s   = &g_io_buffer.buffer[g_io_buffer.tail];
    s->pid          = pid;
    s->question_id  = question_id;
    s->timestamp    = now_ms();
    s->is_partial   = is_partial;
    strncpy(s->answer, answer ? answer : "EMPTY", sizeof(s->answer) - 1);

    g_io_buffer.tail  = (g_io_buffer.tail + 1) % BUFFER_CAPACITY;
    g_io_buffer.count++;

    // Update shared state for dashboard
    pthread_mutex_lock(&g_state.lock);
    g_state.buffer_count       = g_io_buffer.count;
    g_state.total_submissions++;
    pthread_mutex_unlock(&g_state.lock);

    pthread_mutex_unlock(&g_io_buffer.lock);
    sem_post(&g_io_buffer.filled_slots);

    char msg[128];
    snprintf(msg, sizeof(msg), "PID %d submitted Q%d%s",
             pid, question_id, is_partial ? " (PARTIAL/timeout)" : "");
    log_event("INFO", "IO", msg);

    return 0;
}

// ─── Flush batch to disk ──────────────────────────────────
static int flush_buffer() {
    pthread_mutex_lock(&g_io_buffer.lock);
    int count = g_io_buffer.count;
    pthread_mutex_unlock(&g_io_buffer.lock);

    if (count == 0) return 0;

    int flushed = 0;
    while (flushed < count) {
        // Wait for an item
        if (sem_trywait(&g_io_buffer.filled_slots) != 0) break;

        pthread_mutex_lock(&g_io_buffer.lock);
        Submission s = g_io_buffer.buffer[g_io_buffer.head];
        g_io_buffer.head  = (g_io_buffer.head + 1) % BUFFER_CAPACITY;
        g_io_buffer.count--;

        pthread_mutex_lock(&g_state.lock);
        g_state.buffer_count = g_io_buffer.count;
        pthread_mutex_unlock(&g_state.lock);

        pthread_mutex_unlock(&g_io_buffer.lock);
        sem_post(&g_io_buffer.empty_slots);

        // Write to simulated disk
        fprintf(disk_file, "[%ld ms] PID=%-3d Q=%-2d %s ANSWER=%s\n",
                s.timestamp, s.pid, s.question_id,
                s.is_partial ? "[PARTIAL]" : "        ",
                s.answer);
        flushed++;
    }

    if (flushed > 0) {
        fflush(disk_file);

        pthread_mutex_lock(&g_state.lock);
        g_state.flush_count++;
        pthread_mutex_unlock(&g_state.lock);

        char msg[64];
        snprintf(msg, sizeof(msg), "Flushed %d submissions to disk", flushed);
        log_event("INFO", "IO", msg);
    }

    return flushed;
}

// ─── Demo mode: submission storm ─────────────────────────
static void trigger_submission_storm() {
    log_event("WARN", "IO", "SUBMISSION STORM triggered — 30 simultaneous submissions!");

    pthread_mutex_lock(&g_state.lock);
    int count = g_state.process_count;
    pthread_mutex_unlock(&g_state.lock);

    int storms = count < 30 ? count : 30;
    for (int i = 0; i < storms; i++) {
        char answer[64];
        snprintf(answer, sizeof(answer), "ANS_%d_%d", i, rand() % 100);
        io_buffer_submit(i + 1, rand() % 10 + 1, answer, 0);
    }
}

// ─── I/O flusher thread ───────────────────────────────────
void *io_buffer_thread(void *arg) {
    (void)arg;
    log_event("INFO", "IO", "I/O buffer thread started");

    int storm_triggered = 0;

    while (1) {
        pthread_mutex_lock(&g_state.lock);
        int running = g_state.simulation_running;
        int tick    = g_state.current_tick;
        int count   = g_state.process_count;
        pthread_mutex_unlock(&g_state.lock);

        if (!running) {
            // Final flush before exit
            flush_buffer();
            break;
        }

        // Demo mode: trigger submission storm at tick 30
        if (g_config.demo_mode && tick >= 30 && !storm_triggered && count >= 10) {
            trigger_submission_storm();
            storm_triggered = 1;
        }

        // Simulate random submissions from active processes
        pthread_mutex_lock(&g_state.lock);
        int pid     = g_state.running_pid;
        int prcount = g_state.process_count;
        pthread_mutex_unlock(&g_state.lock);

        if (pid > 0 && prcount > 0) {
            // 30% chance a process submits an answer each tick
            if (rand() % 100 < 30) {
                char answer[64];
                snprintf(answer, sizeof(answer), "ANS_%d", rand() % 1000);
                io_buffer_submit(pid, rand() % 10 + 1, answer, 0);
            }
        }

        // Flush if above threshold or every 15 ticks
        pthread_mutex_lock(&g_io_buffer.lock);
        float fill = (float)g_io_buffer.count / BUFFER_CAPACITY;
        pthread_mutex_unlock(&g_io_buffer.lock);

        if (fill >= FLUSH_THRESHOLD || tick % 15 == 0)
            flush_buffer();

        usleep(TIME_TICK_MS * 1000);
    }

    if (disk_file && disk_file != stderr) fclose(disk_file);
    log_event("INFO", "IO", "I/O buffer thread exiting");
    return NULL;
}