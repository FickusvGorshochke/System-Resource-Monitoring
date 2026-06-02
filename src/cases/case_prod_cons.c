#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

#define BUF_N 4

static int             g_buf[BUF_N];
static int             g_in, g_out;
static sem_t           g_empty, g_full;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_running = 1;
static uint32_t        g_prod_ms = 20;
static unsigned long   g_produced, g_consumed;

static void on_signal(int s) { (void)s; g_running = 0; }

static void *producer(void *arg)
{
    (void)arg;
    int item = 0;
    while (g_running) {
        sem_wait(&g_empty);
        pthread_mutex_lock(&g_mtx);
        g_buf[g_in] = item++;
        g_in = (g_in + 1) % BUF_N;
        pthread_mutex_unlock(&g_mtx);
        sem_post(&g_full);
        g_produced++;
        usleep(g_prod_ms * 1000);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    while (g_running) {
        sem_wait(&g_full);
        pthread_mutex_lock(&g_mtx);
        (void)g_buf[g_out];
        g_out = (g_out + 1) % BUF_N;
        pthread_mutex_unlock(&g_mtx);
        sem_post(&g_empty);
        g_consumed++;

    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 0;
    if (argc > 2) g_prod_ms = (uint32_t)atoi(argv[2]);

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "case_prod_cons: pid=%d буфер=%d период_произв=%uмс\n",
            (int)getpid(), BUF_N, g_prod_ms);

    sem_init(&g_empty, 0, BUF_N);
    sem_init(&g_full,  0, 0);

    pthread_t tp, tc;
    pthread_create(&tp, NULL, producer, NULL);
    pthread_create(&tc, NULL, consumer, NULL);

    if (seconds > 0) sleep(seconds);
    else while (g_running) sleep(1);
    g_running = 0;

    sem_post(&g_full);

    pthread_join(tp, NULL);
    pthread_join(tc, NULL);

    fprintf(stderr, "case_prod_cons: произведено=%lu потреблено=%lu\n",
            g_produced, g_consumed);
    return 0;
}
