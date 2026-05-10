#include "collector.h"

#include <sys/neutrino.h>
#include <sys/debug.h>
#include <sys/procfs.h>
#include <sys/dcmd_proc.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>


/*
 * Размер буфера mappings процесса для подсчёта vmem.
 *
 * Буфер размещается в bss-сегменте (см. g_mapinfo_buf ниже) — память
 * выделяется один раз при загрузке процесса, никаких рантайм-аллокаций
 * на горячем пути опроса. Это соответствует архитектурному принципу
 * сервиса: фиксированный объём памяти, выделенный один раз при старте.
 *
 * 1024 * sizeof(procfs_mapinfo) ~= 40 КБ. Покрывает все реально
 * наблюдавшиеся в системе процессы. Для процессов с числом mappings
 * больше MAPINFO_MAX vmem подсчитывается частично — это документировано
 * как известное ограничение в главе 4 диплома.
 *
 * Буфер используется только из единственного потока коллектора, поэтому
 * не требует синхронизации.
 */
#define MAPINFO_MAX 1024

struct collector_state {
    collector_config_t  cfg;
    pthread_t           thread;
    volatile int        running;
    uint64_t            last_poll_ns;
    uint64_t            poll_count;
    pthread_mutex_t     stats_mutex;
};

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}


static uint8_t map_state(unsigned kernel_state)
{
    switch (kernel_state) {
    case STATE_RUNNING: return SYSMON_STATE_RUNNING;
    case STATE_READY:   return SYSMON_STATE_READY;
    case STATE_DEAD:    return SYSMON_STATE_UNKNOWN;
    default:            return SYSMON_STATE_BLOCKED;
    }
}

/*
 * Имя процесса — basename исполняемого файла из его первого mapping.
 * Если получить не удалось, имя формируется как "pid:N".
 */
static void read_proc_name(int fd, pid_t pid, char *out, size_t out_size)
{
    union {
        procfs_debuginfo info;
        char             buf[sizeof(procfs_debuginfo) + PATH_MAX];
    } map;

    memset(&map, 0, sizeof(map));

    if (devctl(fd, DCMD_PROC_MAPDEBUG_BASE,
               &map, sizeof(map), NULL) != EOK) {
        snprintf(out, out_size, "pid:%d", (int)pid);
        return;
    }

    map.buf[sizeof(map.buf) - 1] = '\0';

    const char *base = strrchr(map.info.path, '/');
    base = base ? base + 1 : map.info.path;
    strncpy(out, base, out_size - 1);
    out[out_size - 1] = '\0';
}


/*
 * Статический буфер для запроса mappings процесса.
 *
 * Выделяется в bss-сегменте при загрузке процесса. Используется
 * только из потока коллектора (единственного потребителя), поэтому
 * не требует мьютекса. Соответствует архитектурному принципу сервиса:
 * никаких рантайм-аллокаций на горячем пути.
 */
static procfs_mapinfo g_mapinfo_buf[MAPINFO_MAX];

/*
 * Подсчёт суммарного размера mappings процесса (vmem в КБ).
 *
 * Возвращает сумму sizes всех mappings, помещающихся в g_mapinfo_buf.
 * Для процессов с числом mappings > MAPINFO_MAX результат частичный
 * (учитываются только первые MAPINFO_MAX записей).
 */
static uint32_t read_proc_vmem_kb(int fd)
{
    int num = 0;

    if (devctl(fd, DCMD_PROC_MAPINFO,
               g_mapinfo_buf, sizeof(g_mapinfo_buf), &num) != EOK) {
        return 0;
    }

    /*
     * Ядро возвращает в num общее число mappings процесса. Если оно
     * превышает размер нашего буфера, в буфере лежат только первые
     * MAPINFO_MAX записей — ограничиваем счётчик соответственно.
     */
    if (num > MAPINFO_MAX) num = MAPINFO_MAX;
    if (num < 0)           num = 0;

    uint64_t total_bytes = 0;
    for (int i = 0; i < num; i++) {
        total_bytes += g_mapinfo_buf[i].size;
    }
    return (uint32_t)(total_bytes / 1024);
}

/* Перебор потоков процесса согласно протоколу DCMD_PROC_TIDSTATUS:
 *  - выставляем нужный tid в запросе
 *  - ядро возвращает поток с tid >= запрошенного
 *  - tid == 0 в ответе — потоков больше нет
 */
static void poll_threads(int fd, sysmon_record_t *rec, ringbuf_t *rb)
{
    procfs_status thr;

    memset(&thr, 0, sizeof(thr));
    thr.tid = 1;

    while (devctl(fd, DCMD_PROC_TIDSTATUS, &thr, sizeof(thr), NULL) == EOK) {
        if (thr.tid == 0) break;

        if (thr.state != STATE_DEAD) {
            rec->tid          = thr.tid;
            rec->state        = map_state(thr.state);
            rec->priority     = (uint8_t)thr.priority;
            rec->blocked_type = (uint16_t)thr.state;
            rec->sutime_ns    = thr.sutime;
            ringbuf_write(rb, rec);
        }

        thr.tid++;
    }
}

static void poll_process(pid_t pid, uint64_t timestamp, ringbuf_t *rb)
{
    char         path[64];
    int          fd;
    procfs_info  proc_info;
    sysmon_record_t rec;

    snprintf(path, sizeof(path), "/proc/%d/as", (int)pid);
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return;  
    }

    memset(&proc_info, 0, sizeof(proc_info));
    if (devctl(fd, DCMD_PROC_INFO,
               &proc_info, sizeof(proc_info), NULL) != EOK) {
        close(fd);
        return;
    }

    memset(&rec, 0, sizeof(rec));
    rec.timestamp_ns = timestamp;
    rec.pid          = pid;
    rec.vmem_kb      = read_proc_vmem_kb(fd);
    read_proc_name(fd, pid, rec.proc_name, sizeof(rec.proc_name));

    poll_threads(fd, &rec, rb);

    close(fd);
}

static void *collector_thread_func(void *arg)
{
    collector_state_t *cs = (collector_state_t *)arg;

    while (cs->running) {
        pthread_mutex_lock(&cs->stats_mutex);
        uint32_t period_ms = cs->cfg.poll_period_ms;
        pthread_mutex_unlock(&cs->stats_mutex);

        uint64_t period_ns  = (uint64_t)period_ms * 1000000ULL;
        uint64_t cycle_start = monotonic_ns();

        DIR *proc_dir = opendir("/proc");
        if (proc_dir) {
            struct dirent *entry;
            while ((entry = readdir(proc_dir)) != NULL) {
                if (!isdigit((unsigned char)entry->d_name[0])) continue;

                pid_t pid = (pid_t)atoi(entry->d_name);
                if (pid <= 0) continue;

                poll_process(pid, cycle_start, cs->cfg.ringbuf);
            }
            closedir(proc_dir);
        }

        pthread_mutex_lock(&cs->stats_mutex);
        cs->last_poll_ns = cycle_start;
        cs->poll_count++;
        pthread_mutex_unlock(&cs->stats_mutex);

        uint64_t elapsed_ns = monotonic_ns() - cycle_start;
        if (elapsed_ns < period_ns) {
            uint64_t sleep_ns = period_ns - elapsed_ns;
            struct timespec sleep_ts = {
                .tv_sec  = (time_t)(sleep_ns / 1000000000ULL),
                .tv_nsec = (long)  (sleep_ns % 1000000000ULL),
            };
            nanosleep(&sleep_ts, NULL);
        }
    }

    return NULL;
}

collector_state_t *collector_start(const collector_config_t *cfg)
{
    if (!cfg || !cfg->ringbuf) {
        return NULL;
    }

    collector_state_t *cs = calloc(1, sizeof(*cs));
    if (!cs) return NULL;

    cs->cfg     = *cfg;
    cs->running = 1;
    pthread_mutex_init(&cs->stats_mutex, NULL);

    pthread_attr_t     attr;
    struct sched_param sched = { .sched_priority = (int)cfg->collector_prio };

    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy (&attr, SCHED_RR);
    pthread_attr_setschedparam  (&attr, &sched);

    int rc = pthread_create(&cs->thread, &attr, collector_thread_func, cs);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        pthread_mutex_destroy(&cs->stats_mutex);
        free(cs);
        return NULL;
    }
    return cs;
}

void collector_stop(collector_state_t *cs)
{
    if (!cs) return;

    cs->running = 0;
    pthread_join(cs->thread, NULL);
    pthread_mutex_destroy(&cs->stats_mutex);
    free(cs);
}

uint64_t collector_get_last_poll_ns(const collector_state_t *cs)
{
    if (!cs) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&cs->stats_mutex);
    uint64_t v = cs->last_poll_ns;
    pthread_mutex_unlock((pthread_mutex_t *)&cs->stats_mutex);
    return v;
}

uint64_t collector_get_poll_count(const collector_state_t *cs)
{
    if (!cs) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&cs->stats_mutex);
    uint64_t v = cs->poll_count;
    pthread_mutex_unlock((pthread_mutex_t *)&cs->stats_mutex);
    return v;
}

void collector_set_period_ms(collector_state_t *cs, uint32_t period_ms)
{
    if (!cs) return;
    pthread_mutex_lock(&cs->stats_mutex);
    cs->cfg.poll_period_ms = period_ms;
    pthread_mutex_unlock(&cs->stats_mutex);
}
