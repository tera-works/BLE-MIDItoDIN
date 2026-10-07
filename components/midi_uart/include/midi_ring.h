/*
 * midi_ring.h - lock-free single-producer / single-consumer byte ring.
 * Header only, no dependencies on ESP-IDF so it can be unit tested on a PC.
 * Capacity must be a power of two; the full capacity is usable.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    uint32_t mask;
    volatile uint32_t head; /* written by producer only */
    volatile uint32_t tail; /* written by consumer only */
} midi_ring_t;

static inline __attribute__((always_inline)) void midi_ring_init(midi_ring_t *r, uint8_t *storage, uint32_t size_pow2)
{
    r->buf = storage;
    r->mask = size_pow2 - 1;
    r->head = 0;
    r->tail = 0;
}

static inline __attribute__((always_inline)) uint32_t midi_ring_used(const midi_ring_t *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

static inline __attribute__((always_inline)) uint32_t midi_ring_free(const midi_ring_t *r)
{
    return (r->mask + 1) - midi_ring_used(r);
}

/* All-or-nothing push (producer side). */
static inline __attribute__((always_inline)) bool midi_ring_put(midi_ring_t *r, const uint8_t *src, uint32_t n)
{
    if (midi_ring_free(r) < n) {
        return false;
    }
    uint32_t h = r->head;
    for (uint32_t i = 0; i < n; i++) {
        r->buf[(h + i) & r->mask] = src[i];
    }
    __atomic_store_n(&r->head, h + n, __ATOMIC_RELEASE);
    return true;
}

/* Consumer side: look at byte at offset from tail (caller checks used()). */
static inline __attribute__((always_inline)) uint8_t midi_ring_peek(const midi_ring_t *r, uint32_t off)
{
    return r->buf[(r->tail + off) & r->mask];
}

static inline __attribute__((always_inline)) void midi_ring_drop(midi_ring_t *r, uint32_t n)
{
    __atomic_store_n(&r->tail, r->tail + n, __ATOMIC_RELEASE);
}

/*
 * SysEx push with overflow protection.
 * One byte of space is always reserved so that the terminating F7 can be
 * stored even when the ring is full; a SysEx that does not fit is truncated
 * and closed with F7 (never left open), the overflow counter is increased.
 */
static inline __attribute__((always_inline)) void midi_sysex_put(midi_ring_t *r, const uint8_t *d, size_t n, bool *dropping,
                                  uint32_t *overflow_cnt)
{
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = d[i];
        if (b == 0xF7) {
            if (!midi_ring_put(r, &b, 1)) {
                (*overflow_cnt)++;
            }
            *dropping = false;
            continue;
        }
        if (b == 0xF0) {
            *dropping = false;
        }
        if (*dropping) {
            continue;
        }
        if (midi_ring_free(r) < 2) {
            *dropping = true;
            (*overflow_cnt)++;
            continue;
        }
        midi_ring_put(r, &b, 1);
    }
}
