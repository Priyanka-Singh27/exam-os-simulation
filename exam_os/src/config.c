#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"

void config_load_defaults(Config *cfg) {
    cfg->num_students    = 50;
    cfg->memory_frames   = 64;
    cfg->page_size       = 4;
    cfg->time_quantum    = 5;
    cfg->exam_duration   = 100;
    cfg->sched_algo      = PRIORITY;
    cfg->page_algo       = LRU;
    cfg->buffer_capacity = 256;
    cfg->demo_mode       = 0;
}

int config_parse_file(Config *cfg, const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return 0;  // no config file is fine, defaults are used

    char key[64], val[64];
    while (fscanf(f, "%63s = %63s", key, val) == 2) {
        if (key[0] == '#') continue;  // skip comments

        if      (strcmp(key, "NUM_STUDENTS")     == 0) cfg->num_students    = atoi(val);
        else if (strcmp(key, "MEMORY_FRAMES")    == 0) cfg->memory_frames   = atoi(val);
        else if (strcmp(key, "PAGE_SIZE")        == 0) cfg->page_size       = atoi(val);
        else if (strcmp(key, "TIME_QUANTUM")     == 0) cfg->time_quantum    = atoi(val);
        else if (strcmp(key, "EXAM_DURATION")    == 0) cfg->exam_duration   = atoi(val);
        else if (strcmp(key, "BUFFER_CAPACITY")  == 0) cfg->buffer_capacity = atoi(val);
        else if (strcmp(key, "SCHEDULING_ALGO")  == 0)
            cfg->sched_algo = (strcmp(val, "ROUND_ROBIN") == 0) ? ROUND_ROBIN : PRIORITY;
        else if (strcmp(key, "PAGE_REPLACE")     == 0)
            cfg->page_algo  = (strcmp(val, "FIFO") == 0) ? FIFO : LRU;
    }

    fclose(f);
    return 1;
}

void config_parse_args(Config *cfg, int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "--students") == 0 && i+1 < argc) cfg->num_students  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames")   == 0 && i+1 < argc) cfg->memory_frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--quantum")  == 0 && i+1 < argc) cfg->time_quantum  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc) cfg->exam_duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--algo")     == 0 && i+1 < argc)
            cfg->sched_algo = (strcmp(argv[++i], "RR") == 0) ? ROUND_ROBIN : PRIORITY;
        else if (strcmp(argv[i], "--page")     == 0 && i+1 < argc)
            cfg->page_algo  = (strcmp(argv[++i], "FIFO") == 0) ? FIFO : LRU;
        else if (strcmp(argv[i], "--demo")     == 0) cfg->demo_mode = 1;
    }
}

void config_print(Config *cfg) {
    printf("┌─── Configuration ───────────────────────┐\n");
    printf("│ Students     : %-26d │\n", cfg->num_students);
    printf("│ Memory Frames: %-26d │\n", cfg->memory_frames);
    printf("│ Time Quantum : %-26d │\n", cfg->time_quantum);
    printf("│ Exam Duration: %-26d │\n", cfg->exam_duration);
    printf("│ Scheduling   : %-26s │\n", cfg->sched_algo == PRIORITY ? "PRIORITY" : "ROUND_ROBIN");
    printf("│ Page Replace : %-26s │\n", cfg->page_algo  == LRU      ? "LRU"      : "FIFO");
    printf("│ Demo Mode    : %-26s │\n", cfg->demo_mode  ? "ON" : "OFF");
    printf("└─────────────────────────────────────────┘\n");
}

