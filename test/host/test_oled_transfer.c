#include "oled_transfer.h"
#include <stdio.h>
#include <string.h>
static int checks,failures;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static oled_frame_t panel, wanted, sent;
static uint8_t valid[OLED_PAGES];
/* SSD1306 Co=1 command / Co=0 data decoder, independent of chunk planner. */
static void transmit(const uint8_t *packet,size_t n)
{
    unsigned page=99,column=0,i=0;
    while(i<n) {
        uint8_t control=packet[i++];
        if(control==0x40) {
            CHECK(page<8 && column+n-i<=128);
            while(i<n && page<8 && column<128) panel[page][column++]=packet[i++];
            return;
        }
        CHECK(control==0x80 && i<n);
        if(i>=n) return;
        uint8_t command=packet[i++];
        if((command&0xF8)==0xB0) page=command&7;
        else if((command&0xF0)==0x10) column=(column&15)|((command&15)<<4);
        else if((command&0xF0)==0) column=(column&0xF0)|(command&15);
        else CHECK(false);
    }
    CHECK(false);
}
static unsigned sync_page(unsigned page)
{
    unsigned first,n,transfers=0;
    uint8_t packet[OLED_PACKET_BYTES];
    while(oled_next_chunk(wanted[page],sent[page],valid[page],&first,&n)) {
        CHECK(n>0 && n<=16 && first+n<=128);
        size_t bytes=oled_make_packet(packet,page,first,wanted[page]+first,n);
        CHECK(bytes==n+7 && bytes+1<=32); /* includes I2C slave address */
        transmit(packet,bytes);
        oled_commit_chunk(sent[page],&valid[page],first,wanted[page]+first,n);
        CHECK(++transfers<=8);
    }
    return transfers;
}
int main(void)
{
    memset(panel,0xA5,sizeof panel); memset(sent,0x00,sizeof sent);
    for(unsigned p=0;p<8;p++) for(unsigned x=0;x<128;x++) wanted[p][x]=(uint8_t)(p+x);
    for(unsigned p=0;p<8;p++) CHECK(sync_page(p)==8 && valid[p]==0xFF);
    CHECK(memcmp(wanted,panel,sizeof panel)==0);
    for(unsigned p=0;p<8;p++) CHECK(sync_page(p)==0); /* stable frame: zero I2C */
    wanted[0][0]^=1;wanted[0][127]^=1;
    CHECK(sync_page(0)==2); /* do not transfer the 126-column gap */
    CHECK(memcmp(wanted,panel,sizeof panel)==0);
    unsigned first,n;
    wanted[1][43]^=0x55;
    CHECK(oled_next_chunk(wanted[1],sent[1],valid[1],&first,&n) && first==43 && n==1);
    /* Interrupted / failed I2C: without commit, the same work remains pending. */
    CHECK(oled_next_chunk(wanted[1],sent[1],valid[1],&first,&n) && first==43 && n==1);
    CHECK(sync_page(1)==1);
    /* Desired state changes after half an initial page: converge on latest frame. */
    valid[2]=0;
    for(unsigned i=0;i<4;i++) {
        CHECK(oled_next_chunk(wanted[2],sent[2],valid[2],&first,&n));
        uint8_t packet[OLED_PACKET_BYTES];
        size_t bytes=oled_make_packet(packet,2,first,wanted[2]+first,n);
        transmit(packet,bytes);
        oled_commit_chunk(sent[2],&valid[2],first,wanted[2]+first,n);
    }
    memset(wanted[2],0x33,128); CHECK(sync_page(2)==8);
    CHECK(memcmp(wanted,panel,sizeof panel)==0);
    uint8_t packet[OLED_PACKET_BYTES],data[16]={0};
    CHECK(!oled_make_packet(packet,8,0,data,16));
    CHECK(!oled_make_packet(packet,0,128,data,1));
    CHECK(!oled_make_packet(packet,0,127,data,2));
    CHECK(!oled_make_packet(packet,0,0,data,17));
    CHECK(!oled_make_packet(packet,0,0,data,0));
    CHECK(!oled_make_packet(packet,0,0,NULL,1));
    CHECK(oled_make_packet(packet,7,127,data,1)==8);
    CHECK(!oled_transport_quiet(149,100,50,true));
    CHECK(oled_transport_quiet(150,100,50,true));
    CHECK(!oled_transport_quiet(151,100,50,false)); /* FIFO/backlog/shift active */
    CHECK(!oled_transport_quiet(20,UINT32_MAX-20,50,true));
    CHECK(oled_transport_quiet(30,UINT32_MAX-20,50,true));
    /* Refresh last_rx on every event: a MIDI clock stream never opens a window. */
    for(unsigned t=100;t<10000;t+=20) CHECK(!oled_transport_quiet(t+19,t,50,true));
    printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
