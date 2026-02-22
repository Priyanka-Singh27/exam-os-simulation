#ifndef SHARED_H
#define SHARED_H

#include <pthread.h>
#include <semaphore.h>
#include <time.h>

// ─── Constants ───────────────────────────────────────────
#define MAX_STUDENTS     200
#define MAX_FRAMES       256
#define MAX_PAGES        64
#define BUFFER_CAPACITY  256
#define MAX_LOG_QUEUE    512
#define MAX_INTERRUPTS   8
#define TIME_TICK_MS     100

// ─── Enums ───────────────────────────────────────────────
typedef enum {
    NEW, READY, RUNNING, WAITING, TERMINATED
} ProcessState;

typedef enum {
    ROUND_ROBIN, PRIORITY
} SchedAlgo;

typedef enum {
    LRU, FIFO
} PageAlgo;

// ─── Process Control Block ───────────────────────────────
typedef struct {
    int          pid;
    ProcessState state;
    int          priority;        // higher = more urgent
    int          total_time;      // total exam duration (ticks)
    int          remaining_time;  // ticks left
    int          waiting_time;
    int          turnaround_time;
    int          pages_used;
} PCB;

// ─── Page Table Entry ────────────────────────────────────
typedef struct {
    int  virtual_page;
    int  frame_number;   // -1 if not in memory
    int  valid;
    int  dirty;
    long last_accessed;  // timestamp for LRU
    int  load_order;     // for FIFO
} PageTableEntry;

// ─── Submission (I/O Buffer item) ────────────────────────
typedef struct {
    int  pid;
    int  question_id;
    char answer[128];
    long timestamp;
    int  is_partial;     // 1 if from timeout interrupt
} Submission;

// ─── Log Entry ───────────────────────────────────────────
typedef struct {
    long timestamp_ns;
    char level[8];       // INFO / WARN / ERROR
    char subsystem[16];  // SCHEDULER / MEMORY / IO / INTERRUPT
    char message[256];
} LogEntry;

// ─── Config ──────────────────────────────────────────────
typedef struct {
    int       num_students;
    int       memory_frames;
    int       page_size;
    int       time_quantum;
    int       exam_duration;
    SchedAlgo sched_algo;
    PageAlgo  page_algo;
    int       buffer_capacity;
    int       demo_mode;
} Config;

// ─── System State (shared across all modules) ────────────
typedef struct {
    // CPU
    int   running_pid;
    float cpu_utilization;
    int   context_switches;
    int   completed_processes;

    // Memory
    int   page_faults;
    int   page_hits;
    int   frames_used;

    // I/O Buffer
    int   buffer_count;
    int   total_submissions;
    int   dropped_submissions;
    int   flush_count;

    // Interrupts
    int   timeouts_fired;
    int   overload_signals;

    // Processes
    PCB   processes[MAX_STUDENTS];
    int   process_count;

    // Simulation control
    int   simulation_running;
    int   current_tick;

    // Recent log lines for dashboard
    char  recent_logs[3][256];
    int   log_index;

    pthread_mutex_t lock;
} SystemState;

// ─── I/O Buffer ──────────────────────────────────────────
typedef struct {
    Submission      buffer[BUFFER_CAPACITY];
    int             head, tail, count;
    sem_t           empty_slots;
    sem_t           filled_slots;
    pthread_mutex_t lock;
} IOBuffer;

// ─── Interrupt Vector Table Entry ────────────────────────
typedef void (*handler_fn)(int pid, SystemState *state);

typedef struct {
    int        interrupt_id;
    char       name[32];
    handler_fn handler;
} IVTEntry;

// ─── Global instances (defined in main.c) ────────────────
extern SystemState g_state;
extern IOBuffer    g_io_buffer;
extern Config      g_config;

#endif // SHARED_H