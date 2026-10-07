/*
 * ble_midi_client - NimBLE based BLE-MIDI Central.
 *
 * scan -> connect (7.5 ms interval requested) -> MTU -> discover service /
 * characteristic / CCCD -> enable notifications -> parse in the notification
 * callback and hand events to the sink synchronously.
 *
 * Connection state lives in per-connection slots so that more than one
 * device can be supported later (BLE_MIDI_MAX_CONN in config.h).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_midi_parser.h"
#include "serial_command.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_MIDI_BOOT, BLE_MIDI_SCANNING, BLE_MIDI_CONNECTING, BLE_MIDI_DISCOVERING,
    BLE_MIDI_SUBSCRIBING, BLE_MIDI_READY, BLE_MIDI_DISCONNECTED, BLE_MIDI_RETRYING
} ble_midi_link_state_t;

typedef struct {
    /* Event-only OLED summary: current peer first, then MIDI peers seen in scan. */
    char ui_names[3][32];
    uint8_t ui_status[3]; /* 0 registered/offline, 1 ready, 2 connecting, 3 error */
    int8_t ui_rssi[3];
    uint8_t ui_selected;
    bool ui_scan, ui_saving, ui_delete_confirm, ui_delete_yes;
    char ui_delete_name[32];
    char ui_message[24];
    uint8_t ui_rows, ui_connected, ui_total;
    ble_midi_link_state_t state;
    char peer_name[32];
    char peer_addr[18];
    int8_t rssi; /* RSSI at discovery, not a continuous link measurement */
    uint16_t interval_units; /* actual interval, 1.25ms units */
    uint16_t mtu;
    bool connected;           /* notifications enabled on at least one slot */
    uint32_t notifications;   /* GATT notifications received                */
    uint32_t notif_dropped;   /* too long / unknown handle                  */
    uint32_t connections;     /* successful setups since boot               */
    uint32_t disconnects;
    ble_midi_parser_stats_t parser; /* slot 0 */
} ble_midi_client_stats_t;

typedef struct {
 char name[32],addr[18];uint8_t state;bool disabled;int8_t rssi;
 uint16_t interval_units,mtu;
} midi_serial_peer_t;
typedef struct {
 bool ok,scan, saving,remove_pending;char message[96],remove_name[32];
 uint8_t registered_count,found_count;
 midi_serial_peer_t registered[4],found[24];
 ble_midi_client_stats_t stats;
} midi_serial_response_t;
/* Single console caller. Host queue owns every state read/mutation; no stdout there. */
bool ble_midi_client_serial(const midi_serial_command_t *command,midi_serial_response_t *response);

/* Starts NimBLE, scanning and auto (re)connect. The sink is called from the
 * NimBLE host task, inside the GATT notification callback. */
esp_err_t ble_midi_client_start(const ble_midi_sink_t *sink);

/* Enqueue input action; BLE work is executed by the host task. */
void ble_midi_client_control(int action);

void ble_midi_client_get_stats(ble_midi_client_stats_t *out);
/* Single UI observer. Blocks until a control-plane state event, never MIDI data.
 * Latest state wins if the display is slow. No BLE operation waits for the UI. */
void ble_midi_client_wait_ui(ble_midi_client_stats_t *out);
/* UINT32_MAX waits forever; zero polls. UI only, never used for MIDI data. */
bool ble_midi_client_take_ui(ble_midi_client_stats_t *out, uint32_t wait_ms);

#ifdef __cplusplus
}
#endif
