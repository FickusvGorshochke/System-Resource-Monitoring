/*
 * case_cpu_burner — простейшая CPU-bound нагрузка для проверки sysmond.
 *
 * Один поток выполняет бесконечный busy-loop с арифметикой.
 * Ожидаемое поведение в sysmon_cli snapshot:
 *   - один поток в состоянии RUN или READY (вытесняемое RUNNING)
 *   - CPU% процесса близок к 100% от одного ядра
 *
 * Запуск:  ./case_cpu_burner [seconds]   (по умолч. бесконечно)
 * Выход:   Ctrl+C
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>

static volatile int g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[])
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 0;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "case_cpu_burner: pid=%d, длительность=%s\n",
            (int)getpid(),
            seconds > 0 ? argv[1] : "бесконечно");
    fprintf(stderr, "Запусти параллельно: sysmon_cli snapshot | grep case_cpu\n");

    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    volatile uint64_t acc = 0;
    while (g_running) {
        for (uint64_t i = 0; i < 10000000ULL; i++) {
            acc = acc * 1103515245ULL + 12345ULL;
        }

        if (seconds > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if ((now.tv_sec - start.tv_sec) >= seconds) break;
        }
    }

    fprintf(stderr, "case_cpu_burner: завершение, acc=%llu\n",
            (unsigned long long)acc);
    return 0;
}
