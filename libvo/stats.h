#ifndef MPLAYER_STATS_H
#define MPLAYER_STATS_H

#include <stdint.h>

void stats_init(void);
void stats_exit(void);

/// CPU usage model
enum CpuUsageType {
    CPU_USAGE_QUANTUM = 1, ///< CPU usage since the last call to cpu_get_usage()
    CPU_USAGE_AVERAGE      ///< CPU usage average'd since program start
};

/// Get CPU frequency
unsigned int get_cpu_frequency(void);

/// Get CPU usage in percent
float get_cpu_usage(enum CpuUsageType type);

#endif /* MPLAYER_STATS_H */
