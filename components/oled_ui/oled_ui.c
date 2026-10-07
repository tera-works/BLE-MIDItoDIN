#include "oled_ui.h"
#include "oled_frame.h"
#include "oled_transfer.h"
#include "esp_cpu.h"
#include "config.h"
#include "ble_midi_client.h"
#include "midi_uart.h"
#include "latency_debug.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#if OLED_ENABLED
static const char *TAG = "OLED";
static oled_frame_t s_frame, s_sent;
static uint8_t s_valid[OLED_PAGES];
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;


_Static_assert(CONFIG_FREERTOS_UNICORE, "OLED RX quiet guard uses the host core cycle counter");
_Static_assert(OLED_QUIET_MS > 0 && OLED_QUIET_MS <= 1000, "bounded quiet interval");
_Static_assert(OLED_CHUNK_GAP_MS >= 1, "yield between chunks");
static bool transport_quiet(void)
{
    const uint32_t quiet = OLED_QUIET_MS * 1000u * CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    uint32_t last = latency_debug_last_rx_cycles();
    uint32_t now = esp_cpu_get_cycle_count();
    /* Active RX: avoid even taking the MIDI UART lock from the UI task. */
    if (!oled_transport_quiet(now,last,quiet,true)) return false;
    bool idle = midi_uart_is_idle();
    /* A notification can preempt the UART check: refresh the timestamp afterwards. */
    last = latency_debug_last_rx_cycles();
    now = esp_cpu_get_cycle_count();
    return oled_transport_quiet(now,last,quiet,idle);
}
static void wait_quiet(void)
{
    while (!transport_quiet()) vTaskDelay(pdMS_TO_TICKS(10));
}

static esp_err_t display_init(void)
{
    const i2c_master_bus_config_t bus = {
        .i2c_port = -1, .sda_io_num = OLED_SDA_GPIO, .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .intr_priority = 1, .flags.enable_internal_pullup = true,
    };
    wait_quiet();
    esp_err_t err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) return err;
    /* Probe once. Missing display never prevents BLE/MIDI startup. */
    wait_quiet();
    err = i2c_master_probe(s_bus, OLED_I2C_ADDRESS, OLED_IO_TIMEOUT_MS);
    if (err != ESP_OK) { i2c_del_master_bus(s_bus); s_bus=NULL; return err; }
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = OLED_I2C_ADDRESS,
        .scl_speed_hz = OLED_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev, &s_dev);
    if (err == ESP_OK) {
        const uint8_t init[] = {0x00,0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,
            0x8D,0x14,0x20,0x02,0xA1,0xC8,0xDA,0x12,0x81,0x7F,
            0xD9,0xF1,0xDB,0x40,0xA4,0xA6,0x2E};
        wait_quiet();
        err = i2c_master_transmit(s_dev, init, sizeof(init), OLED_IO_TIMEOUT_MS);
    }
    return err;
}

static void render(const ble_midi_client_stats_t *c)
{
    char line[48];
    oled_frame_clear(s_frame);
    if(c->ui_delete_confirm) {
        oled_frame_text(s_frame,0,"REMOVE DEVICE?");
        oled_frame_text(s_frame,2,c->ui_delete_name[0]?c->ui_delete_name:"Unknown MIDI");
        oled_frame_text(s_frame,4,c->ui_delete_yes?"  Cancel":" >Cancel");
        oled_frame_text(s_frame,5,c->ui_delete_yes?" >Remove":"  Remove");
        oled_frame_text(s_frame,6,c->ui_message[0]?c->ui_message:"Push: Confirm");
        oled_frame_text(s_frame,7,"Hold: Cancel");
        return;
    }
    oled_frame_text(s_frame,0,c->ui_scan?"SCAN BLE MIDI":"BLE MIDI \x1f DIN");
    static const char status[]={0x11,0x10,0x12,'!'};
    for(unsigned i=0;i<c->ui_rows && i<3;i++) {
        if(c->ui_scan) snprintf(line,sizeof line,"%c %-14.14s %d",i==c->ui_selected?'>':' ',c->ui_names[i],c->ui_rssi[i]);
        else snprintf(line,sizeof line,"%c%c %.18s",i==c->ui_selected?'>':' ',status[c->ui_status[i]<4?c->ui_status[i]:3],c->ui_names[i]);
        oled_frame_text(s_frame,2+i,line);
    }
    if(!c->ui_rows) oled_frame_text(s_frame,2,c->ui_scan?"Searching...":"No registered device");
    if(c->ui_message[0]) oled_frame_text(s_frame,5,c->ui_message);
    else if(c->ui_saving) oled_frame_text(s_frame,5,"Saving...");
    if(c->ui_scan) {
        oled_frame_text(s_frame,6,"Push: Add");
        oled_frame_text(s_frame,7,"Hold: Exit");
    } else {
        snprintf(line,sizeof line,"OUT: OK       %u/%u",c->ui_connected,c->ui_total);
        oled_frame_text(s_frame,7,line);
    }
}

/* One packet fits in ESP32's 32-byte I2C FIFO including the slave address.
 * Avoid the refill stages of a 128-byte page write. No async transfers queued. */
static esp_err_t flush_chunk(unsigned page, unsigned first, unsigned length)
{
    uint8_t packet[OLED_PACKET_BYTES];
    size_t n = oled_make_packet(packet, page, first, s_frame[page]+first, length);
    if (!n) return ESP_ERR_INVALID_ARG;
    esp_err_t err = i2c_master_transmit(s_dev, packet, n, OLED_IO_TIMEOUT_MS);
    if (err == ESP_OK) oled_commit_chunk(s_sent[page], &s_valid[page], first, s_frame[page]+first, length);
    return err;
}

static bool find_chunk(unsigned *page, unsigned *first, unsigned *length)
{
    /* Device rows and footer first; static title last. */
    static const uint8_t order[OLED_PAGES]={2,3,4,7,0,1,5,6};
    for (unsigned i=0; i<OLED_PAGES; i++) {
        *page=order[i];
        if (oled_next_chunk(s_frame[*page], s_sent[*page], s_valid[*page], first, length)) return true;
    }
    return false;
}

static void ui_task(void *arg)
{
    (void)arg;
    esp_err_t err = display_init();
    if (err == ESP_OK) {
        bool turned_on = false, pending = false;
        for (;;) {
            ble_midi_client_stats_t state;
            /* Completely asleep without a dirty state; poll only during a pending redraw. */
            if (ble_midi_client_take_ui(&state, pending ? 0 : UINT32_MAX)) {
                render(&state);
                pending=true;
            }
            if (!pending) continue;
            /* Re-check before EVERY transfer, including first display-on. */
            if (!transport_quiet()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            /* A newer state can have arrived while MIDI delayed this update. */
            if (ble_midi_client_take_ui(&state, 0)) render(&state);
            unsigned page, first, length;
            if (find_chunk(&page, &first, &length)) {
                if (!transport_quiet()) continue;
                err=flush_chunk(page,first,length);
                if (err != ESP_OK) break;
                vTaskDelay(pdMS_TO_TICKS(OLED_CHUNK_GAP_MS));
                continue; /* Always yield, even when control events arrive continuously. */
            }
            if (!turned_on) {
                if (!transport_quiet()) continue;
                const uint8_t on[] = {0x00,0xAF};
                err = i2c_master_transmit(s_dev, on, sizeof(on), OLED_IO_TIMEOUT_MS);
                if (err != ESP_OK) break;
                turned_on = true;
                ESP_LOGI(TAG, "SSD1306 event UI enabled: %ubyte chunks, %ums quiet guard",
                         OLED_CHUNK_BYTES, OLED_QUIET_MS);
            }
            pending=false;
        }
    }
    ESP_LOGW(TAG, "Display unavailable (%s); MIDI continues. Reboot to retry.", esp_err_to_name(err));
    if (s_dev) i2c_master_bus_rm_device(s_dev);
    if (s_bus) i2c_del_master_bus(s_bus);
    vTaskDelete(NULL);
}
#endif

void oled_ui_start(void)
{
#if OLED_ENABLED
    if (xTaskCreate(ui_task, "oled", 4096, NULL, 1, NULL) != pdPASS)
        ESP_LOGW(TAG, "UI task allocation failed; MIDI continues");
#endif
}
