/*
 * sysmond — сервис системного профилирования для ЗОСРВ "Нейтрино".
 *
 * Регистрирует /dev/sysmon, периодически опрашивает /proc, складывает
 * метрики в кольцевой буфер. Клиенты получают данные через devctl.
 *
 * При корректном завершении (SIGTERM / SIGINT) содержимое буфера
 * сохраняется в файл-дамп. При следующем запуске история подгружается
 * обратно, обеспечивая непрерывность наблюдения через рестарты.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>

#include "ringbuf.h"
#include "collector.h"
#include "resmgr_handler.h"
#include "../common/sysmon_protocol.h"

#define DEFAULT_POLL_MS         1000
#define DEFAULT_HISTORY_HOURS     24
#define DEFAULT_MAX_THREADS        8
#define DEFAULT_COLLECTOR_PRIO    10

#define SYSMOND_LOG_PATH        "/var/log/sysmond.log"
#define SYSMOND_DUMP_PATH       "/var/log/sysmond_history.bin"

/*
 * Глобальные указатели на ring-буфер и путь дампа — нужны обработчику
 * сигнала, чтобы корректно сохранить буфер при завершении.
 */
static ringbuf_t   *g_rb        = NULL;
static const char  *g_dump_path = SYSMOND_DUMP_PATH;
static volatile sig_atomic_t g_should_stop = 0;

static void on_terminate(int sig)
{
    (void)sig;
    g_should_stop = 1;
    /*
     * Сохранение делается в основном потоке после выхода из resmgr_run().
     * В обработчике сигнала вызовы fwrite/fopen формально не safe,
     * поэтому только взводим флаг.
     */
}

static void daemonize(const char *log_path)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    if (setsid() < 0) {
        perror("setsid");
        exit(EXIT_FAILURE);
    }

    freopen("/dev/null", "r", stdin);
    if (!freopen(log_path, "a", stdout)) {
        freopen("/dev/null", "w", stdout);
    }
    if (!freopen(log_path, "a", stderr)) {
        freopen("/dev/null", "w", stderr);
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Использование: %s [опции]\n"
        "\n"
        "Сервис системного профилирования ЗОСРВ \"Нейтрино\".\n"
        "Регистрирует устройство %s\n"
        "\n"
        "Опции:\n"
        "  -p <мс>    Период опроса, мс         (по умолч. %d)\n"
        "  -H <ч>     Глубина истории, часы      (по умолч. %d)\n"
        "  -t <N>     Макс. число потоков        (по умолч. %d)\n"
        "  -P <prio>  Приоритет коллектора       (по умолч. %d)\n"
        "  -l <путь>  Файл дампа истории         (по умолч. %s)\n"
        "  -L         Отключить дамп истории при завершении\n"
        "  -N         Не загружать историю при старте\n"
        "  -f         Не уходить в фон\n"
        "  -h         Показать эту справку\n"
        "\n"
        "В режиме демона лог пишется в %s\n"
        "Дамп истории — бинарный формат с заголовком \"SYSMOND\\0\" + версия + записи\n",
        prog, SYSMON_DEVICE_PATH,
        DEFAULT_POLL_MS, DEFAULT_HISTORY_HOURS,
        DEFAULT_MAX_THREADS, DEFAULT_COLLECTOR_PRIO,
        SYSMOND_DUMP_PATH, SYSMOND_LOG_PATH);
}

int main(int argc, char *argv[])
{
    int         foreground   = 0;
    int         dump_disabled = 0;
    int         load_disabled = 0;
    uint32_t    poll_ms      = DEFAULT_POLL_MS;
    uint32_t    hist_h       = DEFAULT_HISTORY_HOURS;
    uint32_t    max_thr      = DEFAULT_MAX_THREADS;
    uint32_t    col_prio     = DEFAULT_COLLECTOR_PRIO;
    const char *dump_path    = SYSMOND_DUMP_PATH;

    int opt;
    while ((opt = getopt(argc, argv, "p:H:t:P:l:LNfh")) != -1) {
        switch (opt) {
        case 'p': poll_ms    = (uint32_t)atoi(optarg); break;
        case 'H': hist_h     = (uint32_t)atoi(optarg); break;
        case 't': max_thr    = (uint32_t)atoi(optarg); break;
        case 'P': col_prio   = (uint32_t)atoi(optarg); break;
        case 'l': dump_path     = optarg;              break;
        case 'L': dump_disabled = 1;                   break;
        case 'N': load_disabled = 1;                   break;
        case 'f': foreground    = 1;                   break;
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (poll_ms < 10 || poll_ms > 60000) {
        fprintf(stderr, "Ошибка: период опроса должен быть 10..60000 мс\n");
        return EXIT_FAILURE;
    }
    if (hist_h < 1 || hist_h > 168) {
        fprintf(stderr, "Ошибка: глубина истории должна быть 1..168 часов\n");
        return EXIT_FAILURE;
    }
    if (max_thr < 1 || max_thr > 256) {
        fprintf(stderr, "Ошибка: max_threads должно быть 1..256\n");
        return EXIT_FAILURE;
    }

    uint32_t samples_per_hour = 3600u * 1000u / poll_ms;
    uint32_t capacity         = max_thr * hist_h * samples_per_hour;
    size_t   mem_bytes        = (size_t)capacity * sizeof(sysmon_record_t);

    if (!foreground) {
        daemonize(SYSMOND_LOG_PATH);
    }

    fprintf(stderr,
        "sysmond: period=%u мс, history=%u ч, max_threads=%u\n"
        "sysmond: буфер %u записей × %zu байт = %.1f МБ\n"
        "sysmond: устройство %s\n"
        "sysmond: дамп %s\n",
        poll_ms, hist_h, max_thr,
        capacity, sizeof(sysmon_record_t),
        (double)mem_bytes / (1024.0 * 1024.0),
        SYSMON_DEVICE_PATH,
        dump_disabled ? "отключён" : dump_path);

    /* Сигналы — graceful shutdown */
    struct sigaction sa = {0};
    sa.sa_handler = on_terminate;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    ringbuf_t rb;
    fprintf(stderr, "[1/4] Инициализация кольцевого буфера... ");
    if (ringbuf_init(&rb, capacity) != 0) {
        fprintf(stderr, "FAIL\n");
        perror("ringbuf_init");
        return EXIT_FAILURE;
    }
    fprintf(stderr, "OK\n");
    g_rb        = &rb;
    g_dump_path = dump_path;

    /* Подгружаем дамп предыдущего запуска, если есть */
    if (!load_disabled) {
        struct stat st;
        if (stat(dump_path, &st) == 0) {
            int loaded = ringbuf_load_from_file(&rb, dump_path);
            if (loaded > 0) {
                fprintf(stderr,
                    "[+] Подгружено %d записей из %s\n", loaded, dump_path);
            } else if (loaded == -1) {
                fprintf(stderr,
                    "[!] Не удалось подгрузить %s: %s\n",
                    dump_path, strerror(errno));
            }
        }
    }

    fprintf(stderr, "[2/4] Запуск потока коллектора... ");
    collector_config_t col_cfg = {
        .poll_period_ms = poll_ms,
        .collector_prio = col_prio,
        .ringbuf        = &rb,
    };
    collector_state_t *cs = collector_start(&col_cfg);
    if (!cs) {
        fprintf(stderr, "FAIL\n");
        perror("collector_start");
        ringbuf_destroy(&rb);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "OK\n");

    fprintf(stderr, "[3/4] Регистрация %s... ", SYSMON_DEVICE_PATH);
    sysmon_config_t sysmon_cfg = {
        .poll_period_ms = poll_ms,
        .history_hours  = hist_h,
        .max_threads    = max_thr,
        .collector_prio = col_prio,
    };
    resmgr_ctx_t *rm = resmgr_init(&rb, cs, &sysmon_cfg);
    if (!rm) {
        fprintf(stderr, "FAIL\n");
        perror("resmgr_init");
        collector_stop(cs);
        ringbuf_destroy(&rb);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "OK\n");

    fprintf(stderr, "[4/4] Сервис готов.\n");
    resmgr_run(rm);

    /* Сохранение буфера на диск перед завершением */
    if (!dump_disabled) {
        fprintf(stderr, "Сохранение истории в %s... ", dump_path);
        if (ringbuf_dump_to_file(&rb, dump_path) == 0) {
            fprintf(stderr, "OK\n");
        } else {
            fprintf(stderr, "FAIL: %s\n", strerror(errno));
        }
    }

    resmgr_shutdown(rm);
    collector_stop(cs);
    ringbuf_destroy(&rb);

    return EXIT_SUCCESS;
}
