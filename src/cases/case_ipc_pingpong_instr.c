#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/neutrino.h>
#include <sys/trace.h>

#define LOG_EVERY 5000

static volatile int g_running = 1;
static void on_signal(int sig) { (void)sig; g_running = 0; }

typedef struct { int chid; uint64_t messages; } server_ctx_t;
typedef struct { int coid; uint64_t messages; } client_ctx_t;

static void *server_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;
    while (g_running) {
        char msg[64];
        int rcvid = MsgReceive(ctx->chid, msg, sizeof(msg), NULL);
        if (rcvid > 0) {
            MsgReply(rcvid, 0, "pong", 5);
            if (ctx->messages % LOG_EVERY == 0)
                trace_logi(931, (unsigned)ctx->messages, 0);
            ctx->messages++;
        }
    }
    return NULL;
}

static void *client_thread(void *arg)
{
    client_ctx_t *ctx = (client_ctx_t *)arg;
    char reply[16];
    while (g_running) {
        if (MsgSend(ctx->coid, "ping", 5, reply, sizeof(reply)) == -1) break;
        if (ctx->messages % LOG_EVERY == 0)
            trace_logi(930, (unsigned)ctx->messages, 0);
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
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    fprintf(stderr, "case_ipc_pingpong_instr: pid=%d chid=%d coid=%d\n",
            (int)getpid(), chid, coid);
    trace_logf(932, "ipc start chid=%d coid=%d", chid, coid);

    server_ctx_t srv = { .chid = chid, .messages = 0 };
    client_ctx_t cli = { .coid = coid, .messages = 0 };
    pthread_t st, ct;
    pthread_create(&st, NULL, server_thread, &srv);
    pthread_create(&ct, NULL, client_thread, &cli);

    if (seconds > 0) { sleep(seconds); g_running = 0; }
    else while (g_running) sleep(1);

    ConnectDetach(coid);
    ChannelDestroy(chid);
    pthread_join(st, NULL);
    pthread_join(ct, NULL);

    fprintf(stderr, "case_ipc_pingpong_instr: server=%llu client=%llu\n",
            (unsigned long long)srv.messages, (unsigned long long)cli.messages);
    return 0;
}
