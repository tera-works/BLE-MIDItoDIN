#include "midi_merge.h"
#include <stdio.h>
#include <string.h>
static uint8_t output[64];static size_t used;static int failures,checks;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %d\n",__LINE__);}}while(0)
static void bytes(void *ctx,const uint8_t *d,size_t n,uint16_t ts){(void)ctx;(void)ts;memcpy(output+used,d,n);used+=n;}
static void short_msg(void *ctx,const uint8_t *d,uint8_t n,uint16_t ts){bytes(ctx,d,n,ts);}
static void rt(void *ctx,uint8_t b,uint16_t ts){bytes(ctx,&b,1,ts);}
int main(void)
{
 midi_merge_t merge={.sink={.short_msg=short_msg,.realtime=rt,.sysex=bytes}};
 midi_merge_source_t a,b;ble_midi_sink_t sa=midi_merge_sink(&a,&merge),sb=midi_merge_sink(&b,&merge);
 const uint8_t start[]={0xf0,1},end[]={2,0xf7},note[]={0x90,60,100};
 sa.sysex(sa.ctx,start,2,0);sb.realtime(sb.ctx,0xf8,0);sa.sysex(sa.ctx,end,2,0);
 const uint8_t normal[]={0xf0,1,0xf8,2,0xf7};CHECK(used==sizeof normal && !memcmp(output,normal,used));CHECK(merge.owner==NULL);
 used=0;sa.sysex(sa.ctx,start,2,0);sb.short_msg(sb.ctx,note,3,0);sa.sysex(sa.ctx,end,2,0);
 const uint8_t interrupted[]={0xf0,1,0xf7,0x90,60,100};CHECK(used==sizeof interrupted && !memcmp(output,interrupted,used));CHECK(merge.interrupted==1 && !a.suppressed);
 used=0;sa.sysex(sa.ctx,start,2,0);sb.sysex(sb.ctx,start,2,0);sa.sysex(sa.ctx,end,2,0);sb.sysex(sb.ctx,end,2,0);
 const uint8_t competing[]={0xf0,1,0xf7,0xf0,1,2,0xf7};CHECK(used==sizeof competing && !memcmp(output,competing,used));CHECK(merge.owner==NULL);
 used=0;sa.sysex(sa.ctx,start,2,0);const uint8_t f7=0xf7;sb.sysex(sb.ctx,&f7,1,0);CHECK(used==2 && merge.owner==&a);sa.sysex(sa.ctx,&f7,1,0);CHECK(used==3 && !merge.owner);
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
