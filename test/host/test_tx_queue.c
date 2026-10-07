#include <stdio.h>
#include <string.h>
/* Small capacities deliberately force FIFO/backlog/overflow boundaries. */
#define MIDI_SHORT_RING_LEN 8
#define MIDI_RT_RING_LEN 4
#define MIDI_SYSEX_RING_LEN 16
#include "midi_tx_queue.h"

static midi_tx_queue_t q;
static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static const midi_tx_stamp_t none = {0, false};
static uint8_t wire[256];
static size_t wire_len;
static void reset(void) { midi_tx_queue_init(&q); wire_len = 0; }
static void drain(void)
{
    uint8_t data[3]; midi_tx_stamp_t stamp;
    while (midi_tx_queue_pending(&q)) {
        uint8_t n = midi_tx_queue_pop(&q, data, 3, &stamp);
        CHECK(n != 0);
        if (!n || wire_len + n > sizeof(wire)) break;
        memcpy(wire + wire_len, data, n); wire_len += n;
    }
}
static void expect(const uint8_t *data, size_t n)
{ CHECK(wire_len == n && memcmp(wire, data, n) == 0); }

int main(void)
{
    const uint8_t pc[] = {0xC0, 7}, note[] = {0x90, 60, 100};
    const uint8_t sx[] = {0xF0, 0x7D, 1, 0xF7};
    uint8_t b[3]; midi_tx_stamp_t stamp;
    reset();
    CHECK(midi_tx_queue_short(&q, pc, 2, none));
    midi_tx_queue_sysex(&q, sx, sizeof sx, none);
    CHECK(midi_tx_queue_short(&q, note, 3, none));
    CHECK(midi_tx_queue_pop(&q, b, 0, &stamp) == 0);
    CHECK(midi_tx_queue_pop(&q, b, 1, &stamp) == 0);
    drain();
    const uint8_t ordered[] = {0xC0,7,0xF0,0x7D,1,0xF7,0x90,60,100};
    expect(ordered, sizeof ordered);

    reset();
    midi_tx_queue_sysex(&q, sx, 2, none);
    CHECK(midi_tx_queue_pop(&q, b, 1, &stamp) == 1 && b[0] == 0xF0);
    wire[wire_len++] = b[0];
    CHECK(midi_tx_queue_rt(&q, 0xF8, none));
    midi_tx_queue_sysex(&q, sx + 2, 2, none);
    CHECK(midi_tx_queue_short(&q, note, 3, none));
    drain();
    const uint8_t mixed[] = {0xF0,0xF8,0x7D,1,0xF7,0x90,60,100};
    expect(mixed, sizeof mixed);

    reset();
    uint8_t large[40]; large[0] = 0xF0;
    memset(large + 1, 1, sizeof large - 1);
    midi_tx_queue_sysex(&q, large, sizeof large, none);
    CHECK(q.sysex_overflow == 1 && q.sysex_dropping && !q.sysex_input_open);
    drain();
    CHECK(wire_len == 16 && wire[0] == 0xF0 && wire[15] == 0xF7);
    midi_tx_queue_sysex(&q, sx + 3, 1, none);
    CHECK(!q.sysex_dropping && !midi_tx_queue_pending(&q));
    CHECK(midi_tx_queue_short(&q, note, 3, none));
    drain();
    CHECK(wire_len == 19 && wire[16] == 0x90);

    reset();
    for (unsigned i = 0; i < 7; i++) CHECK(midi_tx_queue_short(&q, pc, 2, none));
    CHECK(!midi_tx_queue_short(&q, note, 3, none) && q.short_overflow == 1);
    midi_tx_queue_sysex(&q, sx, sizeof sx, none);
    CHECK(q.sysex_overflow == 1 && !q.sysex_input_open);
    drain();
    CHECK(wire_len == 14);

    reset();
    midi_tx_queue_sysex(&q, sx, 1, none);
    uint8_t data = 2;
    for (unsigned i = 0; i < 10; i++) midi_tx_queue_sysex(&q, &data, 1, none);
    CHECK(q.sysex_overflow == 1 && !q.sysex_input_open);
    drain();
    CHECK(wire[0] == 0xF0 && wire[wire_len - 1] == 0xF7);
    midi_tx_queue_sysex(&q, sx + 3, 1, none);
    CHECK(!q.sysex_dropping);

    reset();
    for (unsigned i = 0; i < 4; i++) CHECK(midi_tx_queue_rt(&q, 0xF8, none));
    CHECK(!midi_tx_queue_rt(&q, 0xF8, none) && q.rt_overflow == 1);
    drain(); CHECK(wire_len == 4);
    for (unsigned i = 0; i < 1000; i++) {
        CHECK(midi_tx_queue_short(&q, note, 3, none));
        CHECK(midi_tx_queue_pop(&q, b, 3, &stamp) == 3 && memcmp(b, note, 3) == 0);
    }

    reset();
    midi_tx_stamp_t origin = {1234, true};
    midi_tx_queue_sysex(&q, sx, sizeof sx, origin);
    CHECK(midi_tx_queue_pop(&q, b, 1, &stamp) == 1 && stamp.measure && stamp.cycles == 1234);
    CHECK(midi_tx_queue_pop(&q, b, 1, &stamp) == 1 && !stamp.measure);
    reset();
    CHECK(midi_tx_queue_can_direct_short(&q, 3, 3));
    CHECK(!midi_tx_queue_can_direct_short(&q, 3, 2));
    CHECK(!midi_tx_queue_can_direct_short(&q, 0, 128));
    CHECK(!midi_tx_queue_can_direct_short(&q, 4, 128));
    CHECK(midi_tx_queue_short(&q, note, 3, none));
    CHECK(!midi_tx_queue_can_direct_short(&q, 3, 128));
    drain();
    CHECK(midi_tx_queue_rt(&q, 0xF8, none));
    CHECK(!midi_tx_queue_can_direct_short(&q, 3, 128));
    drain();
    midi_tx_queue_sysex(&q, sx, 2, none);
    drain();
    /* Even with an empty backlog, a partial SysEx excludes a channel status. */
    CHECK(!midi_tx_queue_pending(&q) && q.sysex_input_open);
    CHECK(!midi_tx_queue_can_direct_short(&q, 3, 128));
    midi_tx_queue_sysex(&q, sx+2, 2, none);
    drain();
    CHECK(midi_tx_queue_can_direct_short(&q, 3, 128));
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
