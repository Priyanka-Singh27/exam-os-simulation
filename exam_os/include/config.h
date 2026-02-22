#ifndef CONFIG_H
#define CONFIG_H

#include "shared.h"

// Parse config file + override with CLI args
void config_load_defaults(Config *cfg);
int  config_parse_file(Config *cfg, const char *filepath);
void config_parse_args(Config *cfg, int argc, char *argv[]);
void config_print(Config *cfg);

#endif // CONFIG_H