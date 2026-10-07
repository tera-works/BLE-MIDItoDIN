#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_attr.h"

typedef struct {
    uint32_t count, min_cycles, max_cycles;
    uint64_t sum_cycles;
    uint32_t cpu_mhz;
} latency_stats_t;

void latency_debug_init(void);
void latency_debug_rx_enter(void);
void latency_debug_rx_exit(void);
/* Claim one sample per notification; carry its origin through the output queue. */
bool latency_debug_claim_sample(uint32_t *cycles);
void latency_debug_uart_write(uint32_t started, bool measure);
void latency_debug_uart_write_done(void);
void latency_debug_get_stats(latency_stats_t *out, int reset);

/* Read-only UI activity observation; not a new timestamp in the MIDI path. */
uint32_t latency_debug_last_rx_cycles(void);
