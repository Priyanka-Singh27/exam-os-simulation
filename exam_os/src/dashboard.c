#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "dashboard.h"

#define REFRESH_MS 500

static int dash_running = 1;
static long start_time  = 0;

// ─── Timestamp ────────────────────────────────────────────
static long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void format_elapsed(char *buf, int size) {
    long elapsed = (now_ms() - start_time) / 1000;
    int  h = elapsed / 3600;
    int  m = (elapsed % 3600) / 60;
    int  s = elapsed % 60;
    snprintf(buf, size, "%02d:%02d:%02d", h, m, s);
}

// ─── Draw a progress bar ──────────────────────────────────
static void draw_bar(WINDOW *win, int y, int x, int width,
                     float pct, int color_pair) {
    int filled = (int)(pct / 100.0f * width);
    if (filled > width) filled = width;

    wattron(win, COLOR_PAIR(color_pair));
    for (int i = 0; i < filled; i++)
        mvwaddch(win, y, x + i, ACS_CKBOARD);
    wattroff(win, COLOR_PAIR(color_pair));

    // Empty part
    for (int i = filled; i < width; i++)
        mvwaddch(win, y, x + i, ACS_BULLET);
}

// ─── Draw a box with title ────────────────────────────────
static void draw_box(WINDOW *win, const char *title) {
    box(win, 0, 0);
    int w = getmaxx(win);
    wattron(win, A_BOLD | COLOR_PAIR(6));
    mvwprintw(win, 0, (w - strlen(title)) / 2, " %s ", title);
    wattroff(win, A_BOLD | COLOR_PAIR(6));
}

void dashboard_init() {
    start_time = now_ms();
}

void dashboard_shutdown() {
    dash_running = 0;
}

void *dashboard_thread(void *arg) {
    (void)arg;

    // ─── ncurses setup ────────────────────────────────────
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);

    if (!has_colors()) {
        endwin();
        fprintf(stderr, "Terminal does not support colors\n");
        return NULL;
    }

    start_color();
    use_default_colors();

    // Color pairs
    init_pair(1, COLOR_GREEN,   -1);  // CPU bar
    init_pair(2, COLOR_CYAN,    -1);  // Memory bar
    init_pair(3, COLOR_YELLOW,  -1);  // I/O bar
    init_pair(4, COLOR_RED,     -1);  // Warnings / errors
    init_pair(5, COLOR_MAGENTA, -1);  // Interrupts
    init_pair(6, COLOR_WHITE,   -1);  // Box titles
    init_pair(7, COLOR_BLUE,    -1);  // Header

    int max_y __attribute__((unused)), max_x;
    getmaxyx(stdscr, max_y, max_x);

    // ─── Create windows ───────────────────────────────────
    // Header
    WINDOW *w_header  = newwin(3,  max_x,          0, 0);
    // CPU panel
    WINDOW *w_cpu     = newwin(7,  max_x / 2,      3, 0);
    // Memory panel
    WINDOW *w_mem     = newwin(7,  max_x / 2,      3, max_x / 2);
    // IO + Interrupt panel
    WINDOW *w_io      = newwin(7,  max_x / 2,     10, 0);
    WINDOW *w_int     = newwin(7,  max_x / 2,     10, max_x / 2);
    // Process list
    WINDOW *w_procs   = newwin(8,  max_x,          17, 0);
    // Log feed
    WINDOW *w_logs    = newwin(6,  max_x,          25, 0);

    while (dash_running) {
        // Check for 'q' to quit
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            pthread_mutex_lock(&g_state.lock);
            g_state.simulation_running = 0;
            pthread_mutex_unlock(&g_state.lock);
            break;
        }

        // Snapshot state (minimize lock time)
        pthread_mutex_lock(&g_state.lock);
        int   running_pid    = g_state.running_pid;
        float cpu_util       = g_state.cpu_utilization;
        int   ctx_switches   = g_state.context_switches;
        int   completed      = g_state.completed_processes;
        int   page_faults    = g_state.page_faults;
        int   page_hits      = g_state.page_hits;
        int   frames_used    = g_state.frames_used;
        int   buf_count      = g_state.buffer_count;
        int   total_subs     = g_state.total_submissions;
        int   dropped_subs   = g_state.dropped_submissions;
        int   flush_count    = g_state.flush_count;
        int   timeouts       = g_state.timeouts_fired;
        int   overloads      = g_state.overload_signals;
        int   proc_count     = g_state.process_count;
        int   tick           = g_state.current_tick;
        char  logs[3][256];
        for (int i = 0; i < 3; i++)
            strncpy(logs[i], g_state.recent_logs[i], 255);
        PCB   procs[MAX_STUDENTS];
        int   snap_count = proc_count < MAX_STUDENTS ? proc_count : MAX_STUDENTS;
        memcpy(procs, g_state.processes, sizeof(PCB) * snap_count);
        pthread_mutex_unlock(&g_state.lock);

        char elapsed[16];
        format_elapsed(elapsed, sizeof(elapsed));

        int  total_pages = page_faults + page_hits;
        float hit_rate   = total_pages > 0
                           ? (float)page_hits / total_pages * 100.0f : 0.0f;
        float mem_pct    = (float)frames_used / g_config.memory_frames * 100.0f;
        float buf_pct    = (float)buf_count   / BUFFER_CAPACITY        * 100.0f;

        // ── HEADER ─────────────────────────────────────────
        werase(w_header);
        wattron(w_header, A_BOLD | COLOR_PAIR(7));
        mvwprintw(w_header, 1, 2,
                  "  EXAM OS SIMULATION  |  Tick: %-4d  |  Time: %s  |  "
                  "Press 'q' to quit  |  Mode: %s",
                  tick, elapsed,
                  g_config.sched_algo == PRIORITY ? "PRIORITY" : "ROUND_ROBIN");
        wattroff(w_header, A_BOLD | COLOR_PAIR(7));
        box(w_header, 0, 0);
        wrefresh(w_header);

        // ── CPU PANEL ──────────────────────────────────────
        werase(w_cpu);
        draw_box(w_cpu, " CPU SCHEDULER ");
        int bar_w = max_x / 2 - 18;

        mvwprintw(w_cpu, 2, 2, "Utilization:");
        draw_bar(w_cpu, 2, 15, bar_w, cpu_util, 1);
        mvwprintw(w_cpu, 2, 15 + bar_w + 1, "%5.1f%%", cpu_util);

        mvwprintw(w_cpu, 3, 2, "Running PID : ");
        wattron(w_cpu, COLOR_PAIR(1) | A_BOLD);
        wprintw(w_cpu, "%d", running_pid > 0 ? running_pid : 0);
        wattroff(w_cpu, COLOR_PAIR(1) | A_BOLD);

        mvwprintw(w_cpu, 4, 2, "Ctx Switches: %d", ctx_switches);
        mvwprintw(w_cpu, 5, 2, "Completed   : %d / %d",
                  completed, g_config.num_students);
        wrefresh(w_cpu);

        // ── MEMORY PANEL ───────────────────────────────────
        werase(w_mem);
        draw_box(w_mem, " MEMORY PAGING ");

        mvwprintw(w_mem, 2, 2, "Usage  :");
        draw_bar(w_mem, 2, 11, bar_w, mem_pct,
                 mem_pct > 85.0f ? 4 : 2);
        mvwprintw(w_mem, 2, 11 + bar_w + 1, "%5.1f%%", mem_pct);

        mvwprintw(w_mem, 3, 2, "Frames : %d / %d",
                  frames_used, g_config.memory_frames);
        mvwprintw(w_mem, 4, 2, "Faults : ");
        wattron(w_mem, COLOR_PAIR(4));
        wprintw(w_mem, "%d", page_faults);
        wattroff(w_mem, COLOR_PAIR(4));

        mvwprintw(w_mem, 5, 2, "Hit Rate: ");
        wattron(w_mem, COLOR_PAIR(1));
        wprintw(w_mem, "%.1f%%  [%s]", hit_rate,
                g_config.page_algo == LRU ? "LRU" : "FIFO");
        wattroff(w_mem, COLOR_PAIR(1));
        wrefresh(w_mem);

        // ── I/O PANEL ──────────────────────────────────────
        werase(w_io);
        draw_box(w_io, " I/O BUFFER ");

        mvwprintw(w_io, 2, 2, "Buffer :");
        draw_bar(w_io, 2, 11, bar_w, buf_pct,
                 buf_pct > 80.0f ? 4 : 3);
        mvwprintw(w_io, 2, 11 + bar_w + 1, "%5.1f%%", buf_pct);

        mvwprintw(w_io, 3, 2, "Queued  : %d / %d",
                  buf_count, BUFFER_CAPACITY);
        mvwprintw(w_io, 4, 2, "Total   : %d submitted", total_subs);
        mvwprintw(w_io, 5, 2, "Dropped : ");
        wattron(w_io, dropped_subs > 0 ? COLOR_PAIR(4) : COLOR_PAIR(1));
        wprintw(w_io, "%d  |  Flushes: %d",
                dropped_subs, flush_count);
        wattroff(w_io, COLOR_PAIR(4));
        wrefresh(w_io);

        // ── INTERRUPT PANEL ────────────────────────────────
        werase(w_int);
        draw_box(w_int, " INTERRUPTS ");
        mvwprintw(w_int, 2, 2, "Timeouts fired : ");
        wattron(w_int, COLOR_PAIR(4) | A_BOLD);
        wprintw(w_int, "%d", timeouts);
        wattroff(w_int, COLOR_PAIR(4) | A_BOLD);

        mvwprintw(w_int, 3, 2, "Overload signals: ");
        wattron(w_int, COLOR_PAIR(5));
        wprintw(w_int, "%d", overloads);
        wattroff(w_int, COLOR_PAIR(5));

        mvwprintw(w_int, 4, 2, "IVT entries    : 4");
        mvwprintw(w_int, 5, 2, "INT_0 TIMEOUT  INT_1 OVERLOAD");
        wrefresh(w_int);

        // ── PROCESS LIST ───────────────────────────────────
        werase(w_procs);
        draw_box(w_procs, " ACTIVE PROCESSES ");
        mvwprintw(w_procs, 1, 2,
                  "%-6s %-10s %-8s %-8s",
                  "PID", "STATE", "REMAIN", "PRIORITY");

        const char *state_names[] = {
            "NEW", "READY", "RUNNING", "WAITING", "TERMINATED"
        };
        int shown = 0;
        for (int i = 0; i < snap_count && shown < 5; i++) {
            PCB *p = &procs[i];
            if (p->state == TERMINATED) continue;

            int pair = (p->pid == running_pid) ? 1 : 6;
            wattron(w_procs, COLOR_PAIR(pair));
            mvwprintw(w_procs, 2 + shown, 2,
                      "%-6d %-10s %-8d %-8d",
                      p->pid,
                      state_names[p->state],
                      p->remaining_time,
                      p->priority);
            wattroff(w_procs, COLOR_PAIR(pair));
            shown++;
        }
        if (proc_count > 5)
            mvwprintw(w_procs, 7, 2,
                      "... and %d more processes", proc_count - 5);
        wrefresh(w_procs);

        // ── LOG FEED ───────────────────────────────────────
        werase(w_logs);
        draw_box(w_logs, " RECENT EVENTS ");
        for (int i = 0; i < 3; i++) {
            int idx = (g_state.log_index - 3 + i + MAX_LOG_QUEUE) % 3;
            int pair = (strstr(logs[idx], "ERROR") || strstr(logs[idx], "TIMEOUT"))
                       ? 4
                       : (strstr(logs[idx], "WARN") ? 3 : 6);
            wattron(w_logs, COLOR_PAIR(pair));
            mvwprintw(w_logs, i + 1, 2, "%-*.*s",
                      max_x - 4, max_x - 4, logs[idx]);
            wattroff(w_logs, COLOR_PAIR(pair));
        }
        wrefresh(w_logs);

        usleep(REFRESH_MS * 1000);
    }

    // Clean up ncurses
    delwin(w_header);
    delwin(w_cpu);
    delwin(w_mem);
    delwin(w_io);
    delwin(w_int);
    delwin(w_procs);
    delwin(w_logs);
    endwin();

    return NULL;
}