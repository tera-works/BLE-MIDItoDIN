/*
 * BLE MIDI Central -> DIN MIDI OUT   (Phase 1, ESP32-SOLO-1, ESP-IDF + NimBLE)
 *
 * Critical path (all inside the NimBLE host task, no queue / task switch):
 *   GATT notification -> ble_midi_parser_feed() -> sink_* -> midi_uart_send_*()
 *   -> SPSC ring -> UART HW TX FIFO
 */
#include <stdio.h>

#include "ble_midi_client.h"
#include "ble_midi_parser.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "latency_debug.h"
#include "midi_uart.h"
#include "oled_ui.h"
#include "encoder_input.h"
#include "serial_console.h"

static const char *TAG = "MAIN";

/* ---- parser sink -> DIN MIDI (thin adapters, called in the hot path) ---- */
static void sink_short(void *ctx, const uint8_t *d, uint8_t len, uint16_t ts)
{
    (void)ctx;
    (void)ts; /* Low Latency Mode: timestamps are parsed but never scheduled */
    midi_uart_send_short(d, len);
}

static void sink_realtime(void *ctx, uint8_t b, uint16_t ts)
{
    (void)ctx;
    (void)ts;
    midi_uart_send_realtime(b);
}

static void sink_sysex(void *ctx, const uint8_t *d, size_t len, uint16_t ts)
{
    (void)ctx;
    (void)ts;
    midi_uart_send_sysex(d, len);
}

/* ---- statistics task (low priority, never touches the MIDI path) ---- */
static unsigned cyc_us100(uint64_t cyc, uint32_t mhz)
{
    return (unsigned)((cyc * 100u) / mhz);
}

static void stats_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));
        if(!serial_console_stats_enabled()) continue;

        latency_stats_t l;
        midi_uart_stats_t u;
        ble_midi_client_stats_t c;
        latency_debug_get_stats(&l, 1);
        midi_uart_get_stats(&u);
        ble_midi_client_get_stats(&c);

        if (l.count) {
            unsigned mn = cyc_us100(l.min_cycles, l.cpu_mhz);
            unsigned mx = cyc_us100(l.max_cycles, l.cpu_mhz);
            unsigned av = cyc_us100(l.sum_cycles / l.count, l.cpu_mhz);
            ESP_LOGI(TAG, "LAT rx->uart (last %us): n=%u min=%u.%02uus avg=%u.%02uus max=%u.%02uus",
                     (unsigned)(STATS_PERIOD_MS / 1000), (unsigned)l.count, mn / 100, mn % 100, av / 100, av % 100,
                     mx / 100, mx % 100);
        } else {
            ESP_LOGI(TAG, "LAT: no MIDI traffic (link %s)", c.connected ? "up" : "down");
        }
        ESP_LOGI(TAG,
                 "BLE: notif=%u dropped=%u conn=%u disc=%u | parser: pkt=%u short=%u rt=%u sysex_chunks=%u "
                 "err[malformed=%u orphan=%u incomplete=%u sysex_abort=%u]",
                 (unsigned)c.notifications, (unsigned)c.notif_dropped, (unsigned)c.connections,
                 (unsigned)c.disconnects, (unsigned)c.parser.packets, (unsigned)c.parser.short_msgs,
                 (unsigned)c.parser.realtime_msgs, (unsigned)c.parser.sysex_chunks,
                 (unsigned)c.parser.err_malformed, (unsigned)c.parser.err_orphan_data,
                 (unsigned)c.parser.err_incomplete, (unsigned)c.parser.err_sysex_abort);
        ESP_LOGI(TAG, "UART: short=%u rt=%u backlog=%u | overflow short=%u rt=%u sysex=%u | heap=%u",
                 (unsigned)u.short_queued, (unsigned)u.rt_queued, (unsigned)u.backlog_events,
                 (unsigned)u.short_overflow, (unsigned)u.rt_overflow, (unsigned)u.sysex_overflow,
                 (unsigned)esp_get_free_heap_size());
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Firmware: %s", FW_VERSION);
    ESP_LOGI(TAG, "ESP-IDF: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "BLE stack: NimBLE (Central), Wi-Fi: not initialised");
    ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned)esp_get_free_heap_size());
    ESP_LOGI(TAG, "MIDI TX GPIO=%d  DEBUG_BLE_RX GPIO=%d  DEBUG_UART_TX GPIO=%d", MIDI_TX_GPIO, DEBUG_BLE_RX_GPIO,
             DEBUG_UART_TX_GPIO);

    latency_debug_init();
    ESP_ERROR_CHECK(midi_uart_init());

    const ble_midi_sink_t sink = {
        .short_msg = sink_short,
        .realtime = sink_realtime,
        .sysex = sink_sysex,
        .ctx = NULL,
    };
    ESP_ERROR_CHECK(ble_midi_client_start(&sink));

    encoder_input_start();
    oled_ui_start();
    serial_console_start();
    xTaskCreate(stats_task, "stats", 3072, NULL, 1, NULL);
}
