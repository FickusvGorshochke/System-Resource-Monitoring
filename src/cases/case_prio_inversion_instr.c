#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/trace.h>

static pthread_mutex_t   g_mtx;
static sem_t             g_low_locked;
static uint32_t          g_low_work_ms = 200;
static uint32_t          g_mid_burn_ms = 3000;
static volatile uint64_t g_dummy;

static uint64_t mono_ns(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static void busy_ms(uint32_t ms)
{
    uint64_t end = mono_ns() + (uint64_t)ms * 1000000ULL;
    volatile uint64_t a = 0;
    while (mono_ns() < end) for (int i = 0; i < 1000; i++) a = a*1103515245ULL+12345ULL;
    g_dummy = a;
}
static void pin_cpu0(void)
{
    int data[3] = { 1, 1, 1 };
    if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, data) == -1)
        perror("ThreadCtl RUNMASK_INHERIT");
}

static void *low_fn(void *a)
{
    (void)a; pin_cpu0();
    pthread_mutex_lock(&g_mtx);
    trace_logf(950, "L locked mutex");
    sem_post(&g_low_locked);
    busy_ms(g_low_work_ms);
    pthread_mutex_unlock(&g_mtx);
    return NULL;
}
static void *mid_fn(void *a)
{
    (void)a; pin_cpu0();
    trace_logf(953, "M burn start %u ms", g_mid_burn_ms);
    busy_ms(g_mid_burn_ms);
    return NULL;
}
static void *high_fn(void *a)
{
    (void)a; pin_cpu0();
    trace_logf(951, "H wants mutex");
    uint64_t t0 = mono_ns();
    pthread_mutex_lock(&g_mtx);
    uint64_t wait_ns = mono_ns() - t0;
    pthread_mutex_unlock(&g_mtx);
    trace_logi(952, (unsigned)(wait_ns / 1000000ULL), 0);
    fprintf(stderr, "  H ждал мьютекс: %.1f мс\n", (double)wait_ns / 1e6);
    return NULL;
}

static pthread_t spawn(int prio, void *(*fn)(void *))
{
    pthread_attr_t at; struct sched_param sp; pthread_t t;
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_RR);
    sp.sched_priority = prio; pthread_attr_setschedparam(&at, &sp);
    if (pthread_create(&t, &at, fn, NULL) != 0) { perror("pthread_create"); exit(1); }
    pthread_attr_destroy(&at);
    return t;
}

int main(int argc, char *argv[])
{
    int use_pi = 0, argi = 1;
    if (argi < argc && strcmp(argv[argi], "-i") == 0) { use_pi = 1; argi++; }
    if (argi < argc) g_low_work_ms = (uint32_t)atoi(argv[argi++]);
    if (argi < argc) g_mid_burn_ms = (uint32_t)atoi(argv[argi++]);

    const int L = 14, M = 16, H = 20;
    pin_cpu0();
    struct sched_param mp = { .sched_priority = 25 };
    pthread_setschedparam(pthread_self(), SCHED_RR, &mp);

    fprintf(stderr, "case_prio_inversion_instr: pid=%d наследование=%s\n",
            (int)getpid(), use_pi ? "ВКЛ" : "выкл");
    trace_logf(949, "prio_inversion start PI=%d", use_pi);

    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma,
        use_pi ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE);
    pthread_mutex_init(&g_mtx, &ma);
    sem_init(&g_low_locked, 0, 0);

    pthread_t tl = spawn(L, low_fn);
    sem_wait(&g_low_locked);
    pthread_t th = spawn(H, high_fn);
    usleep(50000);
    pthread_t tm = spawn(M, mid_fn);

    pthread_join(tl, NULL); pthread_join(th, NULL); pthread_join(tm, NULL);
    fprintf(stderr, "case_prio_inversion_instr: завершение\n");
    return 0;
}
