#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "memory.h"
#include "logger.h"

// ─── Physical frame pool ──────────────────────────────────
typedef struct {
    int  pid;           // which process owns this frame (-1 = free)
    int  virtual_page;
    int  load_order;    // for FIFO
    long last_accessed; // for LRU
} Frame;

static Frame    frame_pool[MAX_FRAMES];
static int      total_frames;
static int      fifo_counter = 0;
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;

// ─── Per-process page tables ──────────────────────────────
static PageTableEntry page_tables[MAX_STUDENTS][MAX_PAGES];

// ─── Timestamp helper ─────────────────────────────────────
static long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void memory_init() {
    total_frames = g_config.memory_frames;
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;

    for (int i = 0; i < total_frames; i++) {
        frame_pool[i].pid          = -1;
        frame_pool[i].virtual_page = -1;
        frame_pool[i].load_order   =  0;
        frame_pool[i].last_accessed = 0;
    }

    for (int i = 0; i < MAX_STUDENTS; i++)
        for (int j = 0; j < MAX_PAGES; j++) {
            page_tables[i][j].virtual_page  = j;
            page_tables[i][j].frame_number  = -1;
            page_tables[i][j].valid         =  0;
            page_tables[i][j].dirty         =  0;
            page_tables[i][j].last_accessed =  0;
            page_tables[i][j].load_order    =  0;
        }

    log_event("INFO", "MEMORY", "Memory subsystem initialized");
}

// ─── Find a free frame ────────────────────────────────────
static int find_free_frame() {
    for (int i = 0; i < total_frames; i++)
        if (frame_pool[i].pid == -1) return i;
    return -1;
}

// ─── FIFO eviction ────────────────────────────────────────
static int evict_fifo() {
    int oldest_frame = 0;
    int oldest_order = frame_pool[0].load_order;

    for (int i = 1; i < total_frames; i++) {
        if (frame_pool[i].load_order < oldest_order) {
            oldest_order = frame_pool[i].load_order;
            oldest_frame = i;
        }
    }
    return oldest_frame;
}

// ─── LRU eviction ─────────────────────────────────────────
static int evict_lru() {
    int lru_frame = 0;
    long lru_time = frame_pool[0].last_accessed;

    for (int i = 1; i < total_frames; i++) {
        if (frame_pool[i].last_accessed < lru_time) {
            lru_time  = frame_pool[i].last_accessed;
            lru_frame = i;
        }
    }
    return lru_frame;
}

// ─── Load a page into a frame ─────────────────────────────
static void load_page(int pid, int virtual_page, int frame) {
    // Invalidate previous owner's page table entry
    int prev_pid  = frame_pool[frame].pid;
    int prev_page = frame_pool[frame].virtual_page;

    if (prev_pid >= 0 && prev_pid < MAX_STUDENTS && prev_page >= 0) {
        page_tables[prev_pid][prev_page].valid        = 0;
        page_tables[prev_pid][prev_page].frame_number = -1;

        if (page_tables[prev_pid][prev_page].dirty) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Dirty eviction: PID %d page %d → disk write",
                     prev_pid, prev_page);
            log_event("WARN", "MEMORY", msg);
            page_tables[prev_pid][prev_page].dirty = 0;
        }
    }

    // Load new page
    frame_pool[frame].pid           = pid;
    frame_pool[frame].virtual_page  = virtual_page;
    frame_pool[frame].load_order    = fifo_counter++;
    frame_pool[frame].last_accessed = now_ms();

    page_tables[pid][virtual_page].frame_number  = frame;
    page_tables[pid][virtual_page].valid         = 1;
    page_tables[pid][virtual_page].last_accessed = now_ms();
    page_tables[pid][virtual_page].load_order    = frame_pool[frame].load_order;

    // Simulate disk → memory load delay
    usleep(500);
}

// ─── Core memory access (called per tick per running process)
int memory_access(int pid, int virtual_page) {
    if (pid < 0 || pid >= MAX_STUDENTS) return -1;
    if (virtual_page < 0 || virtual_page >= MAX_PAGES) return -1;

    pthread_mutex_lock(&mem_lock);

    PageTableEntry *entry = &page_tables[pid][virtual_page];

    if (entry->valid) {
        // PAGE HIT
        entry->last_accessed = now_ms();
        frame_pool[entry->frame_number].last_accessed = now_ms();

        pthread_mutex_lock(&g_state.lock);
        g_state.page_hits++;
        pthread_mutex_unlock(&g_state.lock);

        pthread_mutex_unlock(&mem_lock);
        return entry->frame_number;
    }

    // PAGE FAULT
    pthread_mutex_lock(&g_state.lock);
    g_state.page_faults++;
    pthread_mutex_unlock(&g_state.lock);

    char msg[128];
    snprintf(msg, sizeof(msg), "Page fault: PID %d page %d", pid, virtual_page);
    log_event("WARN", "MEMORY", msg);

    // Find or evict a frame
    int frame = find_free_frame();
    if (frame == -1) {
        frame = (g_config.page_algo == LRU) ? evict_lru() : evict_fifo();

        snprintf(msg, sizeof(msg), "Evicting frame %d (%s)",
                 frame, g_config.page_algo == LRU ? "LRU" : "FIFO");
        log_event("INFO", "MEMORY", msg);
    }

    load_page(pid, virtual_page, frame);

    // Update frames_used in shared state
    pthread_mutex_lock(&g_state.lock);
    int used = 0;
    for (int i = 0; i < total_frames; i++)
        if (frame_pool[i].pid != -1) used++;
    g_state.frames_used = used;
    pthread_mutex_unlock(&g_state.lock);

    pthread_mutex_unlock(&mem_lock);
    return frame;
}

// ─── Free all frames owned by a process ──────────────────
void memory_free_process(int pid) {
    pthread_mutex_lock(&mem_lock);

    for (int i = 0; i < total_frames; i++) {
        if (frame_pool[i].pid == pid) {
            int vp = frame_pool[i].virtual_page;
            page_tables[pid][vp].valid        = 0;
            page_tables[pid][vp].frame_number = -1;
            frame_pool[i].pid                 = -1;
            frame_pool[i].virtual_page        = -1;
        }
    }

    pthread_mutex_lock(&g_state.lock);
    int used = 0;
    for (int i = 0; i < total_frames; i++)
        if (frame_pool[i].pid != -1) used++;
    g_state.frames_used = used;
    pthread_mutex_unlock(&g_state.lock);

    pthread_mutex_unlock(&mem_lock);

    char msg[64];
    snprintf(msg, sizeof(msg), "Freed all frames for PID %d", pid);
    log_event("INFO", "MEMORY", msg);
}

// ─── Memory thread ────────────────────────────────────────
// Simulates memory accesses for the currently running process
void *memory_thread(void *arg) {
    (void)arg;
    log_event("INFO", "MEMORY", "Memory thread started");

    while (1) {
        pthread_mutex_lock(&g_state.lock);
        int running  = g_state.simulation_running;
        int curr_pid = g_state.running_pid;
        pthread_mutex_unlock(&g_state.lock);

        if (!running) break;

        if (curr_pid > 0) {
            // Simulate 1-3 random page accesses per tick
            int accesses = 1 + rand() % 3;
            for (int i = 0; i < accesses; i++) {
                int vpage = rand() % 8; // working set of 8 pages per process
                memory_access(curr_pid - 1, vpage);
            }
        }

        usleep(TIME_TICK_MS * 1000);
    }

    log_event("INFO", "MEMORY", "Memory thread exiting");
    return NULL;
}