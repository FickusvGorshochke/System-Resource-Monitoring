/*
 * case_ipc_pingpong — нагрузка через QNX IPC (MsgSend/MsgReceive).
 *
 * Создаёт два потока:
 *   - server: ChannelCreate, MsgReceive (блокировка RECEIVE), MsgReply
 *   - client: ConnectAttach, MsgSend (блокировка REPLY)
 *
 * Ожидаемое поведение в sysmon_cli snapshot:
 *   - оба потока большую часть времени в состоянии BLOCKED
 *   - server с blocked_type RECEIVE
 *   - client с blocked_type REPLY
 *   - суммарный CPU% < 1% (вся работа — блокирующие IPC-вызовы)
 *
 * Запуск:  ./case_ipc_pingpong [seconds]
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/neutrino.h>

static volatile int g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

typedef struct {
    int chid;
    uint64_t messages;
} server_ctx_t;

typedef struct {
    int coid;
    uint64_t messages;
} client_ctx_t;

static void *server_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    while (g_running) {
        char msg[64];
        int rcvid = MsgReceive(ctx->chid, msg, sizeof(msg), NULL);
        if (rcvid > 0) {
            MsgReply(rcvid, 0, "pong", 5);
            ctx->messages++;
        } else if (rcvid == 0) {
            /* pulse — игнорируем */
        }
    }
    return NULL;
}

static void *client_thread(void *arg)
{
    client_ctx_t *ctx = (client_ctx_t *)arg;
    char reply[16];

    while (g_running) {
        if (MsgSend(ctx->coid, "ping", 5, reply, sizeof(reply)) == -1) {
            break;
        }
        ctx->messages++;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 0;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int chid = ChannelCreate(0);
    if (chid == -1) {
        perror("ChannelCreate");
        return 1;
    }

    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        perror("ConnectAttach");
        return 1;
    }

    fprintf(stderr, "case_ipc_pingpong: pid=%d chid=%d coid=%d\n",
            (int)getpid(), chid, coid);

    server_ctx_t srv_ctx = { .chid = chid, .messages = 0 };
    client_ctx_t cli_ctx = { .coid = coid, .messages = 0 };

    pthread_t srv_tid, cli_tid;
    pthread_create(&srv_tid, NULL, server_thread, &srv_ctx);
    pthread_create(&cli_tid, NULL, client_thread, &cli_ctx);

    if (seconds > 0) {
        sleep(seconds);
        g_running = 0;
    } else {
        while (g_running) sleep(1);
    }

    /* Заставляем потоки выйти из блокирующих вызовов */
    ConnectDetach(coid);
    ChannelDestroy(chid);

    pthread_join(srv_tid, NULL);
    pthread_join(cli_tid, NULL);

    fprintf(stderr,
        "case_ipc_pingpong: server обработал %llu, client отправил %llu\n",
        (unsigned long long)srv_ctx.messages,
        (unsigned long long)cli_ctx.messages);
    return 0;
}
