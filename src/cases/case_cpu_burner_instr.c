#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/trace.h>

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

int main(int argc, char *argv[])
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 0;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "case_cpu_burner_instr: pid=%d длительность=%s\n",
            (int)getpid(), seconds > 0 ? argv[1] : "бесконечно");

    trace_logf(920, "cpu_burner start dur=%d", seconds);

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    volatile uint64_t acc = 0;
    uint32_t iter = 0;
    while (g_running) {
        for (uint64_t i = 0; i < 10000000ULL; i++)
            acc = acc * 1103515245ULL + 12345ULL;

        trace_logi(921, iter++, (unsigned)acc);

        if (seconds > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((now.tv_sec - start.tv_sec) >= seconds) break;
        }
    }

    fprintf(stderr, "case_cpu_burner_instr: завершение, итераций=%u\n", iter);
    return 0;
}
