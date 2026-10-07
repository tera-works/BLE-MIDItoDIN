#include "oled_transfer.h"
#include <string.h>
_Static_assert(OLED_WIDTH == OLED_CHUNK_BYTES * 8, "8 block validity bits per page");
bool oled_next_chunk(const uint8_t wanted[OLED_WIDTH], const uint8_t sent[OLED_WIDTH],
                    uint8_t valid, unsigned *first, unsigned *length)
{
    for (unsigned block=0; block<8; block++) {
        unsigned begin=block*OLED_CHUNK_BYTES, end=begin+OLED_CHUNK_BYTES;
        if (!(valid & (1u<<block))) { *first=begin; *length=OLED_CHUNK_BYTES; return true; }
        unsigned a=begin, b=end;
        while (a<end && wanted[a]==sent[a]) a++;
        if (a==end) continue;
        while (b>a && wanted[b-1]==sent[b-1]) b--;
        *first=a; *length=b-a; return true;
    }
    *first=0; *length=0; return false;
}
size_t oled_make_packet(uint8_t out[OLED_PACKET_BYTES], unsigned page, unsigned first,
                        const uint8_t *data, unsigned length)
{
    if (page>=OLED_PAGES || first>=OLED_WIDTH || !data || !length ||
        length>OLED_CHUNK_BYTES || length>OLED_WIDTH-first) return 0;
    const uint8_t commands[7]={0x80,(uint8_t)(0xB0|page),0x80,(uint8_t)(first&15),
                              0x80,(uint8_t)(0x10|(first>>4)),0x40};
    memcpy(out,commands,7); memcpy(out+7,data,length);
    return length+7;
}
void oled_commit_chunk(uint8_t sent[OLED_WIDTH], uint8_t *valid,
                       unsigned first, const uint8_t *data, unsigned length)
{
    if (!data || first>=OLED_WIDTH || !length || length>OLED_CHUNK_BYTES ||
        length>OLED_WIDTH-first) return;
    memcpy(sent+first,data,length);
    /* Only a whole block makes previously unknown display RAM valid. */
    if (first%OLED_CHUNK_BYTES==0 && length==OLED_CHUNK_BYTES)
        *valid |= (uint8_t)(1u << (first/OLED_CHUNK_BYTES));
}
