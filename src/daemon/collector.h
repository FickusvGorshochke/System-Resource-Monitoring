#ifndef SYSMON_COLLECTOR_H
#define SYSMON_COLLECTOR_H

#include <stdint.h>
#include "ringbuf.h"

typedef struct {
    uint32_t   poll_period_ms;
    uint32_t   collector_prio;
    ringbuf_t *ringbuf;
} collector_config_t;

typedef struct collector_state collector_state_t;

collector_state_t *collector_start(const collector_config_t *cfg);
void               collector_stop (collector_state_t *cs);

uint64_t collector_get_last_poll_ns(const collector_state_t *cs);
uint64_t collector_get_poll_count  (const collector_state_t *cs);

void collector_set_period_ms(collector_state_t *cs, uint32_t period_ms);

#endif
