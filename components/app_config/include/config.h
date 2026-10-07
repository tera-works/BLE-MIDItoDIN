/*
 * config.h - central place for pin assignments and tunables.
 * Every value may be overridden with -D at compile time (#ifndef guards).
 */
#pragma once

#include "sdkconfig.h"
#define FW_VERSION "0.5.0-serial-console"

/* ---- DIN MIDI OUT ------------------------------------------------------ */
#ifndef MIDI_UART_NUM
#define MIDI_UART_NUM       1      /* UART_NUM_1 (UART0 is the console) */
#endif
#ifndef MIDI_TX_GPIO
#define MIDI_TX_GPIO        17
#endif
#define MIDI_BAUD           31250

/* ---- Latency debug pins (logic analyser) --------------------------------- */
#ifndef DEBUG_BLE_RX_GPIO
#define DEBUG_BLE_RX_GPIO   25
/* HIGH on entry of GATT notification callback */
#endif
#ifndef DEBUG_UART_TX_GPIO
#define DEBUG_UART_TX_GPIO  26
/* HIGH right before first byte goes to the UART FIFO */
#endif

/* ---- Output buffers (all static, fixed size) ------------------------------ */
#define MIDI_SHORT_RING_LEN     256     /* ordered event descriptors, power of two */
#define MIDI_RT_RING_LEN        128     /* realtime bytes,      power of two */
#define MIDI_SYSEX_RING_LEN     4096    /* sysex bytes,         power of two */
#define MIDI_TXFIFO_EMPTY_THR   16      /* ISR fires when FIFO level <= this */

/* ---- BLE connection ------------------------------------------------------- */
#define BLE_MIDI_MAX_CONN       4       /* Registered BLE MIDI inputs */
/* units: 1.25 ms */
#define BLE_CONN_ITVL_MIN       6       /* 7.5 ms */
#define BLE_CONN_ITVL_MAX       6       /* 7.5 ms */
#define BLE_CONN_LATENCY        0
/* units: 10 ms */
#ifndef BLE_CONN_SUP_TIMEOUT
#define BLE_CONN_SUP_TIMEOUT    100     /* 1 s; verify RF stability on hardware */
#endif
#define BLE_FAST_SCAN_UNITS      16      /* 10ms in NimBLE's 0.625ms units */
#define BLE_DIRECT_TIMEOUT_MS    1000
#define BLE_KNOWN_SCAN_MS        5000

#define STATS_PERIOD_MS         5000

/* SSD1306 128x64. UI is optional and never called by a MIDI sink. */
#ifndef OLED_ENABLED
#define OLED_ENABLED 1
#endif
#ifndef OLED_SDA_GPIO
#define OLED_SDA_GPIO 21
#endif
#ifndef OLED_SCL_GPIO
#define OLED_SCL_GPIO 22
#endif
#ifndef OLED_I2C_ADDRESS
#define OLED_I2C_ADDRESS 0x3C
#endif
#ifndef OLED_I2C_HZ
#define OLED_I2C_HZ 400000
#endif

/* Defer all OLED I2C until notification activity has been quiet and TX drained. */
#ifndef OLED_QUIET_MS
#define OLED_QUIET_MS 50
#endif
#ifndef OLED_CHUNK_GAP_MS
#define OLED_CHUNK_GAP_MS 2
#endif
#ifndef OLED_IO_TIMEOUT_MS
#define OLED_IO_TIMEOUT_MS 5
#endif

/* Mechanical encoder: common terminals to GND, internal pull-ups. */
#ifndef ENCODER_A_GPIO
#define ENCODER_A_GPIO 32
#endif
#ifndef ENCODER_B_GPIO
#define ENCODER_B_GPIO 33
#endif
#ifndef ENCODER_BUTTON_GPIO
#define ENCODER_BUTTON_GPIO 27
#endif
