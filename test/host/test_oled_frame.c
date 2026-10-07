#include "oled_frame.h"
#include <stdio.h>
#include <string.h>
static int checks, failures;
#define CHECK(x) do {checks++; if (!(x)) {failures++; printf("FAIL %d: %s\n",__LINE__,#x);}} while(0)
int main(int argc, char **argv)
{
    struct { unsigned char pre[16]; oled_frame_t f; unsigned char post[16]; } guarded;
    memset(&guarded,0xA5,sizeof guarded);
    oled_frame_clear(guarded.f);
    oled_frame_text(guarded.f,7,"ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFGHIJKLMNOPQRSTUVWXYZ");
    oled_frame_text(guarded.f,8,"INVALID PAGE");
    for(unsigned i=0;i<16;i++) CHECK(guarded.pre[i]==0xA5 && guarded.post[i]==0xA5);
    oled_frame_t sent;
    oled_frame_clear(sent);
    unsigned first,n;
    CHECK(!oled_frame_diff(sent[0],sent[1],&first,&n) && n==0);
    sent[0][0]=1; sent[0][127]=2;
    CHECK(oled_frame_diff(sent[0],sent[1],&first,&n) && first==0 && n==128);
    memset(sent[0],0,128);sent[0][127]=2;
    CHECK(oled_frame_diff(sent[0],sent[1],&first,&n) && first==127 && n==1);
    sent[0][17]=3;
    CHECK(oled_frame_diff(sent[0],sent[1],&first,&n) && first==17 && n==111);
    memcpy(sent[1]+first,sent[0]+first,n);
    CHECK(!oled_frame_diff(sent[0],sent[1],&first,&n));
    oled_frame_text(guarded.f,0,"1234567890");
    oled_frame_text(guarded.f,0,"1");
    for(unsigned i=6;i<128;i++) CHECK(guarded.f[0][i]==0); /* erase stale text */
    oled_frame_text(guarded.f,0,"mi.1");
    oled_frame_text(guarded.f,1,"MI.1");
    CHECK(memcmp(guarded.f[0],guarded.f[1],128)!=0);
    oled_frame_clear(guarded.f);
    const char *rows[]={"BLE MIDI \x1f DIN","","\x10 ShoulderKB","\x10 Accordion",
        "\x11 nanoKEY", "", "","OUT: OK       2/4"};
    for(unsigned i=0;i<8;i++) oled_frame_text(guarded.f,i,rows[i]);
    if(argc>1) {
        FILE *out=fopen(argv[1],"wb");
        CHECK(out!=NULL);
        if(out) {fprintf(out,"P5\n128 64\n255\n");
            for(unsigned y=0;y<64;y++) for(unsigned x=0;x<128;x++)
                fputc((guarded.f[y/8][x] >> (y%8)) & 1 ? 255:0,out);
            fclose(out);
        }
    }
    printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
