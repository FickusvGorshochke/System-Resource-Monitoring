/*
 * sysmon_cli — клиентская утилита к /dev/sysmon.
 *
 * Команды:
 *   snapshot              — текущий снимок состояния потоков
 *   history [-p pid] [-t tid] [-n N]
 *                         — исторические данные с пагинацией
 *   stats                 — статистика сервиса
 *   config                — показать конфигурацию
 *   set [-p ms] [-H h] [-t N] [-P prio]
 *                         — изменить конфигурацию
 *   top                   — непрерывное обновление snapshot
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "../common/sysmon_protocol.h"

static const char *state_str(uint8_t state)
{
    switch ((sysmon_state_t)state) {
    case SYSMON_STATE_RUNNING: return "RUN";
    case SYSMON_STATE_READY:   return "RDY";
    case SYSMON_STATE_BLOCKED: return "BLK";
    default:                   return "???";
    }
}

/* Имена raw STATE_* из <sys/neutrino.h> */
static const char *blocked_str(uint16_t bt)
{
    switch (bt) {
    case  0: return "DEAD";
    case  1: return "RUNNING";
    case  2: return "READY";
    case  3: return "STOPPED";
    case  4: return "SEND";
    case  5: return "RECEIVE";
    case  6: return "REPLY";
    case  7: return "STACK";
    case  8: return "WAITTHREAD";
    case  9: return "WAITPAGE";
    case 10: return "SIGSUSPEND";
    case 11: return "SIGWAITINFO";
    case 12: return "NANOSLEEP";
    case 13: return "MUTEX";
    case 14: return "CONDVAR";
    case 15: return "JOIN";
    case 16: return "INTR";
    case 17: return "SEM";
    default: return "OTHER";
    }
}

static void fmt_sutime(uint64_t ns, char *buf, size_t len)
{
    uint64_t ms    = ns / 1000000ULL;
    uint64_t secs  = ms / 1000;
    uint64_t mins  = secs / 60;
    uint64_t hours = mins / 60;
    snprintf(buf, len, "%02llu:%02llu:%02llu.%03llu",
             (unsigned long long)hours,
             (unsigned long long)(mins % 60),
             (unsigned long long)(secs % 60),
             (unsigned long long)(ms   % 1000));
}

static int open_device(void)
{
    int fd = open(SYSMON_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Ошибка открытия %s: %s\n"
                        "Убедитесь, что sysmond запущен.\n",
                SYSMON_DEVICE_PATH, strerror(errno));
    }
    return fd;
}

static void print_record_row(const sysmon_record_t *r)
{
    char sutime_str[24];
    fmt_sutime(r->sutime_ns, sutime_str, sizeof(sutime_str));

    printf("%-6d %-5d %-4u %-3s %-16s %-12u %s",
           (int)r->pid,
           (int)r->tid,
           (unsigned)r->priority,
           state_str(r->state),
           sutime_str,
           (unsigned)r->vmem_kb,
           r->proc_name);

    if (r->state == SYSMON_STATE_BLOCKED) {
        printf(" [%s]", blocked_str(r->blocked_type));
    }
    printf("\n");
}

static void print_table_header(void)
{
    printf("%-6s %-5s %-4s %-3s %-16s %-12s %s\n",
           "PID", "TID", "PRI", "ST", "CPU-время", "Память(КБ)", "Процесс");
    printf("%-6s %-5s %-4s %-3s %-16s %-12s %s\n",
           "------", "-----", "----", "---",
           "----------------", "------------", "----------------");
}

static int cmd_snapshot(void)
{
    int fd = open_device();
    if (fd < 0) return 1;

    sysmon_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));

    if (devctl(fd, DCMD_SYSMON_GET_SNAPSHOT, &snap, sizeof(snap), NULL) != EOK) {
        perror("devctl(GET_SNAPSHOT)");
        close(fd);
        return 1;
    }
    close(fd);

    printf("Снимок системы (timestamp: %llu нс)\n",
           (unsigned long long)snap.timestamp_ns);
    printf("Активных потоков: %u\n\n", snap.count);

    print_table_header();
    for (uint32_t i = 0; i < snap.count; i++) {
        print_record_row(&snap.records[i]);
    }
    return 0;
}

static int cmd_stats(void)
{
    int fd = open_device();
    if (fd < 0) return 1;

    sysmon_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    if (devctl(fd, DCMD_SYSMON_GET_STATS, &stats, sizeof(stats), NULL) != EOK) {
        perror("devctl(GET_STATS)");
        close(fd);
        return 1;
    }
    close(fd);

    uint64_t uptime_s = stats.uptime_ns / 1000000000ULL;
    double   buf_pct  = stats.buffer_capacity > 0
        ? 100.0 * stats.buffer_used / stats.buffer_capacity : 0.0;
    double   buf_mb   = (double)stats.buffer_capacity * sizeof(sysmon_record_t)
                        / (1024.0 * 1024.0);

    printf("Статистика сервиса sysmond\n");
    printf("==========================\n");
    printf("Время работы:               %lluh %02llum %02llus\n",
           (unsigned long long)(uptime_s / 3600),
           (unsigned long long)((uptime_s % 3600) / 60),
           (unsigned long long)(uptime_s % 60));
    printf("Всего записей:              %llu\n",
           (unsigned long long)stats.total_samples);
    printf("Перезаписано старых:        %llu\n",
           (unsigned long long)stats.dropped_samples);
    printf("Буфер использовано:         %u / %u (%.1f%%)\n",
           stats.buffer_used, stats.buffer_capacity, buf_pct);
    printf("Размер буфера:              %.1f МБ\n", buf_mb);
    printf("Потоков в последнем снимке: %u\n", stats.active_threads);
    return 0;
}

static int cmd_config(void)
{
    int fd = open_device();
    if (fd < 0) return 1;

    sysmon_config_t cfg;
    if (devctl(fd, DCMD_SYSMON_GET_CONFIG, &cfg, sizeof(cfg), NULL) != EOK) {
        perror("devctl(GET_CONFIG)");
        close(fd);
        return 1;
    }
    close(fd);

    printf("Конфигурация sysmond:\n");
    printf("  Период опроса:      %u мс\n",   cfg.poll_period_ms);
    printf("  Глубина истории:    %u часов\n", cfg.history_hours);
    printf("  Макс. потоков:      %u\n",       cfg.max_threads);
    printf("  Приоритет коллект.: %u\n",       cfg.collector_prio);
    return 0;
}

static int cmd_set(int argc, char *argv[])
{
    int fd = open_device();
    if (fd < 0) return 1;

    sysmon_config_t cfg;
    if (devctl(fd, DCMD_SYSMON_GET_CONFIG, &cfg, sizeof(cfg), NULL) != EOK) {
        perror("devctl(GET_CONFIG)");
        close(fd);
        return 1;
    }
    close(fd);

    int  changed = 0;
    int  opt;
    optind = 0;
    while ((opt = getopt(argc, argv, "p:H:t:P:")) != -1) {
        switch (opt) {
        case 'p': cfg.poll_period_ms = (uint32_t)atoi(optarg); changed = 1; break;
        case 'H': cfg.history_hours  = (uint32_t)atoi(optarg); changed = 1; break;
        case 't': cfg.max_threads    = (uint32_t)atoi(optarg); changed = 1; break;
        case 'P': cfg.collector_prio = (uint32_t)atoi(optarg); changed = 1; break;
        default:
            fprintf(stderr, "Использование: set [-p ms] [-H h] [-t N] [-P prio]\n");
            return 1;
        }
    }

    if (!changed) {
        fprintf(stderr, "Укажите хотя бы один параметр: -p / -H / -t / -P\n");
        return 1;
    }

    fd = open_device();
    if (fd < 0) return 1;

    if (devctl(fd, DCMD_SYSMON_SET_CONFIG, &cfg, sizeof(cfg), NULL) != EOK) {
        perror("devctl(SET_CONFIG)");
        close(fd);
        return 1;
    }
    close(fd);

    printf("Конфигурация применена.\n");
    return 0;
}

static int cmd_history(int argc, char *argv[])
{
    pid_t    filter_pid = 0;
    int32_t  filter_tid = 0;
    uint32_t user_limit = 0;

    int opt;
    optind = 0;
    while ((opt = getopt(argc, argv, "p:t:n:")) != -1) {
        switch (opt) {
        case 'p': filter_pid = (pid_t)atoi(optarg);    break;
        case 't': filter_tid = (int32_t)atoi(optarg);  break;
        case 'n': user_limit = (uint32_t)atoi(optarg); break;
        default:
            fprintf(stderr,
                "Использование: history [-p pid] [-t tid] [-n max_records]\n");
            return 1;
        }
    }

    int fd = open_device();
    if (fd < 0) return 1;

    printf("%-20s ", "Время(нс)");
    print_table_header();

    sysmon_history_t h;
    uint32_t page  = 0;
    uint32_t shown = 0;
    uint32_t total = 0;

    for (;;) {
        memset(&h, 0, sizeof(h));
        h.pid  = filter_pid;
        h.tid  = filter_tid;
        h.page = page;

        int rc = devctl(fd, DCMD_SYSMON_GET_HISTORY, &h, sizeof(h), NULL);
        if (rc != EOK) {
            errno = rc;
            perror("devctl(GET_HISTORY)");
            close(fd);
            return 1;
        }

        if (page == 0) {
            total = h.total;
            if (total == 0) {
                printf("\nЗаписей нет\n");
                close(fd);
                return 0;
            }
        }

        for (uint32_t i = 0; i < h.count; i++) {
            const sysmon_record_t *r = &h.records[i];
            printf("%-20llu ", (unsigned long long)r->timestamp_ns);
            print_record_row(r);

            shown++;
            if (user_limit > 0 && shown >= user_limit) {
                printf("\nПоказано %u из %u (лимит -n)\n", shown, total);
                close(fd);
                return 0;
            }
        }

        if (h.count < SYSMON_HISTORY_PAGE_MAX) break;
        page++;
    }

    printf("\nВсего записей: %u\n", total);
    close(fd);
    return 0;
}

/*
 * cpuns <pid> — вывести "<snapshot_timestamp_ns> <cpu_time_ns>"
 *
 * Возвращает сумму sutime_ns всех потоков указанного процесса
 * из последнего собранного снимка. Используется bench.sh для
 * вычисления CPU% с наносекундной точностью.
 */
static int cmd_cpuns(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Использование: cpuns <pid>\n");
        return 1;
    }

    pid_t target_pid = (pid_t)atoi(argv[1]);
    if (target_pid <= 0) {
        fprintf(stderr, "Некорректный PID: %s\n", argv[1]);
        return 1;
    }

    int fd = open_device();
    if (fd < 0) return 1;

    sysmon_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    if (devctl(fd, DCMD_SYSMON_GET_SNAPSHOT, &snap, sizeof(snap), NULL) != EOK) {
        perror("devctl(GET_SNAPSHOT)");
        close(fd);
        return 1;
    }
    close(fd);

    uint64_t total_ns = 0;
    for (uint32_t i = 0; i < snap.count; i++) {
        if (snap.records[i].pid == target_pid) {
            total_ns += snap.records[i].sutime_ns;
        }
    }

    printf("%llu %llu\n",
           (unsigned long long)snap.timestamp_ns,
           (unsigned long long)total_ns);
    return 0;
}

static int cmd_top(void)
{
    struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
    printf("Режим top — обновление каждую секунду. Ctrl+C для выхода.\n\n");

    for (;;) {
        printf("\033[2J\033[H");      /* очистка экрана + курсор в (0,0) */

        int rc = cmd_snapshot();
        if (rc != 0) return rc;

        nanosleep(&ts, NULL);
    }
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Использование: %s <команда> [опции]\n"
        "\n"
        "Команды:\n"
        "  snapshot              Текущий снимок состояния потоков\n"
        "  history [-p pid]      Исторические данные из буфера\n"
        "          [-t tid]\n"
        "          [-n N]        Ограничить вывод N записями\n"
        "  stats                 Статистика сервиса\n"
        "  config                Показать конфигурацию\n"
        "  set [-p мс] [-H ч]   Изменить конфигурацию\n"
        "      [-t N] [-P prio]\n"
        "  top                   Непрерывное обновление\n"
        "\n"
        "Устройство: %s\n",
        prog, SYSMON_DEVICE_PATH);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *cmd = argv[1];

    if      (strcmp(cmd, "snapshot") == 0) return cmd_snapshot();
    else if (strcmp(cmd, "history")  == 0) return cmd_history(argc - 1, argv + 1);
    else if (strcmp(cmd, "stats")    == 0) return cmd_stats();
    else if (strcmp(cmd, "config")   == 0) return cmd_config();
    else if (strcmp(cmd, "set")      == 0) return cmd_set(argc - 1, argv + 1);
    else if (strcmp(cmd, "top")      == 0) return cmd_top();
    else if (strcmp(cmd, "cpuns")    == 0) return cmd_cpuns(argc - 1, argv + 1);

    fprintf(stderr, "Неизвестная команда: %s\n\n", cmd);
    print_usage(argv[0]);
    return EXIT_FAILURE;
}
