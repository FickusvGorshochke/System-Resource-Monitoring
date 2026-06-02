#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/trace.h>
#include <sys/traceparser.h>
#include <sys/states.h>

#define PRTH_CLASS   4
#define MAX_THREADS  64
#define MAX_STATES   32

static const char *state_name(unsigned s)
{
    static const char *n[] = {
        "DEAD","RUNNING","READY","STOPPED","SEND","RECEIVE","REPLY",
        "STACK","WAITTHREAD","WAITPAGE","SIGSUSPEND","SIGWAITINFO",
        "NANOSLEEP","MUTEX","CONDVAR","JOIN","INTR","SEM","WAITCTX",
        "NET_SEND","NET_REPLY"
    };
    if (s < sizeof(n)/sizeof(n[0])) return n[s];
    if (s == (unsigned)STATE_MAX)     return "THCREATE";
    if (s == (unsigned)STATE_MAX + 1) return "THDESTROY";
    return "?";
}

typedef struct {
    int                tid;
    int                have_last;
    unsigned long long last_full;
    unsigned           last_state;
    unsigned           last_cpu;
    unsigned long long accum[MAX_STATES];
    unsigned long long total;
    unsigned long long skipped;
    unsigned           migrations;
} thread_acc_t;

static pid_t              g_target_pid;
static thread_acc_t       g_thr[MAX_THREADS];
static int                g_nthr;

#define MAX_CPU 8
static unsigned long long g_off[MAX_CPU];
static unsigned           g_last32[MAX_CPU];
static int                g_have32[MAX_CPU];

static thread_acc_t *find_thread(int tid)
{
    for (int i = 0; i < g_nthr; i++)
        if (g_thr[i].tid == tid) return &g_thr[i];
    if (g_nthr >= MAX_THREADS) return NULL;
    thread_acc_t *t = &g_thr[g_nthr++];
    memset(t, 0, sizeof(*t));
    t->tid = tid;
    return t;
}

static int on_event(struct traceparser_state *st, void *ud,
                    unsigned header, unsigned time,
                    unsigned *buffer, unsigned buffer_len)
{
    (void)st; (void)ud;

    unsigned cpu = (header >> 24) & 0x3f;
    if (cpu >= MAX_CPU) cpu = MAX_CPU - 1;
    if (g_have32[cpu] && time < g_last32[cpu]) g_off[cpu] += 0x100000000ULL;
    g_last32[cpu] = time;
    g_have32[cpu] = 1;
    unsigned long long full = g_off[cpu] + time;

    if (((header >> 10) & 0x1f) != PRTH_CLASS) return 0;
    unsigned event = header & 0x3ff;
    if (event > (unsigned)STATE_MAX + 1) return 0;
    if (buffer_len < 2) return 0;

    int pid = (int)buffer[0];
    int tid = (int)buffer[1];
    if (pid != (int)g_target_pid) return 0;

    thread_acc_t *t = find_thread(tid);
    if (!t) return 0;

    if (t->have_last) {
        unsigned long long dur = full - t->last_full;
        if (cpu != t->last_cpu) {

            t->migrations++;
            t->skipped += dur;
        } else if (t->last_state < MAX_STATES) {
            t->accum[t->last_state] += dur;
            t->total += dur;
        }
    }
    t->last_full  = full;
    t->last_state = event;
    t->last_cpu   = cpu;
    t->have_last  = 1;
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Использование: %s <файл.kev> <pid>\n", argv[0]);
        return 1;
    }
    const char *kev = argv[1];
    g_target_pid    = (pid_t)atoi(argv[2]);

    struct traceparser_state *st = traceparser_init(NULL);
    if (!st) { fprintf(stderr, "traceparser_init failed\n"); return 1; }

    for (unsigned cls = 0; cls <= 16; cls++)
        traceparser_cs_range(st, NULL, on_event, cls, 0, 1023);

    int rc = traceparser(st, NULL, kev);
    if (rc != 0) fprintf(stderr, "traceparser rc=%d\n", rc);

    unsigned len = 0;
    unsigned long long cps = 0;
    void *p = traceparser_get_info(st, _TRACEPARSER_INFO_CYCLES_PER_SEC, &len);
    if (p) cps = strtoull((const char *)p, NULL, 10);

    printf("\n============================================================\n");
    printf("  Профиль состояний потоков PID=%d\n", (int)g_target_pid);
    printf("  CYCLES_PER_SEC=%llu  потоков=%d\n", cps, g_nthr);
    printf("============================================================\n");

    for (int i = 0; i < g_nthr; i++) {
        thread_acc_t *t = &g_thr[i];
        if (t->total == 0) continue;
        double secs = cps ? (double)t->total / (double)cps : 0.0;
        printf("\n  tid=%d  наблюдалось %.3f c  (миграций между CPU: %u):\n",
               t->tid, secs, t->migrations);
        for (unsigned s = 0; s < MAX_STATES; s++) {
            if (t->accum[s] == 0) continue;
            printf("      %-12s %5.1f%%\n",
                   state_name(s), 100.0 * (double)t->accum[s] / (double)t->total);
        }
    }

    traceparser_destroy(&st);
    return 0;
}
