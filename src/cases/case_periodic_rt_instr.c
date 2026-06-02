#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/trace.h>

#define EV_START  910
#define EV_WORK   911
#define EV_SLEEP  912

static volatile int g_running = 1;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void busy_for_ns(uint64_t ns)
{
    uint64_t start = mono_ns();
    volatile uint64_t acc = 0;
    while (mono_ns() - start < ns) {
        for (int i = 0; i < 1000; i++) acc = acc * 1103515245ULL + 12345ULL;
    }
    (void)acc;
}

int main(int argc, char *argv[])
{
    uint32_t period_ms = (argc > 1) ? (uint32_t)atoi(argv[1]) : 50;
    uint32_t work_ms   = (argc > 2) ? (uint32_t)atoi(argv[2]) : 5;
    int      seconds   = (argc > 3) ? atoi(argv[3])           : 0;

    if (work_ms >= period_ms) {
        fprintf(stderr, "Ошибка: work_ms должно быть меньше period_ms\n");
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "case_periodic_rt_instr: pid=%d period=%u мс work=%u мс\n",
            (int)getpid(), period_ms, work_ms);

    trace_logf(EV_START, "periodic start period=%u work=%u", period_ms, work_ms);

    uint64_t period_ns = (uint64_t)period_ms * 1000000ULL;
    uint64_t work_ns   = (uint64_t)work_ms   * 1000000ULL;
    uint64_t deadline  = mono_ns() + period_ns;
    uint64_t start_ns  = mono_ns();
    uint32_t cycles    = 0;

    while (g_running) {
        trace_logi(EV_WORK, cycles, work_ms);
        busy_for_ns(work_ns);

        uint64_t now = mono_ns();
        if (now < deadline) {
            uint64_t sleep_ns = deadline - now;
            struct timespec ts = {
                .tv_sec  = (time_t)(sleep_ns / 1000000000ULL),
                .tv_nsec = (long)  (sleep_ns % 1000000000ULL),
            };
            trace_logi(EV_SLEEP, cycles, 0);
            nanosleep(&ts, NULL);
        }
        deadline += period_ns;
        cycles++;

        if (seconds > 0 && (mono_ns() - start_ns) / 1000000000ULL >= (uint64_t)seconds)
            break;
    }

    fprintf(stderr, "case_periodic_rt_instr: завершение, циклов=%u\n", cycles);
    return 0;
}
