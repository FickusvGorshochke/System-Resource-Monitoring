/*
 * case_mutex_contention — конкуренция за один мьютекс.
 *
 * N потоков в цикле захватывают один и тот же мьютекс, выполняют
 * короткую работу, освобождают.
 *
 * Ожидаемое поведение в sysmon_cli snapshot:
 *   - один поток в состоянии RUNNING (владелец мьютекса)
 *   - остальные N-1 потоков в состоянии BLOCKED с blocked_type MUTEX
 *   - частая смена владельца видна как переходы между снимками
 *
 * Запуск:  ./case_mutex_contention [N_threads] [seconds]
 *          (по умолч. N=4, seconds=бесконечно)
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define MAX_THREADS 16

static volatile int    g_running = 1;
static pthread_mutex_t g_mutex   = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int       id;
    uint64_t  iterations;
} worker_ctx_t;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void *worker(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;

    while (g_running) {
        pthread_mutex_lock(&g_mutex);

        /* Короткая работа в критической секции */
        volatile uint64_t acc = 0;
        for (int i = 0; i < 1000; i++) {
            acc = acc * 1103515245ULL + ctx->id;
        }
        ctx->iterations++;

        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int n_threads = (argc > 1) ? atoi(argv[1]) : 4;
    int seconds   = (argc > 2) ? atoi(argv[2]) : 0;

    if (n_threads < 1 || n_threads > MAX_THREADS) {
        fprintf(stderr, "Ошибка: N_threads должно быть 1..%d\n", MAX_THREADS);
        return 1;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "case_mutex_contention: pid=%d threads=%d\n",
            (int)getpid(), n_threads);

    pthread_t   tids[MAX_THREADS];
    worker_ctx_t ctxs[MAX_THREADS];

    for (int i = 0; i < n_threads; i++) {
        ctxs[i].id         = i + 1;
        ctxs[i].iterations = 0;
        pthread_create(&tids[i], NULL, worker, &ctxs[i]);
    }

    if (seconds > 0) {
        sleep(seconds);
        g_running = 0;
    } else {
        while (g_running) sleep(1);
    }

    for (int i = 0; i < n_threads; i++) {
        pthread_join(tids[i], NULL);
    }

    uint64_t total = 0;
    for (int i = 0; i < n_threads; i++) {
        fprintf(stderr, "  поток %d: %llu итераций\n",
                ctxs[i].id, (unsigned long long)ctxs[i].iterations);
        total += ctxs[i].iterations;
    }
    fprintf(stderr, "case_mutex_contention: всего %llu итераций\n",
            (unsigned long long)total);
    pthread_mutex_destroy(&g_mutex);
    return 0;
}
