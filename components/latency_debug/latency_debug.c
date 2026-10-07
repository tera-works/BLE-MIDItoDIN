#include "latency_debug.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "hal/gpio_ll.h"
#include "sdkconfig.h"
#include "soc/gpio_struct.h"

static uint32_t s_t0;
static bool s_armed;
static latency_stats_t s_stats;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void latency_debug_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << DEBUG_BLE_RX_GPIO) | (1ULL << DEBUG_UART_TX_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(DEBUG_BLE_RX_GPIO, 0);
    gpio_set_level(DEBUG_UART_TX_GPIO, 0);
    s_stats.min_cycles = UINT32_MAX;
    s_stats.cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
}

void IRAM_ATTR latency_debug_rx_enter(void)
{
    gpio_ll_set_level(&GPIO, DEBUG_BLE_RX_GPIO, 1);
    __atomic_store_n(&s_t0, esp_cpu_get_cycle_count(), __ATOMIC_RELAXED);
    s_armed = true;
}

void IRAM_ATTR latency_debug_rx_exit(void)
{
    gpio_ll_set_level(&GPIO, DEBUG_BLE_RX_GPIO, 0);
    s_armed = false;
}

bool IRAM_ATTR latency_debug_claim_sample(uint32_t *cycles)
{
    if (!s_armed) return false;
    *cycles = __atomic_load_n(&s_t0, __ATOMIC_RELAXED);
    s_armed = false;
    return true;
}

void IRAM_ATTR latency_debug_uart_write(uint32_t started, bool measure)
{
    if (!measure) return;
    gpio_ll_set_level(&GPIO, DEBUG_UART_TX_GPIO, 1);
    uint32_t dt = esp_cpu_get_cycle_count() - started;
    portENTER_CRITICAL_SAFE(&s_lock);
    s_stats.count++;
    s_stats.sum_cycles += dt;
    if (dt < s_stats.min_cycles) s_stats.min_cycles = dt;
    if (dt > s_stats.max_cycles) s_stats.max_cycles = dt;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

void IRAM_ATTR latency_debug_uart_write_done(void)
{
    gpio_ll_set_level(&GPIO, DEBUG_UART_TX_GPIO, 0);
}

void latency_debug_get_stats(latency_stats_t *out, int reset)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    if (reset) {
        s_stats.count = 0;
        s_stats.sum_cycles = 0;
        s_stats.max_cycles = 0;
        s_stats.min_cycles = UINT32_MAX;
    }
    portEXIT_CRITICAL(&s_lock);
}

uint32_t latency_debug_last_rx_cycles(void)
{
    return __atomic_load_n(&s_t0, __ATOMIC_RELAXED);
}
