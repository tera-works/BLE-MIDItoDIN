#include "midi_uart.h"
#include "config.h"
#include "driver/uart.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "hal/uart_ll.h"
#include "hal/uart_periph.h"
#include "latency_debug.h"
#include "midi_tx_queue.h"

#define TXFIFO_EMPTY_INTR UART_INTR_TXFIFO_EMPTY
#define s_hw UART_LL_GET_HW(MIDI_UART_NUM)
static midi_tx_queue_t s_queue;
static uint32_t s_backlog_events;
static intr_handle_t s_intr;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR drain(void)
{
    uint8_t bytes[3];
    midi_tx_stamp_t stamp;
    for (;;) {
        uint32_t room = uart_ll_get_txfifo_len(s_hw);
        uint8_t n = midi_tx_queue_pop(&s_queue, bytes, room, &stamp);
        if (!n) break;
        latency_debug_uart_write(stamp.cycles, stamp.measure);
        uart_ll_write_txfifo(s_hw, bytes, n);
        latency_debug_uart_write_done();
    }
    if (midi_tx_queue_pending(&s_queue)) {
        uart_ll_clr_intsts_mask(s_hw, TXFIFO_EMPTY_INTR);
        uart_ll_ena_intr_mask(s_hw, TXFIFO_EMPTY_INTR);
    } else {
        uart_ll_disable_intr_mask(s_hw, TXFIFO_EMPTY_INTR);
    }
}

static void IRAM_ATTR uart_isr(void *arg)
{
    (void)arg;
    uint32_t status = uart_ll_get_intsts_mask(s_hw);
    uart_ll_clr_intsts_mask(s_hw, status);
    if (status & TXFIFO_EMPTY_INTR) {
        portENTER_CRITICAL_ISR(&s_lock);
        drain();
        portEXIT_CRITICAL_ISR(&s_lock);
    }
}

static inline __attribute__((always_inline)) midi_tx_stamp_t current_stamp(void)
{
    midi_tx_stamp_t stamp = {0, false};
    stamp.measure = latency_debug_claim_sample(&stamp.cycles);
    return stamp;
}

static inline __attribute__((always_inline)) void kick(void)
{
    drain();
    if (midi_tx_queue_pending(&s_queue)) s_backlog_events++;
}

void IRAM_ATTR midi_uart_send_short(const uint8_t *data, uint8_t len)
{
    if (!data || !len || len > 3) return;
    portENTER_CRITICAL(&s_lock);
    /* Empty backlog: write the whole message without descriptor push/pop. */
    if (midi_tx_queue_can_direct_short(&s_queue, len, uart_ll_get_txfifo_len(s_hw))) {
        midi_tx_stamp_t stamp = current_stamp();
        s_queue.short_queued++;
        latency_debug_uart_write(stamp.cycles, stamp.measure);
        uart_ll_write_txfifo(s_hw, data, len);
        latency_debug_uart_write_done();
    } else {
        midi_tx_queue_short(&s_queue, data, len, current_stamp());
        kick();
    }
    portEXIT_CRITICAL(&s_lock);
}

void IRAM_ATTR midi_uart_send_realtime(uint8_t byte)
{
    portENTER_CRITICAL(&s_lock);
    midi_tx_queue_rt(&s_queue, byte, current_stamp());
    kick();
    portEXIT_CRITICAL(&s_lock);
}

void IRAM_ATTR midi_uart_send_sysex(const uint8_t *data, size_t len)
{
    if (!data || !len) return;
    portENTER_CRITICAL(&s_lock);
    midi_tx_queue_sysex(&s_queue, data, len, current_stamp());
    kick();
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t midi_uart_init(void)
{
    midi_tx_queue_init(&s_queue);
    const uart_config_t cfg = {
        .baud_rate = MIDI_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(MIDI_UART_NUM, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(MIDI_UART_NUM, MIDI_TX_GPIO, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    uart_ll_disable_intr_mask(s_hw, UINT32_MAX);
    uart_ll_clr_intsts_mask(s_hw, UINT32_MAX);
    uart_ll_set_txfifo_empty_thr(s_hw, MIDI_TXFIFO_EMPTY_THR);
    return esp_intr_alloc(uart_periph_signal[MIDI_UART_NUM].irq,
                          ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL2, uart_isr, NULL, &s_intr);
}

void midi_uart_get_stats(midi_uart_stats_t *out)
{
    portENTER_CRITICAL(&s_lock);
    out->short_queued = s_queue.short_queued;
    out->rt_queued = s_queue.rt_queued;
    out->short_overflow = s_queue.short_overflow;
    out->rt_overflow = s_queue.rt_overflow;
    out->sysex_overflow = s_queue.sysex_overflow;
    out->backlog_events = s_backlog_events;
    portEXIT_CRITICAL(&s_lock);
}

bool midi_uart_is_idle(void)
{
    portENTER_CRITICAL(&s_lock);
    bool idle = !midi_tx_queue_pending(&s_queue) && uart_ll_is_tx_idle(s_hw);
    portEXIT_CRITICAL(&s_lock);
    return idle;
}
