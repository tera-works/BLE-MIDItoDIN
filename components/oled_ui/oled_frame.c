#include "oled_frame.h"
#include <string.h>
/* Compact, original 5x7 column glyph definitions for the ASCII status UI. */
static const uint8_t digits[10][5] = {
 {62,65,73,65,62},{0,66,127,64,0},{98,81,73,73,70},{34,65,73,73,54},
 {24,20,18,127,16},{39,69,69,69,57},{62,73,73,73,48},{1,113,9,5,3},
 {54,73,73,73,54},{6,73,73,73,62}};
static const uint8_t letters[26][5] = {
 {126,9,9,9,126},{127,73,73,73,54},{62,65,65,65,34},{127,65,65,34,28},
 {127,73,73,73,65},{127,9,9,9,1},{62,65,73,73,58},{127,8,8,8,127},
 {0,65,127,65,0},{32,64,65,63,1},{127,8,20,34,65},{127,64,64,64,64},
 {127,2,12,2,127},{127,4,8,16,127},{62,65,65,65,62},{127,9,9,9,6},
 {62,65,81,33,94},{127,9,25,41,70},{38,73,73,73,50},{1,1,127,1,1},
 {63,64,64,64,63},{31,32,64,32,31},{63,64,56,64,63},{99,20,8,20,99},
 {7,8,112,8,7},{97,81,73,69,67}};
static const uint8_t lowercase[26][5] = {
 {32,84,84,84,120},{127,72,68,68,56},{56,68,68,68,32},{56,68,68,72,127},
 {56,84,84,84,24},{8,126,9,1,2},{12,82,82,82,62},{127,8,4,4,120},
 {0,68,125,64,0},{32,64,68,61,0},{127,16,40,68,0},{0,65,127,64,0},
 {124,4,24,4,120},{124,8,4,4,120},{56,68,68,68,56},{124,20,20,20,8},
 {8,20,20,24,124},{124,8,4,4,8},{72,84,84,84,32},{4,63,68,64,32},
 {60,64,64,32,124},{28,32,64,32,28},{60,64,48,64,60},{68,40,16,40,68},
 {12,80,80,80,60},{68,100,84,76,68}};
static void glyph(unsigned char c, uint8_t out[5])
{
    memset(out, 0, 5);
    if (c >= 'a' && c <= 'z') { memcpy(out, lowercase[c-'a'], 5); return; }
    if (c == 0x10) { const uint8_t g[5]={28,62,62,62,28}; memcpy(out,g,5); return; }
    if (c == 0x11) { const uint8_t g[5]={28,34,34,34,28}; memcpy(out,g,5); return; }
    if (c == 0x12) { out[0]=out[2]=out[4]=32; return; }
    if (c == '!') { out[2]=95; return; }
    if (c == '>') { out[1]=65;out[2]=34;out[3]=20;out[4]=8;return; }
    if (c == 0x1f) { const uint8_t g[5]={8,8,42,28,8}; memcpy(out,g,5); return; }
    if (c >= 'A' && c <= 'Z') memcpy(out, letters[c-'A'], 5);
    else if (c >= '0' && c <= '9') memcpy(out, digits[c-'0'], 5);
    else if (c == '.') out[2] = 64;
    else if (c == ':') out[2] = 36;
    else if (c == '-') out[1] = out[2] = out[3] = 8;
    else if (c == '/') {out[0]=32;out[1]=16;out[2]=8;out[3]=4;out[4]=2;}
    else if (c != ' ') {out[0]=2;out[1]=1;out[2]=81;out[3]=9;out[4]=6;}
}
void oled_frame_clear(oled_frame_t frame) { memset(frame, 0, sizeof(oled_frame_t)); }
void oled_frame_text(oled_frame_t frame, unsigned page, const char *text)
{
    if (page >= OLED_PAGES || !text) return;
    memset(frame[page], 0, OLED_WIDTH);
    for (unsigned x=0; *text && x+5 <= OLED_WIDTH; x+=6) {
        glyph((unsigned char)*text++, &frame[page][x]);
    }
}
bool oled_frame_diff(const uint8_t wanted[OLED_WIDTH], const uint8_t sent[OLED_WIDTH],
                     unsigned *first, unsigned *length)
{
    unsigned a=0, b=OLED_WIDTH;
    while (a < OLED_WIDTH && wanted[a] == sent[a]) a++;
    if (a == OLED_WIDTH) { *first=0; *length=0; return false; }
    while (b > a && wanted[b-1] == sent[b-1]) b--;
    *first=a; *length=b-a;
    return true;
}
