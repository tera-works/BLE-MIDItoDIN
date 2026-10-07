/*
 * midi_uart - DIN MIDI OUT (31250 8N1) driven directly from the HW TX FIFO.
 *
 * Hot path: send_* pushes into a fixed event queue and, in the same call,
 * moves bytes from the rings into the 128 byte UART FIFO under a very short
 * critical section.  No task switch is involved.  Only when the FIFO is full
 * the remainder is sent from a TX-FIFO-empty interrupt.
 *
 * Realtime has priority. Short messages and SysEx retain arrival order.
 * A short message is always written atomically; realtime bytes may pass
 * between messages and inside SysEx.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_attr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t short_queued;
    uint32_t rt_queued;
    uint32_t short_overflow;  /* dropped: short ring full   */
    uint32_t rt_overflow;     /* dropped: realtime ring full */
    uint32_t sysex_overflow;  /* SysEx truncated: ring full  */
    uint32_t backlog_events;  /* FIFO was full, ISR had to finish */
} midi_uart_stats_t;

esp_err_t midi_uart_init(void);

/* len = 1..3, d[0] = status */
void midi_uart_send_short(const uint8_t *d, uint8_t len);
void midi_uart_send_realtime(uint8_t b);
/* chunk of a SysEx (F0 .. F7), see ble_midi_sink_t */
void midi_uart_send_sysex(const uint8_t *d, size_t len);

void midi_uart_get_stats(midi_uart_stats_t *out);
/* Checks the software backlog, FIFO and UART shift register. Task context only. */
bool midi_uart_is_idle(void);

#ifdef __cplusplus
}
#endif
