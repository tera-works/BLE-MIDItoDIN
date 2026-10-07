/* Fixed storage MIDI queue. Caller serializes producer and FIFO consumer.
 * Realtime may interrupt; all other events retain their arrival order.
 * Header-only so exactly the firmware queue can be exercised on a PC. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "midi_ring.h"

#ifndef MIDI_SHORT_RING_LEN
#define MIDI_SHORT_RING_LEN 256
#endif
#ifndef MIDI_RT_RING_LEN
#define MIDI_RT_RING_LEN 128
#endif
#ifndef MIDI_SYSEX_RING_LEN
#define MIDI_SYSEX_RING_LEN 4096
#endif
#define MIDI_INLINE static inline __attribute__((always_inline))
_Static_assert((MIDI_SHORT_RING_LEN & (MIDI_SHORT_RING_LEN - 1)) == 0, "event capacity must be power of two");
_Static_assert(MIDI_SHORT_RING_LEN >= 4, "reserve a SysEx closing event");
_Static_assert((MIDI_RT_RING_LEN & (MIDI_RT_RING_LEN - 1)) == 0, "realtime capacity must be power of two");
_Static_assert((MIDI_SYSEX_RING_LEN & (MIDI_SYSEX_RING_LEN - 1)) == 0, "SysEx capacity must be power of two");

typedef struct { uint32_t cycles; bool measure; } midi_tx_stamp_t;
typedef struct {
    midi_tx_stamp_t stamp;
    uint16_t len;
    bool sysex;
    uint8_t data[3];
} midi_tx_event_t;
typedef struct { midi_tx_stamp_t stamp; uint8_t byte; } midi_tx_rt_t;
typedef struct {
    midi_tx_event_t events[MIDI_SHORT_RING_LEN];
    midi_tx_rt_t realtime[MIDI_RT_RING_LEN];
    uint8_t sysex_storage[MIDI_SYSEX_RING_LEN];
    midi_ring_t sysex;
    uint32_t head, tail, rt_head, rt_tail;
    bool sysex_input_open, sysex_dropping;
    uint32_t short_queued, rt_queued, short_overflow, rt_overflow, sysex_overflow;
} midi_tx_queue_t;

MIDI_INLINE uint32_t midi_tx_event_free(const midi_tx_queue_t *q)
{ return MIDI_SHORT_RING_LEN - (q->head - q->tail); }

MIDI_INLINE void midi_tx_queue_init(midi_tx_queue_t *q)
{
    q->head = q->tail = q->rt_head = q->rt_tail = 0;
    q->sysex_input_open = q->sysex_dropping = false;
    q->short_queued = q->rt_queued = 0;
    q->short_overflow = q->rt_overflow = q->sysex_overflow = 0;
    midi_ring_init(&q->sysex, q->sysex_storage, MIDI_SYSEX_RING_LEN);
}

MIDI_INLINE bool midi_tx_queue_short(midi_tx_queue_t *q, const uint8_t *d, uint8_t n, midi_tx_stamp_t stamp)
{
    /* A parser must close SysEx before it delivers a non-realtime status. */
    if (n == 0 || n > 3 || q->sysex_input_open || midi_tx_event_free(q) <= 1) {
        q->short_overflow++;
        return false;
    }
    midi_tx_event_t *e = &q->events[q->head & (MIDI_SHORT_RING_LEN - 1)];
    e->len = n; e->sysex = false; e->stamp = stamp;
    for (uint8_t i = 0; i < n; i++) e->data[i] = d[i];
    q->head++;
    q->short_queued++;
    return true;
}

MIDI_INLINE bool midi_tx_queue_rt(midi_tx_queue_t *q, uint8_t b, midi_tx_stamp_t stamp)
{
    if (q->rt_head - q->rt_tail == MIDI_RT_RING_LEN) {
        q->rt_overflow++;
        return false;
    }
    midi_tx_rt_t *e = &q->realtime[q->rt_head & (MIDI_RT_RING_LEN - 1)];
    e->byte = b; e->stamp = stamp;
    q->rt_head++;
    q->rt_queued++;
    return true;
}

MIDI_INLINE void midi_tx_sysex_commit(midi_tx_queue_t *q, uint16_t n, midi_tx_stamp_t stamp)
{
    midi_tx_event_t *e = &q->events[q->head & (MIDI_SHORT_RING_LEN - 1)];
    e->len = n; e->sysex = true; e->stamp = stamp;
    q->head++;
}

MIDI_INLINE void midi_tx_sysex_close(midi_tx_queue_t *q)
{
    if (q->sysex_input_open) {
        const uint8_t end = 0xF7;
        /* One data byte AND one event slot are reserved while SysEx is open. */
        midi_ring_put(&q->sysex, &end, 1);
        midi_tx_stamp_t none = {0, false};
        midi_tx_sysex_commit(q, 1, none);
        q->sysex_input_open = false;
    }
}

MIDI_INLINE void midi_tx_queue_sysex(midi_tx_queue_t *q, const uint8_t *d, size_t len, midi_tx_stamp_t stamp)
{
    uint16_t span = 0;
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = d[i];
        if (q->sysex_dropping) {
            if (b == 0xF7) q->sysex_dropping = false;
            continue;
        }
        if (b == 0xF7 && !q->sysex_input_open) continue;
        if (b != 0xF0 && !q->sysex_input_open) continue;
        /* A complete closing chunk may consume the reserved descriptor. */
        const uint32_t slots_needed = b == 0xF7 ? 1 : 2;
        const uint32_t bytes_needed = b == 0xF7 ? 1 : 2;
        if ((!span && midi_tx_event_free(q) < slots_needed) || midi_ring_free(&q->sysex) < bytes_needed) {
            if (span) {
                midi_tx_sysex_commit(q, span, stamp);
                span = 0;
                stamp.measure = false;
            }
            q->sysex_overflow++;
            midi_tx_sysex_close(q);
            q->sysex_dropping = b != 0xF7;
            continue;
        }
        midi_ring_put(&q->sysex, &b, 1);
        span++;
        if (b == 0xF0) q->sysex_input_open = true;
        if (b == 0xF7) {
            q->sysex_input_open = false;
            midi_tx_sysex_commit(q, span, stamp);
            span = 0;
            stamp.measure = false;
        }
    }
    if (span) midi_tx_sysex_commit(q, span, stamp);
}

MIDI_INLINE bool midi_tx_queue_pending(const midi_tx_queue_t *q)
{ return q->rt_head != q->rt_tail || q->head != q->tail; }

/* Safe direct-FIFO eligibility shared by firmware and host boundary tests. */
MIDI_INLINE bool midi_tx_queue_can_direct_short(const midi_tx_queue_t *q, uint8_t len, uint32_t room)
{
    return len > 0 && len <= 3 && room >= len &&
        !q->sysex_input_open && !midi_tx_queue_pending(q);
}

/* Up to three bytes, bounded by FIFO room. Short messages stay indivisible. */
MIDI_INLINE uint8_t midi_tx_queue_pop(midi_tx_queue_t *q, uint8_t out[3], uint32_t room, midi_tx_stamp_t *stamp)
{
    if (!room) return 0;
    if (q->rt_head != q->rt_tail) {
        midi_tx_rt_t *e = &q->realtime[q->rt_tail & (MIDI_RT_RING_LEN - 1)];
        out[0] = e->byte; *stamp = e->stamp; q->rt_tail++;
        return 1;
    }
    if (q->head == q->tail) return 0;
    midi_tx_event_t *e = &q->events[q->tail & (MIDI_SHORT_RING_LEN - 1)];
    if (!e->sysex) {
        if (room < e->len) return 0;
        for (uint8_t i = 0; i < e->len; i++) out[i] = e->data[i];
        *stamp = e->stamp;
        q->tail++;
        return (uint8_t)e->len;
    }
    uint8_t n = e->len < 3 ? (uint8_t)e->len : 3;
    if (n > room) n = (uint8_t)room;
    for (uint8_t i = 0; i < n; i++) out[i] = midi_ring_peek(&q->sysex, i);
    midi_ring_drop(&q->sysex, n);
    *stamp = e->stamp;
    e->stamp.measure = false;
    e->len -= n;
    if (!e->len) q->tail++;
    return n;
}
