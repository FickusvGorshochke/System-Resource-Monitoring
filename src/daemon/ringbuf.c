#include "ringbuf.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int ringbuf_init(ringbuf_t *rb, uint32_t capacity)
{
    if (!rb || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    rb->records = malloc(sizeof(sysmon_record_t) * capacity);
    if (!rb->records) {
        return -1;
    }

    rb->capacity      = capacity;
    rb->head          = 0;
    rb->count         = 0;
    rb->total_written = 0;
    rb->total_dropped = 0;
    pthread_mutex_init(&rb->mutex, NULL);
    return 0;
}

void ringbuf_destroy(ringbuf_t *rb)
{
    if (!rb) return;

    pthread_mutex_destroy(&rb->mutex);
    free(rb->records);
    rb->records  = NULL;
    rb->capacity = 0;
    rb->head     = 0;
    rb->count    = 0;
}

void ringbuf_write(ringbuf_t *rb, const sysmon_record_t *rec)
{
    pthread_mutex_lock(&rb->mutex);

    if (rb->count == rb->capacity) {
        rb->total_dropped++;
    } else {
        rb->count++;
    }

    rb->records[rb->head] = *rec;
    rb->head = (rb->head + 1) % rb->capacity;
    rb->total_written++;

    pthread_mutex_unlock(&rb->mutex);
}


static inline uint32_t slot_of(const ringbuf_t *rb, uint32_t i)
{
    uint32_t tail = (rb->head + rb->capacity - rb->count) % rb->capacity;
    return (tail + i) % rb->capacity;
}

uint32_t ringbuf_latest_snapshot(ringbuf_t       *rb,
                                 sysmon_record_t *out,
                                 uint32_t         max_out)
{
    uint32_t copied = 0;

    pthread_mutex_lock(&rb->mutex);

    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    uint32_t newest = (rb->head + rb->capacity - 1) % rb->capacity;
    uint64_t latest_ts = rb->records[newest].timestamp_ns;

    for (uint32_t i = 0; i < rb->count && copied < max_out; i++) {
        const sysmon_record_t *r = &rb->records[slot_of(rb, i)];
        if (r->timestamp_ns == latest_ts) {
            out[copied++] = *r;
        }
    }

    pthread_mutex_unlock(&rb->mutex);
    return copied;
}

uint32_t ringbuf_latest_count(ringbuf_t *rb)
{
    uint32_t count = 0;

    pthread_mutex_lock(&rb->mutex);

    if (rb->count == 0) {
        pthread_mutex_unlock(&rb->mutex);
        return 0;
    }

    uint32_t newest = (rb->head + rb->capacity - 1) % rb->capacity;
    uint64_t latest_ts = rb->records[newest].timestamp_ns;

    for (uint32_t i = 0; i < rb->count; i++) {
        if (rb->records[slot_of(rb, i)].timestamp_ns == latest_ts) {
            count++;
        }
    }

    pthread_mutex_unlock(&rb->mutex);
    return count;
}

uint32_t ringbuf_query(ringbuf_t       *rb,
                       uint64_t         from_ns,
                       uint64_t         to_ns,
                       pid_t            pid,
                       int32_t          tid,
                       uint32_t         skip,
                       sysmon_record_t *out,
                       uint32_t         max_out,
                       uint32_t        *out_total)
{
    uint32_t matched = 0;
    uint32_t copied  = 0;

    pthread_mutex_lock(&rb->mutex);

    for (uint32_t i = 0; i < rb->count; i++) {
        const sysmon_record_t *r = &rb->records[slot_of(rb, i)];

        if (from_ns != 0 && r->timestamp_ns < from_ns) continue;
        if (to_ns   != 0 && r->timestamp_ns > to_ns  ) continue;
        if (pid     != 0 && r->pid          != pid   ) continue;
        if (tid     != 0 && r->tid          != tid   ) continue;

        if (matched >= skip && copied < max_out) {
            out[copied++] = *r;
        }
        matched++;
    }

    if (out_total) *out_total = matched;

    pthread_mutex_unlock(&rb->mutex);
    return copied;
}

void ringbuf_get_stats(ringbuf_t *rb,
                       uint32_t  *out_capacity,
                       uint32_t  *out_used,
                       uint64_t  *out_total_written,
                       uint64_t  *out_total_dropped)
{
    pthread_mutex_lock(&rb->mutex);
    if (out_capacity)      *out_capacity      = rb->capacity;
    if (out_used)          *out_used          = rb->count;
    if (out_total_written) *out_total_written = rb->total_written;
    if (out_total_dropped) *out_total_dropped = rb->total_dropped;
    pthread_mutex_unlock(&rb->mutex);
}

/*
 * Формат файла-дампа:
 *   offset  size   field
 *   0       8      magic         = "SYSMOND\0"
 *   8       4      version       = 1
 *   12      4      record_size   = sizeof(sysmon_record_t)
 *   16      4      count         (число записей)
 *   20      4      reserved      = 0
 *   24      N*sz   records       (записи в хронологическом порядке)
 */

#define DUMP_MAGIC    "SYSMOND\0"
#define DUMP_VERSION  1
#define DUMP_HDR_SIZE 24

int ringbuf_dump_to_file(ringbuf_t *rb, const char *path)
{
    if (!rb || !path) {
        errno = EINVAL;
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    pthread_mutex_lock(&rb->mutex);

    uint32_t version = DUMP_VERSION;
    uint32_t rec_sz  = (uint32_t)sizeof(sysmon_record_t);
    uint32_t count   = rb->count;
    uint32_t zero    = 0;

    if (fwrite(DUMP_MAGIC, 1, 8, f) != 8 ||
        fwrite(&version,  4, 1, f) != 1 ||
        fwrite(&rec_sz,   4, 1, f) != 1 ||
        fwrite(&count,    4, 1, f) != 1 ||
        fwrite(&zero,     4, 1, f) != 1)
    {
        pthread_mutex_unlock(&rb->mutex);
        fclose(f);
        return -1;
    }

    /* Записи в хронологическом порядке: от хвоста к голове */
    uint32_t tail = (rb->head + rb->capacity - rb->count) % rb->capacity;
    for (uint32_t i = 0; i < rb->count; i++) {
        uint32_t slot = (tail + i) % rb->capacity;
        if (fwrite(&rb->records[slot], rec_sz, 1, f) != 1) {
            pthread_mutex_unlock(&rb->mutex);
            fclose(f);
            return -1;
        }
    }

    pthread_mutex_unlock(&rb->mutex);
    fclose(f);
    return 0;
}

int ringbuf_load_from_file(ringbuf_t *rb, const char *path)
{
    if (!rb || !path) {
        errno = EINVAL;
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char     magic[8];
    uint32_t version, rec_sz, count, reserved;

    if (fread(magic,    1, 8, f) != 8 ||
        fread(&version, 4, 1, f) != 1 ||
        fread(&rec_sz,  4, 1, f) != 1 ||
        fread(&count,   4, 1, f) != 1 ||
        fread(&reserved,4, 1, f) != 1)
    {
        fclose(f);
        errno = EIO;
        return -1;
    }

    if (memcmp(magic, DUMP_MAGIC, 8) != 0 ||
        version != DUMP_VERSION ||
        rec_sz  != sizeof(sysmon_record_t))
    {
        fclose(f);
        errno = EILSEQ;
        return -1;
    }

    sysmon_record_t rec;
    int loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (fread(&rec, sizeof(rec), 1, f) != 1) break;
        ringbuf_write(rb, &rec);
        loaded++;
    }

    fclose(f);
    return loaded;
}
