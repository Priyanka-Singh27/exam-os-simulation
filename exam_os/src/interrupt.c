#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "interrupt.h"
#include "logger.h"
#include "scheduler.h"
#include "memory.h"
#include "io_buffer.h"

// ─── Interrupt Vector Table ───────────────────────────────
static IVTEntry ivt[MAX_INTERRUPTS];
static int      ivt_size = 0;
static pthread_mutex_t ivt_lock = PTHREAD_MUTEX_INITIALIZER;

// ─── Interrupt queue (raised but not yet handled) ─────────
typedef struct {
    int interrupt_id;
    int pid;
    long timestamp;
} PendingInterrupt;

static PendingInterrupt int_queue[64];
static int int_q_head = 0, int_q_tail = 0, int_q_count = 0;
static pthread_mutex_t int_q_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t           int_ready;

// ─── Timestamp ────────────────────────────────────────────
static long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

// ════════════════════════════════════════════════════════
//  INTERRUPT HANDLERS
// ════════════════════════════════════════════════════════

// Handler 0: Exam timeout — save partial, terminate process
static void handle_exam_timeout(int pid, SystemState *state) {
    char msg[128];
    snprintf(msg, sizeof(msg), "TIMEOUT: PID %d exam expired — saving partial submission", pid);
    log_event("WARN", "INTERRUPT", msg);

    // Save whatever the student had to I/O buffer as partial
    char answer[64];
    snprintf(answer, sizeof(answer), "PARTIAL_PID%d", pid);
    io_buffer_submit(pid, 0, answer, 1);

    // Free memory pages
    memory_free_process(pid - 1);

    // Terminate the process
    scheduler_terminate_process(pid);

    pthread_mutex_lock(&state->lock);
    state->timeouts_fired++;
    pthread_mutex_unlock(&state->lock);
}

// Handler 1: System overload — pause new logins
static void handle_overload(int pid, SystemState *state) {
    (void)pid;
    log_event("WARN", "INTERRUPT", "OVERLOAD: Buffer critical — pausing new submissions");

    pthread_mutex_lock(&state->lock);
    state->overload_signals++;
    pthread_mutex_unlock(&state->lock);

    // Simulate brief pause — back-pressure
    usleep(TIME_TICK_MS * 2000);

    log_event("INFO", "INTERRUPT", "OVERLOAD resolved — resuming normal operation");
}

// Handler 2: Page fault notification
static void handle_page_fault(int pid, SystemState *state) {
    (void)state;
    char msg[64];
    snprintf(msg, sizeof(msg), "PAGE FAULT raised for PID %d", pid);
    log_event("INFO", "INTERRUPT", msg);
    // Actual handling done in memory.c — this just logs it centrally
}

// Handler 3: Submission complete
static void handle_submit_complete(int pid, SystemState *state) {
    (void)state;
    char msg[64];
    snprintf(msg, sizeof(msg), "Submission complete for PID %d", pid);
    log_event("INFO", "INTERRUPT", msg);
}

// ─── Register handler in IVT ──────────────────────────────
static void ivt_register(int id, const char *name, handler_fn handler) {
    pthread_mutex_lock(&ivt_lock);
    if (ivt_size < MAX_INTERRUPTS) {
        ivt[ivt_size].interrupt_id = id;
        strncpy(ivt[ivt_size].name, name, sizeof(ivt[ivt_size].name) - 1);
        ivt[ivt_size].handler = handler;
        ivt_size++;
    }
    pthread_mutex_unlock(&ivt_lock);
}

// ─── Init: register all handlers ─────────────────────────
void interrupt_init() {
    sem_init(&int_ready, 0, 0);

    ivt_register(INT_EXAM_TIMEOUT,    "EXAM_TIMEOUT",    handle_exam_timeout);
    ivt_register(INT_OVERLOAD,        "OVERLOAD",        handle_overload);
    ivt_register(INT_PAGE_FAULT,      "PAGE_FAULT",      handle_page_fault);
    ivt_register(INT_SUBMIT_COMPLETE, "SUBMIT_COMPLETE", handle_submit_complete);

    log_event("INFO", "INTERRUPT", "Interrupt vector table initialized (4 handlers)");
}

// ─── Raise an interrupt (thread-safe, non-blocking) ──────
void interrupt_raise(int interrupt_id, int pid) {
    pthread_mutex_lock(&int_q_lock);

    if (int_q_count < 64) {
        PendingInterrupt *pi = &int_queue[int_q_tail];
        pi->interrupt_id     = interrupt_id;
        pi->pid              = pid;
        pi->timestamp        = now_ms();

        int_q_tail  = (int_q_tail + 1) % 64;
        int_q_count++;
        sem_post(&int_ready);
    }

    pthread_mutex_unlock(&int_q_lock);
}

// ─── Dispatch: look up IVT and call handler ───────────────
static void dispatch(PendingInterrupt *pi) {
    pthread_mutex_lock(&ivt_lock);

    for (int i = 0; i < ivt_size; i++) {
        if (ivt[i].interrupt_id == pi->interrupt_id) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Dispatching INT_%d (%s) for PID %d at %ldms",
                     pi->interrupt_id, ivt[i].name, pi->pid, pi->timestamp);
            log_event("INFO", "INTERRUPT", msg);

            pthread_mutex_unlock(&ivt_lock);
            ivt[i].handler(pi->pid, &g_state);
            return;
        }
    }

    pthread_mutex_unlock(&ivt_lock);
    log_event("WARN", "INTERRUPT", "Unknown interrupt ID received");
}

// ─── Check for overload condition ─────────────────────────
static void check_overload() {
    pthread_mutex_lock(&g_io_buffer.lock);
    float fill = (float)g_io_buffer.count / BUFFER_CAPACITY;
    pthread_mutex_unlock(&g_io_buffer.lock);

    if (fill >= 0.95f) {
        interrupt_raise(INT_OVERLOAD, -1);
    }
}

// ─── Check for process timeouts ───────────────────────────
static void check_timeouts() {
    pthread_mutex_lock(&g_state.lock);

    for (int i = 0; i < g_state.process_count; i++) {
        PCB *p = &g_state.processes[i];
        if (p->state == RUNNING || p->state == READY) {
            p->remaining_time--;
            if (p->remaining_time <= 0) {
                // Raise timeout interrupt — handled asynchronously
                int pid = p->pid;
                p->state = TERMINATED;
                pthread_mutex_unlock(&g_state.lock);
                interrupt_raise(INT_EXAM_TIMEOUT, pid);
                pthread_mutex_lock(&g_state.lock);
            }
        }
    }

    pthread_mutex_unlock(&g_state.lock);
}

// ─── Interrupt thread: monitors system + dispatches ───────
void *interrupt_thread(void *arg) {
    (void)arg;
    log_event("INFO", "INTERRUPT", "Interrupt handler thread started");

    int tick_counter = 0;

    while (1) {
        pthread_mutex_lock(&g_state.lock);
        int running = g_state.simulation_running;
        pthread_mutex_unlock(&g_state.lock);

        if (!running) break;

        // Check system conditions every tick
        check_timeouts();
        check_overload();

        // Dispatch any pending interrupts
        while (1) {
            if (sem_trywait(&int_ready) != 0) break;

            pthread_mutex_lock(&int_q_lock);
            if (int_q_count == 0) {
                pthread_mutex_unlock(&int_q_lock);
                break;
            }
            PendingInterrupt pi = int_queue[int_q_head];
            int_q_head  = (int_q_head + 1) % 64;
            int_q_count--;
            pthread_mutex_unlock(&int_q_lock);

            dispatch(&pi);
        }

        tick_counter++;
        usleep(TIME_TICK_MS * 1000);
    }

    log_event("INFO", "INTERRUPT", "Interrupt thread exiting");
    return NULL;
}