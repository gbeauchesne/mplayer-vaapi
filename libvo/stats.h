#ifndef MPLAYER_STATS_H
#define MPLAYER_STATS_H

#include <stdint.h>

void stats_init(void);
void stats_exit(void);

// Get CPU frequency
unsigned int get_cpu_frequency(void);

// Get CPU usage in percent
float get_cpu_usage(void);

#endif /* MPLAYER_STATS_H */
