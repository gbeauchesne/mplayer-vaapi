#include "config.h"
#include "stats.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <inttypes.h>

#if CONFIG_LIBGTOP
#include <glibtop/cpu.h>
#include <glibtop/proctime.h>
#include <glibtop/procstate.h>
#endif

// Process statistics
struct proc_stats {
    uint64_t utime;
    uint64_t stime;
    uint64_t cutime;
    uint64_t cstime;
    uint64_t frequency;
    uint64_t cpu_time;
    uint64_t start_time;
    uint64_t current_time;
};

// Get current process stats
static int get_proc_stats(struct proc_stats *pstats);

void stats_init(void)
{
#if CONFIG_LIBGTOP
    glibtop_init();
#endif
}

void stats_exit(void)
{
#if CONFIG_LIBGTOP
    glibtop_close();
#endif
}

// Get CPU frequency
unsigned int get_cpu_frequency(void)
{
    unsigned int freq = 0;
#if defined __linux__
    {
        FILE *proc_file = fopen("/proc/cpuinfo", "r");
        if (proc_file) {
            char line[256];
            char *old_locale = setlocale(LC_NUMERIC, NULL);
            setlocale(LC_NUMERIC, "C");
            while(fgets(line, sizeof(line), proc_file)) {
                float f;
                int len = strlen(line);
                if (len == 0)
                    continue;
                line[len - 1] = 0;
                if (sscanf(line, "cpu MHz : %f", &f) == 1)
                    freq = (unsigned int)f;
            }
            setlocale(LC_NUMERIC, old_locale);
            fclose(proc_file);
        }
    }
#endif
    return freq;
}

// Get CPU usage in percent
static float get_cpu_usage_1(void)
{
    static struct proc_stats prev_stats;
    struct proc_stats curr_stats;
    uint64_t prev_proc_time = 0, curr_proc_time = 0;
    float pcpu = 0.0f;

    if (get_proc_stats(&curr_stats) == 0) {
        prev_proc_time += prev_stats.utime;
        prev_proc_time += prev_stats.stime;
        prev_proc_time += prev_stats.cutime;
        prev_proc_time += prev_stats.cstime;
        curr_proc_time += curr_stats.utime;
        curr_proc_time += curr_stats.stime;
        curr_proc_time += curr_stats.cutime;
        curr_proc_time += curr_stats.cstime;
        if (prev_stats.start_time > 0)
            pcpu = 100.0 * ((float)(curr_proc_time - prev_proc_time) /
                            (float)(curr_stats.cpu_time - prev_stats.cpu_time));
        prev_stats = curr_stats;
    }
    return pcpu;
}

float get_cpu_usage(enum CpuUsageType type)
{
    static float pcpu_total = 0.0;
    static unsigned int n_samples;
    float pcpu;

    pcpu        = get_cpu_usage_1();
    pcpu_total += pcpu / 100.0;
    ++n_samples;

    if (type == CPU_USAGE_AVERAGE)
        pcpu = 100.0 * (pcpu_total / n_samples);
    return pcpu;
}

// For ELF executable, notes are pushed before environment and args
static int find_elf_note(unsigned long match, unsigned long *pval)
{
    unsigned long *ep = (unsigned long *)__environ;
    while (*ep++);
    for (; *ep != 0; ep += 2) {
        if (ep[0] == match) {
            *pval = ep[1];
            return 0;
        }
    }
    return -1;
}

#ifndef AT_CLKTCK
#define AT_CLKTCK 17
#endif

// Get current process stats
int get_proc_stats(struct proc_stats *pstats)
{
    int error = -1;
    char line[256], *str, *end;
    char vc;
    int vi;
    unsigned long vul;
    unsigned long long vull;
    float vf;
#if defined __linux__
    {
        FILE *proc_file = fopen("/proc/self/stat", "r");
        if (proc_file) {
            if (fgets(line, sizeof(line), proc_file)) {
                unsigned long utime, stime, cutime, cstime, start_time;
                str = strrchr(line, ')');
                if (str && sscanf(str + 2,
                                  "%c "
                                  "%d %d %d %d %d "
                                  "%lu %lu %lu %lu %lu %lu %lu "
                                  "%ld %ld %ld %ld %ld %ld "
                                  "%lu %lu ",
                                  &vc,
                                  &vi, &vi, &vi, &vi, &vi, 
                                  &vul, &vul, &vul, &vul, &vul, &utime, &stime,
                                  &cutime, &cstime, &vul, &vul, &vul, &vul,
                                  &start_time, &vul) == 21) {
                    pstats->utime      = utime;
                    pstats->stime      = stime;
                    pstats->cutime     = cutime;
                    pstats->cstime     = cstime;
                    pstats->start_time = start_time;
                    error = 0;
                }
            }
            fclose(proc_file);
        }
        if (error)
            return error;
        error = -1;

        if (find_elf_note(AT_CLKTCK, &vul) == 0) {
            pstats->frequency = vul;
            error = 0;
        }
        if (error)
            return error;
        error = -1;

        proc_file = fopen("/proc/uptime", "r");
        if (proc_file) {
            if (fgets(line, sizeof(line), proc_file)) {
                char *old_locale = setlocale(LC_NUMERIC, NULL);
                setlocale(LC_NUMERIC, "C");
                if (sscanf(line, "%f", &vf) == 1) {
                    pstats->cpu_time = (uint64_t)(vf * (float)pstats->frequency);
                    error = 0;
                }
                setlocale(LC_NUMERIC, old_locale);
            }
            fclose(proc_file);
        }
    }
#elif CONFIG_LIBGTOP
    {
        glibtop_cpu cpu;
        glibtop_proc_time proc_time;
        glibtop_proc_state proc_state;

        glibtop_get_cpu(&cpu);
        glibtop_get_proc_state(&proc_state, getpid());
        pstats->cpu_time   = cpu.xcpu_total[proc_state.processor];

        glibtop_get_proc_time(&proc_time, getpid());
        pstats->utime      = proc_time.utime;
        pstats->stime      = proc_time.stime;
        pstats->cutime     = proc_time.cutime;
        pstats->cstime     = proc_time.cstime;
        pstats->start_time = proc_time.start_time;
        pstats->frequency  = proc_time.frequency;

        error = 0;
    }
#endif
    return error;
}
