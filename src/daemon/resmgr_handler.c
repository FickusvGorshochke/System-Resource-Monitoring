#include "resmgr_handler.h"

#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <sys/types.h>      
#include <unistd.h>         
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


static resmgr_connect_funcs_t g_connect_funcs;
static resmgr_io_funcs_t      g_io_funcs;
static resmgr_attr_t          g_resmgr_attr;
static iofunc_attr_t          g_attr;

static resmgr_ctx_t *g_ctx = NULL;

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int sysmon_io_devctl(resmgr_context_t *ctp,
                            io_devctl_t      *msg,
                            iofunc_ocb_t     *ocb)
{
    void *dptr = _DEVCTL_DATA(msg->i);
    (void)ocb;

    switch (msg->i.dcmd) {

    case DCMD_SYSMON_GET_SNAPSHOT: {
        sysmon_snapshot_t *snap = dptr;

        snap->timestamp_ns = monotonic_ns();
        snap->reserved     = 0;
        snap->count = ringbuf_latest_snapshot(g_ctx->ringbuf,
                                              snap->records,
                                              SYSMON_SNAPSHOT_MAX);

        msg->o.ret_val = EOK;
        msg->o.nbytes  = sizeof(*snap);
        break;
    }

    case DCMD_SYSMON_GET_HISTORY: {
        sysmon_history_t *h = dptr;

        uint64_t from_ns = h->from_ns;
        uint64_t to_ns   = h->to_ns;
        pid_t    pid     = h->pid;
        int32_t  tid     = h->tid;
        uint32_t skip    = h->page * SYSMON_HISTORY_PAGE_MAX;

        h->count = ringbuf_query(g_ctx->ringbuf,
                                 from_ns, to_ns, pid, tid,
                                 skip,
                                 h->records, SYSMON_HISTORY_PAGE_MAX,
                                 &h->total);

        msg->o.ret_val = EOK;
        msg->o.nbytes  = sizeof(*h);
        break;
    }

    case DCMD_SYSMON_SET_CONFIG: {
        sysmon_config_t *cfg = dptr;

        if (cfg->poll_period_ms < 10 || cfg->poll_period_ms > 60000) {
            return EINVAL;
        }

        pthread_mutex_lock(&g_ctx->config_mutex);
        g_ctx->config = *cfg;
        pthread_mutex_unlock(&g_ctx->config_mutex);

        collector_set_period_ms(g_ctx->collector, cfg->poll_period_ms);

        msg->o.ret_val = EOK;
        msg->o.nbytes  = 0;
        break;
    }

    case DCMD_SYSMON_GET_CONFIG: {
        sysmon_config_t *cfg = dptr;

        pthread_mutex_lock(&g_ctx->config_mutex);
        *cfg = g_ctx->config;
        pthread_mutex_unlock(&g_ctx->config_mutex);

        msg->o.ret_val = EOK;
        msg->o.nbytes  = sizeof(*cfg);
        break;
    }

    case DCMD_SYSMON_GET_STATS: {
        sysmon_stats_t *stats = dptr;

        memset(stats, 0, sizeof(*stats));
        stats->uptime_ns = monotonic_ns() - g_ctx->start_ns;
        ringbuf_get_stats(g_ctx->ringbuf,
                          &stats->buffer_capacity,
                          &stats->buffer_used,
                          &stats->total_samples,
                          &stats->dropped_samples);
        stats->active_threads = ringbuf_latest_count(g_ctx->ringbuf);

        msg->o.ret_val = EOK;
        msg->o.nbytes  = sizeof(*stats);
        break;
    }

    default:
        return iofunc_devctl_default(ctp, msg, ocb);
    }

    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + msg->o.nbytes);
}

resmgr_ctx_t *resmgr_init(ringbuf_t             *rb,
                          collector_state_t     *cs,
                          const sysmon_config_t *cfg)
{
    resmgr_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->ringbuf   = rb;
    ctx->collector = cs;
    ctx->config    = *cfg;
    ctx->start_ns  = monotonic_ns();
    pthread_mutex_init(&ctx->config_mutex, NULL);

    g_ctx = ctx;

    ctx->dpp = dispatch_create();
    if (!ctx->dpp) {
        free(ctx);
        return NULL;
    }

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_connect_funcs,
                     _RESMGR_IO_NFUNCS,      &g_io_funcs);
    g_io_funcs.devctl = sysmon_io_devctl;

    iofunc_attr_init(&g_attr, S_IFCHR | 0666, NULL, NULL);

    memset(&g_resmgr_attr, 0, sizeof(g_resmgr_attr));
    g_resmgr_attr.nparts_max   = 1;
    g_resmgr_attr.msg_max_size = 16384;

    ctx->resmgr_id = resmgr_attach(ctx->dpp, &g_resmgr_attr,
                                   SYSMON_DEVICE_PATH,
                                   _FTYPE_ANY, 0,
                                   &g_connect_funcs, &g_io_funcs,
                                   &g_attr);
    if (ctx->resmgr_id == -1) {
        dispatch_destroy(ctx->dpp);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void resmgr_run(resmgr_ctx_t *ctx)
{
    dispatch_context_t *dctp = dispatch_context_alloc(ctx->dpp);
    if (!dctp) return;

    for (;;) {
        dctp = dispatch_block(dctp);
        if (!dctp) break;
        dispatch_handler(dctp);
    }

    dispatch_context_free(dctp);
}

void resmgr_shutdown(resmgr_ctx_t *ctx)
{
    if (!ctx) return;

    resmgr_detach(ctx->dpp, ctx->resmgr_id, _RESMGR_DETACH_ALL);
    dispatch_destroy(ctx->dpp);
    pthread_mutex_destroy(&ctx->config_mutex);
    free(ctx);
    g_ctx = NULL;
}
