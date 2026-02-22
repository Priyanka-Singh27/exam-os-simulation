#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "scheduler.h"
#include "logger.h"

// ─── Ready Queue (min-heap by priority) ──────────────────
static PCB  ready_queue[MAX_STUDENTS];
static int  rq_size = 0;
static pthread_mutex_t rq_lock = PTHREAD_MUTEX_INITIALIZER;

// ─── Round Robin queue index ─────────────────────────────
static int rr_index = 0;

// ─── Heap helpers (higher priority = lower remaining_time) 
static void swap_pcb(int a, int b) {
    PCB tmp = ready_queue[a];
    ready_queue[a] = ready_queue[b];
    ready_queue[b] = tmp;
}

static void heap_push(PCB p) {
    ready_queue[rq_size] = p;
    int i = rq_size++;
    // bubble up
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (ready_queue[parent].remaining_time > ready_queue[i].remaining_time) {
            swap_pcb(parent, i);
            i = parent;
        } else break;
    }
}

static PCB heap_pop() {
    PCB top = ready_queue[0];
    ready_queue[0] = ready_queue[--rq_size];
    // bubble down
    int i = 0;
    while (1) {
        int left  = 2*i+1, right = 2*i+2, smallest = i;
        if (left  < rq_size && ready_queue[left].remaining_time  < ready_queue[smallest].remaining_time) smallest = left;
        if (right < rq_size && ready_queue[right].remaining_time < ready_queue[smallest].remaining_time) smallest = right;
        if (smallest == i) break;
        swap_pcb(i, smallest);
        i = smallest;
    }
    return top;
}

// ─── Public: add process to ready queue ──────────────────
void scheduler_init() {
    rq_size  = 0;
    rr_index = 0;
    log_event("INFO", "SCHEDULER", "Scheduler initialized");
}

void scheduler_add_process(PCB process) {
    pthread_mutex_lock(&rq_lock);
    process.state = READY;

    // Add to global state process list
    pthread_mutex_lock(&g_state.lock);
    g_state.processes[g_state.process_count++] = process;
    pthread_mutex_unlock(&g_state.lock);

    heap_push(process);

    char msg[128];
    snprintf(msg, sizeof(msg), "PID %d added to ready queue (remaining=%d ticks)", 
             process.pid, process.remaining_time);
    log_event("INFO", "SCHEDULER", msg);
    pthread_mutex_unlock(&rq_lock);
}

void scheduler_terminate_process(int pid) {
    pthread_mutex_lock(&g_state.lock);
    for (int i = 0; i < g_state.process_count; i++) {
        if (g_state.processes[i].pid == pid) {
            g_state.processes[i].state = TERMINATED;
            g_state.completed_processes++;
            break;
        }
    }
    pthread_mutex_unlock(&g_state.lock);

    char msg[64];
    snprintf(msg, sizeof(msg), "PID %d terminated", pid);
    log_event("INFO", "SCHEDULER", msg);
}

// ─── Round Robin scheduling ───────────────────────────────
static void run_round_robin() {
    pthread_mutex_lock(&rq_lock);
    if (rq_size == 0) {
        pthread_mutex_unlock(&rq_lock);

        pthread_mutex_lock(&g_state.lock);
        g_state.running_pid    = -1;
        g_state.cpu_utilization = 0.0f;
        pthread_mutex_unlock(&g_state.lock);
        return;
    }

    // Pick next in circular order
    PCB current = ready_queue[rr_index % rq_size];
    pthread_mutex_unlock(&rq_lock);

    // Simulate running for one quantum
    pthread_mutex_lock(&g_state.lock);
    g_state.running_pid     = current.pid;
    g_state.cpu_utilization = 100.0f * rq_size / (rq_size + 1);
    g_state.context_switches++;
    pthread_mutex_unlock(&g_state.lock);

    // Tick down remaining time
    pthread_mutex_lock(&rq_lock);
    if (rq_size > 0) {
        int idx = rr_index % rq_size;
        ready_queue[idx].remaining_time -= g_config.time_quantum;

        if (ready_queue[idx].remaining_time <= 0) {
            // Process finished naturally
            PCB done = ready_queue[idx];
            ready_queue[idx] = ready_queue[--rq_size];
            scheduler_terminate_process(done.pid);

            char msg[64];
            snprintf(msg, sizeof(msg), "PID %d completed exam (RR)", done.pid);
            log_event("INFO", "SCHEDULER", msg);
        } else {
            rr_index = (rr_index + 1) % rq_size;
        }
    }
    pthread_mutex_unlock(&rq_lock);
}

// ─── Priority scheduling ──────────────────────────────────
static void run_priority() {
    pthread_mutex_lock(&rq_lock);
    if (rq_size == 0) {
        pthread_mutex_unlock(&rq_lock);

        pthread_mutex_lock(&g_state.lock);
        g_state.running_pid     = -1;
        g_state.cpu_utilization = 0.0f;
        pthread_mutex_unlock(&g_state.lock);
        return;
    }

    PCB current = heap_pop();
    pthread_mutex_unlock(&rq_lock);

    // Run it
    pthread_mutex_lock(&g_state.lock);
    g_state.running_pid     = current.pid;
    g_state.cpu_utilization = 100.0f * (g_config.num_students - g_state.completed_processes)
                              / g_config.num_students;
    g_state.context_switches++;
    pthread_mutex_unlock(&g_state.lock);

    // Simulate one quantum of work
    usleep(TIME_TICK_MS * 500);

    current.remaining_time -= g_config.time_quantum;

    if (current.remaining_time <= 0) {
        scheduler_terminate_process(current.pid);

        char msg[64];
        snprintf(msg, sizeof(msg), "PID %d completed exam (PRIORITY)", current.pid);
        log_event("INFO", "SCHEDULER", msg);
    } else {
        // Put back in heap with updated time
        pthread_mutex_lock(&rq_lock);
        heap_push(current);
        pthread_mutex_unlock(&rq_lock);
    }
}

// ─── Main scheduler thread ────────────────────────────────
void *scheduler_thread(void *arg) {
    (void)arg;
    log_event("INFO", "SCHEDULER", "Scheduler thread started");

    while (1) {
        pthread_mutex_lock(&g_state.lock);
        int running = g_state.simulation_running;
        int tick    = g_state.current_tick;
        pthread_mutex_unlock(&g_state.lock);

        if (!running) break;

        // Add a new batch of student processes every 10 ticks
        // (simulates students joining exam over time)
        if (tick % 10 == 0) {
            pthread_mutex_lock(&g_state.lock);
            int added = g_state.process_count;
            pthread_mutex_unlock(&g_state.lock);

            if (added < g_config.num_students) {
                int batch = (g_config.num_students - added < 5)
                            ? g_config.num_students - added : 5;
                for (int i = 0; i < batch; i++) {
                    PCB p = {
                        .pid            = added + i + 1,
                        .state          = NEW,
                        .priority       = 1,
                        .total_time     = g_config.exam_duration,
                        .remaining_time = g_config.exam_duration - (rand() % 10),
                        .waiting_time   = 0,
                        .turnaround_time = 0,
                        .pages_used     = 0
                    };
                    scheduler_add_process(p);
                }
            }
        }

        // Run one scheduling decision
        if (g_config.sched_algo == ROUND_ROBIN)
            run_round_robin();
        else
            run_priority();

        usleep(TIME_TICK_MS * 1000);
    }

    log_event("INFO", "SCHEDULER", "Scheduler thread exiting");
    return NULL;
}