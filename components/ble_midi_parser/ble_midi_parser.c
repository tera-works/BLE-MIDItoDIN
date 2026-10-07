#include "ble_midi_parser.h"

#include <string.h>

static const uint8_t k_sysex_end = 0xF7;

void ble_midi_parser_init(ble_midi_parser_t *p, const ble_midi_sink_t *sink)
{
    memset(p, 0, sizeof(*p));
    p->sink = *sink;
}

void ble_midi_parser_reset(ble_midi_parser_t *p)
{
    if (p->in_sysex) {
        p->stats.err_sysex_abort++;
        if (p->sink.sysex) {
            p->sink.sysex(p->sink.ctx, &k_sysex_end, 1, 0);
        }
    }
    p->in_sysex = false;
    p->status = 0;
    p->need = 0;
    p->count = 0;
}

static inline void emit_short(ble_midi_parser_t *p, const uint8_t *d, uint8_t len, uint16_t ts)
{
    p->stats.short_msgs++;
    if (p->sink.short_msg) {
        p->sink.short_msg(p->sink.ctx, d, len, ts);
    }
}

static inline void emit_sysex(ble_midi_parser_t *p, const uint8_t *d, size_t len, uint16_t ts)
{
    p->stats.sysex_chunks++;
    if (p->sink.sysex) {
        p->sink.sysex(p->sink.ctx, d, len, ts);
    }
}

void ble_midi_parser_feed(ble_midi_parser_t *p, const uint8_t *pkt, size_t len)
{
    if (pkt == NULL || len < 2 || (pkt[0] & 0xC0) != 0x80) {
        /* header must be 10tttttt */
        p->stats.err_malformed++;
        ble_midi_parser_reset(p); /* never leave a transmitted SysEx open after loss */
        return;
    }
    p->stats.packets++;

    uint16_t ts_hi = (uint16_t)(pkt[0] & 0x3F) << 7;
    uint16_t ts = ts_hi;
    uint16_t msg_ts = ts;
    bool got_ts = false;
    bool have_message_ts = false;
    bool have_ts = false;
    uint8_t last_ts_low = 0;

    /* Messages are never split across packets (except SysEx): drop partial. */
    if (p->count > 0) {
        p->stats.err_incomplete++;
        p->count = 0;
    }
    /* BLE running status is local to a notification; only SysEx spans it. */
    p->status = 0;
    p->need = 0;
    if (!(pkt[1] & 0x80) && !p->in_sysex) {
        p->stats.err_malformed++;
        return;
    }

    /* contiguous run of SysEx bytes inside this packet (zero copy) */
    const uint8_t *run = NULL;
    size_t run_len = 0;

#define FLUSH_RUN()                                  \
    do {                                             \
        if (run_len) {                               \
            emit_sysex(p, run, run_len, ts);         \
            run_len = 0;                             \
        }                                            \
    } while (0)

    for (size_t i = 1; i < len; i++) {
        const uint8_t b = pkt[i];

        if (b & 0x80) {
            if (!got_ts) {
                /* timestamp low byte */
                FLUSH_RUN();
                const uint8_t low = b & 0x7F;
                if (have_ts && low < last_ts_low) {
                    ts_hi = (ts_hi + 0x80) & 0x1FFF;
                }
                last_ts_low = low;
                have_ts = true;
                ts = ts_hi | low;
                got_ts = true;
                continue;
            }
            got_ts = false;

            if (b >= 0xF8) { /* realtime: may interrupt anything */
                FLUSH_RUN();
                if (b == 0xF9 || b == 0xFD) {
                    p->stats.err_malformed++;
                    continue;
                }
                p->stats.realtime_msgs++;
                if (p->sink.realtime) {
                    p->sink.realtime(p->sink.ctx, b, ts);
                }
                continue;
            }

            if (b == 0xF7) {
                FLUSH_RUN();
                if (p->in_sysex) {
                    emit_sysex(p, &pkt[i], 1, ts);
                    p->in_sysex = false;
                } else {
                    p->stats.err_orphan_data++;
                }
                p->status = 0;
                p->need = 0;
                p->count = 0;
                continue;
            }

            /* any other status terminates an open SysEx */
            if (p->in_sysex) {
                FLUSH_RUN();
                p->stats.err_sysex_abort++;
                emit_sysex(p, &k_sysex_end, 1, ts);
                p->in_sysex = false;
            }
            if (p->count > 0) {
                p->stats.err_incomplete++;
                p->count = 0;
            }

            if (b == 0xF0) {
                p->in_sysex = true;
                p->status = 0;
                p->need = 0;
                run = &pkt[i];
                run_len = 1;
            } else if (b == 0xF6) { /* Tune Request */
                p->status = 0;
                p->need = 0;
                emit_short(p, &pkt[i], 1, ts);
            } else if (b == 0xF4 || b == 0xF5) { /* undefined */
                p->status = 0;
                p->need = 0;
                p->stats.err_malformed++;
            } else {
                p->status = b;
                p->count = 0;
                msg_ts = ts;
                have_message_ts = true;
                if (b == 0xF1 || b == 0xF3 || (b >= 0xC0 && b <= 0xDF)) {
                    p->need = 1;
                } else { /* F2, 80-BF, E0-EF */
                    p->need = 2;
                }
            }
        } else {
            /* data byte */
            got_ts = false;

            if (p->in_sysex) {
                if (run_len == 0) {
                    run = &pkt[i];
                }
                run_len++;
                continue;
            }
            if (p->status == 0 || p->need == 0) {
                p->stats.err_orphan_data++;
                continue;
            }
            if (!have_message_ts) {
                msg_ts = ts;
                have_message_ts = true;
            }
            p->data[p->count++] = b;
            if (p->count >= p->need) {
                uint8_t msg[3];
                msg[0] = p->status;
                msg[1] = p->data[0];
                msg[2] = (p->need == 2) ? p->data[1] : 0;
                emit_short(p, msg, (uint8_t)(p->need + 1), msg_ts);
                p->count = 0;
                have_message_ts = false;
                if (p->status >= 0xF0) { /* system common: no running status */
                    p->status = 0;
                    p->need = 0;
                }
            }
        }
    }
    FLUSH_RUN();
#undef FLUSH_RUN

    if (p->count > 0) {
        p->stats.err_incomplete++;
        p->count = 0;
    }
    if (got_ts) {
        p->stats.err_malformed++;
    }
    p->status = 0;
    p->need = 0;
}
