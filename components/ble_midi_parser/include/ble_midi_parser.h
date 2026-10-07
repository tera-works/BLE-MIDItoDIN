/*
 * ble_midi_parser - BLE-MIDI packet parser (no dependencies, host testable).
 *
 * Packet layout (BLE-MIDI 1.0):
 *   [header: 1 0 tttttt] { [timestamp: 1 ttttttt] [status] [data...] }*
 * A timestamp byte always precedes a status byte (including realtime and
 * F7).  Data bytes without status use running status.  SysEx may continue
 * in the next packet: such a packet starts with the header followed directly
 * by SysEx data bytes.
 *
 * The parser is completely allocation free and never waits: events are
 * delivered to the sink synchronously from ble_midi_parser_feed().
 * The timestamp is parsed and handed to the sink but must NOT be used to
 * delay output in low-latency mode.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Complete channel voice / system common message, len = 1..3 (status first). */
    void (*short_msg)(void *ctx, const uint8_t *d, uint8_t len, uint16_t ts);
    /* Single realtime byte (0xF8..0xFF). Can occur between bytes of any message. */
    void (*realtime)(void *ctx, uint8_t b, uint16_t ts);
    /* Chunk of SysEx bytes. First chunk starts with F0, final chunk ends with F7.
     * An aborted SysEx is closed with a lone F7 chunk. */
    void (*sysex)(void *ctx, const uint8_t *d, size_t len, uint16_t ts);
    void *ctx;
} ble_midi_sink_t;

typedef struct {
    uint32_t packets;
    uint32_t short_msgs;
    uint32_t realtime_msgs;
    uint32_t sysex_chunks;
    uint32_t err_malformed;   /* bad header, undefined status, empty packet */
    uint32_t err_orphan_data; /* data byte without status / stray F7        */
    uint32_t err_incomplete;  /* message cut by new status or packet end    */
    uint32_t err_sysex_abort; /* SysEx terminated without F7                */
} ble_midi_parser_stats_t;

typedef struct {
    ble_midi_sink_t sink;
    uint8_t status;  /* running status within the current packet only */
    uint8_t need;    /* data bytes needed by status                           */
    uint8_t count;   /* data bytes collected                                  */
    uint8_t data[2];
    bool in_sysex;
    ble_midi_parser_stats_t stats;
} ble_midi_parser_t;

void ble_midi_parser_init(ble_midi_parser_t *p, const ble_midi_sink_t *sink);

/* Drop all state (e.g. on disconnect). Closes an open SysEx with F7. Stats are kept. */
void ble_midi_parser_reset(ble_midi_parser_t *p);

/* Parse one GATT notification. Valid prefix events are streamed immediately;
 * incomplete messages are discarded, and only SysEx survives packet boundaries. */
void ble_midi_parser_feed(ble_midi_parser_t *p, const uint8_t *pkt, size_t len);

#ifdef __cplusplus
}
#endif
