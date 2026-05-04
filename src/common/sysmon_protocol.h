/*
 * sysmon_protocol.h — публичный протокол сервиса системного профилирования
 *
 * Используется как сервисом sysmond, так и клиентами (sysmon_cli и др.).
 * Совместим с ЗОСРВ "Нейтрино" редакции 2021 (СВД ВС / КПДА).
 */

#ifndef SYSMON_PROTOCOL_H
#define SYSMON_PROTOCOL_H

#include <stdint.h>
#include <sys/types.h>
#include <devctl.h>

#define SYSMON_DEVICE_PATH       "/dev/sysmon"

#define SYSMON_PROC_NAME_MAX     16
#define SYSMON_SNAPSHOT_MAX      128
#define SYSMON_HISTORY_PAGE_MAX  64

typedef enum {
    SYSMON_STATE_UNKNOWN  = 0,
    SYSMON_STATE_RUNNING  = 1,
    SYSMON_STATE_READY    = 2,
    SYSMON_STATE_BLOCKED  = 3,
} sysmon_state_t;

/*
 * sysmon_record_t — снимок одного потока в один момент времени.

 */
typedef struct {
    uint64_t  timestamp_ns;
    pid_t     pid;
    int32_t   tid;
    uint8_t   state;            
    uint8_t   priority;
    uint16_t  blocked_type;     
    uint64_t  sutime_ns;        
    uint32_t  vmem_kb;          
    char      proc_name[SYSMON_PROC_NAME_MAX];
} sysmon_record_t;

typedef struct {
    uint64_t        timestamp_ns;
    uint32_t        count;
    uint32_t        reserved;
    sysmon_record_t records[SYSMON_SNAPSHOT_MAX];
} sysmon_snapshot_t;

/*
 * sysmon_history_t — пагинированная выборка из ring-буфера.
 *
 * Клиент: заполняет фильтры, page=0; повторяет devctl с page=1,2,...
 * Сервер: возвращает соответствующую страницу записей и общее число
 *         совпадающих записей в total.
 */
typedef struct {
    uint64_t  from_ns;
    uint64_t  to_ns;
    pid_t     pid;              /* 0 = все процессы */
    int32_t   tid;              /* 0 = все потоки   */
    uint32_t  page;
    uint32_t  reserved_in;

    uint32_t  count;            /* записей в этой странице */
    uint32_t  total;            /* всего совпадающих в буфере */

    sysmon_record_t records[SYSMON_HISTORY_PAGE_MAX];
} sysmon_history_t;

typedef struct {
    uint32_t  poll_period_ms;
    uint32_t  history_hours;
    uint32_t  max_threads;
    uint32_t  collector_prio;
} sysmon_config_t;

typedef struct {
    uint64_t  total_samples;
    uint64_t  dropped_samples;
    uint64_t  uptime_ns;
    uint32_t  buffer_capacity;
    uint32_t  buffer_used;
    uint32_t  active_threads;
    uint32_t  reserved;
} sysmon_stats_t;

#define DCMD_SYSMON_GET_SNAPSHOT  __DIOF (_DCMD_MISC, 0x80, sysmon_snapshot_t)
#define DCMD_SYSMON_GET_HISTORY   __DIOTF(_DCMD_MISC, 0x81, sysmon_history_t)
#define DCMD_SYSMON_SET_CONFIG    __DIOT (_DCMD_MISC, 0x82, sysmon_config_t)
#define DCMD_SYSMON_GET_CONFIG    __DIOF (_DCMD_MISC, 0x83, sysmon_config_t)
#define DCMD_SYSMON_GET_STATS     __DIOF (_DCMD_MISC, 0x84, sysmon_stats_t)

#endif /* SYSMON_PROTOCOL_H */
