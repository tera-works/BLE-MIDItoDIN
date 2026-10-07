#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "oled_frame.h"
#define OLED_CHUNK_BYTES 16
#define OLED_PACKET_BYTES (OLED_CHUNK_BYTES + 7)
#define OLED_VALID_ALL 0xFFu
/* Every page has 8 validity bits, one per 16-column block. */
bool oled_next_chunk(const uint8_t wanted[OLED_WIDTH], const uint8_t sent[OLED_WIDTH],
                    uint8_t valid, unsigned *first, unsigned *length);
/* Co=1 command bytes then Co=0 data stream, in a single I2C transaction. */
size_t oled_make_packet(uint8_t out[OLED_PACKET_BYTES], unsigned page, unsigned first,
                        const uint8_t *data, unsigned length);
void oled_commit_chunk(uint8_t sent[OLED_WIDTH], uint8_t *valid,
                       unsigned first, const uint8_t *data, unsigned length);
/* Unsigned subtraction handles a recent cycle-counter rollover. */
static inline bool oled_transport_quiet(uint32_t now, uint32_t last_rx,
                                       uint32_t quiet_cycles, bool uart_idle)
{ return uart_idle && (uint32_t)(now-last_rx) >= quiet_cycles; }
