/*
 * Host unit tests for ble_midi_parser and midi_ring.
 * Build & run: see Makefile / CMakeLists.txt in this directory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_midi_parser.h"
#include "midi_ring.h"

/* ---------- tiny test framework ---------- */
static int g_fail, g_checks;
#define CHECK(c)                                                         \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(c)) {                                                      \
            g_fail++;                                                    \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                                \
    } while (0)

/* ---------- recording sink ---------- */
typedef struct {
    enum { EV_SHORT, EV_RT } type;
    uint8_t len;
    uint8_t b[3];
    uint16_t ts;
} rec_ev_t;

typedef struct {
    rec_ev_t ev[64];
    int n;
    uint8_t sx[8192];
    size_t sx_len;
    int sx_chunks;
} rec_t;

static void r_short(void *c, const uint8_t *d, uint8_t len, uint16_t ts)
{
    rec_t *r = c;
    rec_ev_t *e = &r->ev[r->n++];
    e->type = EV_SHORT;
    e->len = len;
    e->ts = ts;
    memcpy(e->b, d, len);
}
static void r_rt(void *c, uint8_t b, uint16_t ts)
{
    rec_t *r = c;
    rec_ev_t *e = &r->ev[r->n++];
    e->type = EV_RT;
    e->len = 1;
    e->ts = ts;
    e->b[0] = b;
}
static void r_sx(void *c, const uint8_t *d, size_t len, uint16_t ts)
{
    (void)ts;
    rec_t *r = c;
    memcpy(r->sx + r->sx_len, d, len);
    r->sx_len += len;
    r->sx_chunks++;
}

static rec_t R;
static ble_midi_parser_t P;

static void setup(void)
{
    memset(&R, 0, sizeof(R));
    ble_midi_sink_t s = {r_short, r_rt, r_sx, &R};
    ble_midi_parser_init(&P, &s);
}
#define FEED(...)                                      \
    do {                                               \
        static const uint8_t pkt_[] = {__VA_ARGS__};   \
        ble_midi_parser_feed(&P, pkt_, sizeof(pkt_));  \
    } while (0)

static int is_short(int i, uint8_t len, uint8_t a, uint8_t b, uint8_t c)
{
    if (i >= R.n || R.ev[i].type != EV_SHORT || R.ev[i].len != len) return 0;
    if (R.ev[i].b[0] != a) return 0;
    if (len > 1 && R.ev[i].b[1] != b) return 0;
    if (len > 2 && R.ev[i].b[2] != c) return 0;
    return 1;
}
static int is_rt(int i, uint8_t b)
{
    return i < R.n && R.ev[i].type == EV_RT && R.ev[i].b[0] == b;
}
static int sx_is(const uint8_t *e, size_t n)
{
    return R.sx_len == n && memcmp(R.sx, e, n) == 0;
}
static int no_errors(void)
{
    const ble_midi_parser_stats_t *s = &P.stats;
    return !(s->err_malformed || s->err_orphan_data || s->err_incomplete || s->err_sysex_abort);
}

/* ---------- tests ---------- */
static void t01_note_on(void)
{
    setup();
    FEED(0x80, 0x80, 0x90, 0x3C, 0x7F);
    CHECK(R.n == 1 && is_short(0, 3, 0x90, 0x3C, 0x7F) && no_errors());
}
static void t02_note_off(void)
{
    setup();
    FEED(0xBF, 0xFF, 0x80, 0x3C, 0x40); /* max timestamp bits */
    CHECK(R.n == 1 && is_short(0, 3, 0x80, 0x3C, 0x40) && no_errors());
}
static void t03_cc(void)
{
    setup();
    FEED(0x80, 0x81, 0xB3, 0x07, 0x64);
    CHECK(R.n == 1 && is_short(0, 3, 0xB3, 0x07, 0x64));
}
static void t04_pc_and_pressure(void)
{
    setup();
    FEED(0x80, 0x81, 0xC2, 0x05, 0x82, 0xD1, 0x30, 0x83, 0xA0, 0x3C, 0x20);
    CHECK(R.n == 3);
    CHECK(is_short(0, 2, 0xC2, 0x05, 0));
    CHECK(is_short(1, 2, 0xD1, 0x30, 0));
    CHECK(is_short(2, 3, 0xA0, 0x3C, 0x20));
    CHECK(no_errors());
}
static void t05_pitch_bend(void)
{
    setup();
    FEED(0x80, 0x81, 0xE0, 0x00, 0x40);
    CHECK(R.n == 1 && is_short(0, 3, 0xE0, 0x00, 0x40));
}
static void t06_running_status(void)
{
    setup();
    /* data bytes right after message, and after a timestamp only */
    FEED(0x80, 0x81, 0x90, 0x3C, 0x7F, 0x3E, 0x70, 0x82, 0x40, 0x60);
    CHECK(R.n == 3);
    CHECK(is_short(0, 3, 0x90, 0x3C, 0x7F));
    CHECK(is_short(1, 3, 0x90, 0x3E, 0x70));
    CHECK(is_short(2, 3, 0x90, 0x40, 0x60));
    CHECK(no_errors());
    /* BLE running status must not survive into the next packet. */
    FEED(0x80, 0x81, 0x41, 0x50);
    CHECK(R.n == 3 && P.stats.err_orphan_data == 2);
    /* program change running status (1 data byte) */
    FEED(0x80, 0x81, 0xC0, 0x01, 0x02);
    CHECK(R.n == 5 && is_short(3, 2, 0xC0, 0x01, 0) && is_short(4, 2, 0xC0, 0x02, 0));
}
static void t07_system_common_and_realtime(void)
{
    setup();
    FEED(0x80, 0x81, 0xF8, 0x82, 0xFA, 0x83, 0xFB, 0x84, 0xFC, 0x85, 0xFE, 0x86, 0xFF);
    CHECK(R.n == 6);
    CHECK(is_rt(0, 0xF8) && is_rt(1, 0xFA) && is_rt(2, 0xFB) && is_rt(3, 0xFC) && is_rt(4, 0xFE) && is_rt(5, 0xFF));
    setup();
    FEED(0x80, 0x81, 0xF1, 0x25, 0x82, 0xF2, 0x10, 0x20, 0x83, 0xF3, 0x07, 0x84, 0xF6);
    CHECK(R.n == 4);
    CHECK(is_short(0, 2, 0xF1, 0x25, 0));
    CHECK(is_short(1, 3, 0xF2, 0x10, 0x20));
    CHECK(is_short(2, 2, 0xF3, 0x07, 0));
    CHECK(is_short(3, 1, 0xF6, 0, 0));
    CHECK(no_errors());
    /* system common cancels running status */
    setup();
    FEED(0x80, 0x81, 0x90, 0x3C, 0x7F, 0x82, 0xF3, 0x01, 0x10);
    CHECK(R.n == 2 && P.stats.err_orphan_data == 1);
}
static void t08_realtime_inside_message(void)
{
    setup();
    FEED(0x80, 0x81, 0x90, 0x3C, 0x82, 0xF8, 0x7F);
    CHECK(R.n == 2);
    CHECK(is_rt(0, 0xF8));
    CHECK(is_short(1, 3, 0x90, 0x3C, 0x7F));
    CHECK(no_errors());
    /* between the two data bytes of a running-status message too */
    setup();
    FEED(0x80, 0x81, 0xB0, 0x07, 0x64, 0x82, 0x0A, 0x83, 0xFE, 0x20);
    CHECK(R.n == 3 && is_short(0, 3, 0xB0, 0x07, 0x64) && is_rt(1, 0xFE) && is_short(2, 3, 0xB0, 0x0A, 0x20));
}
static void t09_sysex(void)
{
    static const uint8_t exp[] = {0xF0, 0x7D, 0x01, 0x02, 0xF7};
    setup();
    FEED(0x80, 0x81, 0xF0, 0x7D, 0x01, 0x02, 0x82, 0xF7);
    CHECK(sx_is(exp, sizeof exp) && R.n == 0 && no_errors());
}
static void t10_sysex_across_packets(void)
{
    static const uint8_t exp[] = {0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0xF7};
    setup();
    FEED(0x80, 0x81, 0xF0, 0x01, 0x02);
    CHECK(P.in_sysex);
    FEED(0x80, 0x03, 0x04); /* continuation: header + data */
    CHECK(P.in_sysex);
    FEED(0x80, 0x05, 0x82, 0xF7);
    CHECK(!P.in_sysex && sx_is(exp, sizeof exp) && no_errors());
    /* realtime inside SysEx */
    static const uint8_t exp2[] = {0xF0, 0x01, 0x02, 0xF7};
    setup();
    FEED(0x80, 0x81, 0xF0, 0x01, 0x82, 0xF8, 0x02, 0x83, 0xF7);
    CHECK(R.n == 1 && is_rt(0, 0xF8) && sx_is(exp2, sizeof exp2) && no_errors());
    /* short message after SysEx in same packet */
    setup();
    FEED(0x80, 0x81, 0xF0, 0x01, 0x82, 0xF7, 0x83, 0x90, 0x3C, 0x7F);
    CHECK(R.n == 1 && is_short(0, 3, 0x90, 0x3C, 0x7F) && no_errors());
}
static void t11_multiple_events(void)
{
    setup();
    FEED(0x80, 0x81, 0x90, 0x3C, 0x7F, 0x82, 0x80, 0x3C, 0x00, 0x83, 0xB0, 0x07, 0x64, 0x84, 0xF8, 0x85, 0xC1, 0x09);
    CHECK(R.n == 5);
    CHECK(is_short(0, 3, 0x90, 0x3C, 0x7F));
    CHECK(is_short(1, 3, 0x80, 0x3C, 0x00));
    CHECK(is_short(2, 3, 0xB0, 0x07, 0x64));
    CHECK(is_rt(3, 0xF8));
    CHECK(is_short(4, 2, 0xC1, 0x09, 0));
    CHECK(P.stats.packets == 1 && no_errors());
}
static void t12_malformed(void)
{
    setup();
    FEED(0x00, 0x90, 0x3C, 0x7F); /* bad header */
    FEED(0xC0, 0x81, 0x90, 0x3C, 0x7F); /* header bit6 set */
    ble_midi_parser_feed(&P, (const uint8_t *)"", 0);
    ble_midi_parser_feed(&P, NULL, 5);
    CHECK(R.n == 0 && P.stats.err_malformed == 4);

    setup();
    FEED(0x80);
    FEED(0x80, 0x81);
    CHECK(R.n == 0 && P.stats.err_malformed == 2);

    setup(); /* orphan data */
    FEED(0x80, 0x81, 0x3C, 0x7F);
    CHECK(R.n == 0 && P.stats.err_orphan_data == 2);

    setup(); /* truncated message: dropped, parser recovers */
    FEED(0x80, 0x81, 0x90, 0x3C);
    FEED(0x80, 0x82, 0x80, 0x3C, 0x00);
    CHECK(R.n == 1 && is_short(0, 3, 0x80, 0x3C, 0x00) && P.stats.err_incomplete == 1);

    setup(); /* new status interrupts message */
    FEED(0x80, 0x81, 0x90, 0x3C, 0x82, 0xB0, 0x07, 0x64);
    CHECK(R.n == 1 && is_short(0, 3, 0xB0, 0x07, 0x64) && P.stats.err_incomplete == 1);

    setup(); /* SysEx not terminated by F7 -> closed with F7 */
    static const uint8_t exp[] = {0xF0, 0x01, 0xF7};
    FEED(0x80, 0x81, 0xF0, 0x01, 0x82, 0x90, 0x3C, 0x7F);
    CHECK(sx_is(exp, sizeof exp) && R.n == 1 && P.stats.err_sysex_abort == 1 && !P.in_sysex);

    setup(); /* reset closes open SysEx */
    FEED(0x80, 0x81, 0xF0, 0x01);
    ble_midi_parser_reset(&P);
    CHECK(sx_is(exp, sizeof exp) && !P.in_sysex);

    setup(); /* stray F7, undefined statuses */
    FEED(0x80, 0x81, 0xF7, 0x82, 0xF4, 0x83, 0xF5);
    CHECK(R.n == 0 && P.stats.err_orphan_data == 1 && P.stats.err_malformed == 2);

    /* fuzz: must never crash or overrun, even with tiny sink */
    setup();
    srand(12345);
    uint8_t buf[64];
    for (int it = 0; it < 200000; it++) {
        size_t n = (size_t)(rand() % 64);
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)rand();
        if (n && (it & 1)) buf[0] = 0x80 | (buf[0] & 0x3F);
        R.n = 0;
        R.sx_len = 0;
        ble_midi_parser_feed(&P, buf, n);
        if (R.n > 60 || R.sx_len > 4000) break; /* bounded per packet */
    }
    CHECK(R.n <= 60 && R.sx_len <= 4000);
}

static void t14_timestamp_and_recovery(void)
{
    setup();
    FEED(0x80, 0xFE, 0x90, 0x3C, 0x7F, 0x81, 0x80, 0x3C, 0x00);
    CHECK(R.n == 2 && R.ev[0].ts == 126 && R.ev[1].ts == 129);
    setup();
    FEED(0xBF, 0xFE, 0x90, 0x3C, 0x7F, 0x81, 0x80, 0x3C, 0x00);
    CHECK(R.n == 2 && R.ev[0].ts == 8190 && R.ev[1].ts == 1);
    setup();
    FEED(0x80, 0x81, 0x90, 0x3C, 0x82, 0xF8, 0x7F);
    CHECK(R.n == 2 && R.ev[0].ts == 2 && R.ev[1].ts == 1);
    setup();
    FEED(0x80, 0x81, 0x90, 0x82, 0xF8, 0x3C, 0x7F);
    CHECK(R.n == 2 && R.ev[0].ts == 2 && R.ev[1].ts == 1);
    setup();
    FEED(0x80, 0x81, 0x90, 0x3C);
    CHECK(P.stats.err_incomplete == 1 && P.count == 0 && P.status == 0);
    FEED(0x80, 0x82, 0x40, 0x60);
    CHECK(R.n == 0 && P.stats.err_orphan_data == 2);
    FEED(0x80, 0x83, 0x90, 0x40, 0x60);
    CHECK(R.n == 1 && is_short(0, 3, 0x90, 0x40, 0x60));
    setup();
    FEED(0x80, 0x81, 0xF9, 0x82, 0xFD);
    CHECK(R.n == 0 && P.stats.err_malformed == 2);
    setup();
    FEED(0x80, 0x81, 0xF0, 0x7D);
    ble_midi_parser_feed(&P, NULL, 0);
    const uint8_t closed[] = {0xF0, 0x7D, 0xF7};
    CHECK(sx_is(closed, sizeof closed) && !P.in_sysex);
    FEED(0x80, 0x82, 0x90, 0x3C, 0x7F);
    CHECK(R.n == 1 && P.stats.err_malformed == 1 && P.stats.err_sysex_abort == 1);
}

struct ctx_pub {
    midi_ring_t *r;
    bool *d;
    uint32_t *o;
};
static void sink_ring_sx(void *c, const uint8_t *d, size_t len, uint16_t ts)
{
    (void)ts;
    struct ctx_pub *x = c;
    midi_sysex_put(x->r, d, len, x->d, x->o);
}

static void t13_buffer_overflow(void)
{
    /* SysEx ring of 16 bytes fed with a 200 byte SysEx through the parser */
    static uint8_t store[16];
    midi_ring_t ring;
    midi_ring_init(&ring, store, sizeof store);
    bool dropping = false;
    uint32_t ovf = 0;

    uint8_t big[2 + 200 + 1 + 1];
    big[0] = 0x80;
    big[1] = 0x81;
    big[2] = 0xF0;
    for (int i = 0; i < 200; i++) big[3 + i] = (uint8_t)(i & 0x7F);
    big[203] = 0x82;
    uint8_t tail[] = {0x80, 0x82, 0xF7};

    struct ctx_pub cx = {&ring, &dropping, &ovf};
    ble_midi_sink_t sink = {0, 0, sink_ring_sx, &cx};
    ble_midi_parser_t p;
    ble_midi_parser_init(&p, &sink);

    ble_midi_parser_feed(&p, big, 203); /* header, ts, F0 + 200 data */
    CHECK(midi_ring_used(&ring) == 15);  /* 1 byte reserved for F7 */
    CHECK(dropping && ovf == 1);
    ble_midi_parser_feed(&p, tail, sizeof tail);
    CHECK(midi_ring_used(&ring) == 16);
    CHECK(midi_ring_peek(&ring, 0) == 0xF0);
    CHECK(midi_ring_peek(&ring, 15) == 0xF7); /* terminated, never left open */
    CHECK(!dropping);

    /* consumer drains, next SysEx works again */
    midi_ring_drop(&ring, 16);
    static const uint8_t ok[] = {0xF0, 0x01, 0xF7};
    midi_sysex_put(&ring, ok, sizeof ok, &dropping, &ovf);
    CHECK(midi_ring_used(&ring) == 3 && ovf == 1);

    /* short message ring: all-or-nothing, never overwrites */
    static uint8_t sstore[8];
    midi_ring_t sr;
    midi_ring_init(&sr, sstore, sizeof sstore);
    uint8_t e[4] = {3, 0x90, 0x3C, 0x7F};
    CHECK(midi_ring_put(&sr, e, 4));
    CHECK(midi_ring_put(&sr, e, 4));
    CHECK(!midi_ring_put(&sr, e, 4));
    CHECK(midi_ring_used(&sr) == 8 && midi_ring_peek(&sr, 0) == 3);
    midi_ring_drop(&sr, 4);
    CHECK(midi_ring_put(&sr, e, 4)); /* wrap-around */
    CHECK(midi_ring_peek(&sr, 4) == 3 && midi_ring_peek(&sr, 7) == 0x7F);
}

int main(void)
{
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"01 Note On", t01_note_on},
        {"02 Note Off", t02_note_off},
        {"03 Control Change", t03_cc},
        {"04 Program Change / Pressure", t04_pc_and_pressure},
        {"05 Pitch Bend", t05_pitch_bend},
        {"06 Running Status", t06_running_status},
        {"07 System Common / Realtime", t07_system_common_and_realtime},
        {"08 Realtime inside message", t08_realtime_inside_message},
        {"09 SysEx", t09_sysex},
        {"10 SysEx across packets", t10_sysex_across_packets},
        {"11 Multiple events per packet", t11_multiple_events},
        {"12 Malformed packets (+fuzz)", t12_malformed},
        {"13 Buffer overflow", t13_buffer_overflow},
        {"14 Timestamp wrap / packet recovery", t14_timestamp_and_recovery},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_fail;
        tests[i].fn();
        printf("[%s] %s\n", g_fail == before ? "PASS" : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
