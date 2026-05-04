/*
 * resmgr_handler.h — Resource Manager для /dev/sysmon.
 */

#ifndef SYSMON_RESMGR_HANDLER_H
#define SYSMON_RESMGR_HANDLER_H

#include <pthread.h>

#include "ringbuf.h"
#include "collector.h"
#include "../common/sysmon_protocol.h"

#include <sys/iofunc.h>
#include <sys/dispatch.h>

typedef struct {
    dispatch_t          *dpp;
    int                  resmgr_id;
    ringbuf_t           *ringbuf;
    collector_state_t   *collector;
    uint64_t             start_ns;
    sysmon_config_t      config;
    pthread_mutex_t      config_mutex;
} resmgr_ctx_t;

resmgr_ctx_t *resmgr_init(ringbuf_t             *rb,
                          collector_state_t     *cs,
                          const sysmon_config_t *cfg);

void resmgr_run     (resmgr_ctx_t *ctx);
void resmgr_shutdown(resmgr_ctx_t *ctx);

#endif /* SYSMON_RESMGR_HANDLER_H */
