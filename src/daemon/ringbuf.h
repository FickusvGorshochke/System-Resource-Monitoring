/*
 * ringbuf.h — потокобезопасный кольцевой буфер записей sysmon_record_t.
 *
 * При заполнении перезаписывает самые старые данные (overwrite-on-full).
 */

#ifndef SYSMON_RINGBUF_H
#define SYSMON_RINGBUF_H

#include <stdint.h>
#include <pthread.h>
#include "../common/sysmon_protocol.h"

typedef struct {
    sysmon_record_t  *records;
    uint32_t          capacity;
    uint32_t          head;
    uint32_t          count;
    uint64_t          total_written;
    uint64_t          total_dropped;
    pthread_mutex_t   mutex;
} ringbuf_t;

int  ringbuf_init   (ringbuf_t *rb, uint32_t capacity);
void ringbuf_destroy(ringbuf_t *rb);

void ringbuf_write(ringbuf_t *rb, const sysmon_record_t *rec);

uint32_t ringbuf_latest_snapshot(ringbuf_t       *rb,
                                 sysmon_record_t *out,
                                 uint32_t         max_out);

uint32_t ringbuf_latest_count(ringbuf_t *rb);


uint32_t ringbuf_query(ringbuf_t       *rb,
                       uint64_t         from_ns,
                       uint64_t         to_ns,
                       pid_t            pid,
                       int32_t          tid,
                       uint32_t         skip,
                       sysmon_record_t *out,
                       uint32_t         max_out,
                       uint32_t        *out_total);

void ringbuf_get_stats(ringbuf_t *rb,
                       uint32_t  *out_capacity,
                       uint32_t  *out_used,
                       uint64_t  *out_total_written,
                       uint64_t  *out_total_dropped);

/*
 * Сохранить содержимое буфера в файл бинарного формата.
 * Формат: [магическое число "SYSMON" + версия + count + records...]
 * Возвращает 0 при успехе, -1 при ошибке (errno установлен).
 */
int ringbuf_dump_to_file(ringbuf_t *rb, const char *path);

/*
 * Загрузить содержимое из ранее сохранённого файла.
 * Записи добавляются в буфер обычным write (с возможной перезаписью
 * старых, если буфер мал). Возвращает число загруженных записей или -1.
 */
int ringbuf_load_from_file(ringbuf_t *rb, const char *path);

#endif /* SYSMON_RINGBUF_H */
