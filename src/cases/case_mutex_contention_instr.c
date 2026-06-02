#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/trace.h>

#define MAX_THREADS 16
#define LOG_EVERY   2000

static volatile int    g_running = 1;
static pthread_mutex_t g_mutex   = PTHREAD_MUTEX_INITIALIZER;

typedef struct { int id; uint64_t iterations; } worker_ctx_t;

static void on_signal(int sig) { (void)sig; g_running = 0; }

static void *worker(void *arg)
{
    worker_ctx_t *ctx = (worker_ctx_t *)arg;
    while (g_running) {
        pthread_mutex_lock(&g_mutex);
        int log = (ctx->iterations % LOG_EVERY == 0);
        if (log) trace_logi(940, (unsigned)ctx->id, (unsigned)ctx->iterations);

        volatile uint64_t acc = 0;
        for (int i = 0; i < 1000; i++) acc = acc * 1103515245ULL + ctx->id;
        ctx->iterations++;

        if (log) trace_logi(941, (unsigned)ctx->id, 0);
        pthread_mutex_unlock(&g_mutex);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int n_threads = (argc > 1) ? atoi(argv[1]) : 4;
    int seconds   = (argc > 2) ? atoi(argv[2]) : 0;
    if (n_threads < 1 || n_threads > MAX_THREADS) {
        fprintf(stderr, "Ошибка: N_threads 1..%d\n", MAX_THREADS);
        return 1;
    }
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "case_mutex_contention_instr: pid=%d threads=%d\n",
            (int)getpid(), n_threads);
    trace_logf(942, "mutex start threads=%d", n_threads);

    pthread_t    tids[MAX_THREADS];
    worker_ctx_t ctxs[MAX_THREADS];
    for (int i = 0; i < n_threads; i++) {
        ctxs[i].id = i + 1; ctxs[i].iterations = 0;
        pthread_create(&tids[i], NULL, worker, &ctxs[i]);
    }

    if (seconds > 0) { sleep(seconds); g_running = 0; }
    else while (g_running) sleep(1);

    uint64_t total = 0;
    for (int i = 0; i < n_threads; i++) { pthread_join(tids[i], NULL); total += ctxs[i].iterations; }
    fprintf(stderr, "case_mutex_contention_instr: всего %llu итераций\n",
            (unsigned long long)total);
    pthread_mutex_destroy(&g_mutex);
    return 0;
}
