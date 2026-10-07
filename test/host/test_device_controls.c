#include "device_controls.h"
#include <stdio.h>
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
int main(void)
{
 midi_registry_t r={.version=MIDI_REGISTRY_VERSION};
 midi_peer_t p={.type=0,.addr={1,2,3,4,5,6},.name="ShoulderKB"};
 CHECK(midi_registry_valid(&r));CHECK(midi_registry_add(&r,&p)==0);
 CHECK(midi_registry_add(&r,&p)==0 && r.count==1);
 for(int i=1;i<4;i++){p.addr[0]=(uint8_t)(i+1);CHECK(midi_registry_add(&r,&p)==i);}
 p.addr[0]=9;CHECK(midi_registry_add(&r,&p)==-1 && r.count==4);
 midi_registry_t restored;memcpy(&restored,&r,sizeof r);CHECK(midi_registry_valid(&restored));
 restored.version=99;CHECK(!midi_registry_valid(&restored));restored=r;
 restored.count=5;CHECK(!midi_registry_valid(&restored));restored=r;
 memset(restored.peer[0].name,'a',32);CHECK(!midi_registry_valid(&restored));restored=r;
 restored.peer[1]=restored.peer[0];CHECK(!midi_registry_valid(&restored));
 r=(midi_registry_t){.version=MIDI_REGISTRY_VERSION};
 p=(midi_peer_t){.type=1,.addr={1,0,0,0,0,0x40},.name="nanoKEY Studio"};
 CHECK(midi_registry_add(&r,&p)==0);p.addr[0]=2;CHECK(midi_registry_find(&r,&p)==0);
 p.name[0]=0;CHECK(midi_registry_add(&r,&p)==-2);
 p.addr[5]=0xc0;CHECK(midi_registry_add(&r,&p)==1);CHECK(midi_addr_stable(&p));
 CHECK(midi_selection_move(0,4,-1)==3);CHECK(midi_selection_move(3,4,1)==0);
 CHECK(midi_selection_move(0,0,1)==0);CHECK(midi_selection_move(1,4,10)==3);
 midi_registry_t removed=r;CHECK(!midi_registry_remove(&removed,99));
 CHECK(midi_registry_remove(&removed,0));CHECK(removed.count==1 && removed.peer[0].addr[5]==0xc0);
 CHECK(midi_registry_valid(&removed));CHECK(midi_registry_remove(&removed,0));CHECK(removed.count==0 && midi_registry_valid(&removed));
 CHECK(midi_registered_after_remove(-1,1)==-1);CHECK(midi_registered_after_remove(0,1)==0);
 CHECK(midi_registered_after_remove(1,1)==-1);CHECK(midi_registered_after_remove(3,1)==2);
 midi_input_t input={.ab=3};unsigned clockwise[]={1,0,2,3};
 for(unsigned i=0;i<4;i++) CHECK(midi_input_step(&input,clockwise[i],false,i*5)==(i==3?MIDI_CONTROL_NEXT:MIDI_CONTROL_NONE));
 unsigned reverse[]={2,0,1,3};
 for(unsigned i=0;i<4;i++) CHECK(midi_input_step(&input,reverse[i],false,50+i*5)==(i==3?MIDI_CONTROL_PREV:MIDI_CONTROL_NONE));
 CHECK(!midi_input_step(&input,0,false,100));CHECK(!midi_input_step(&input,3,false,105));
 CHECK(!midi_input_step(&input,3,true,200));CHECK(!midi_input_step(&input,3,false,205));
 CHECK(!midi_input_step(&input,3,true,210));CHECK(!midi_input_step(&input,3,true,230));
 CHECK(!midi_input_step(&input,3,false,300));CHECK(!midi_input_step(&input,3,false,320));
 CHECK(!midi_input_step(&input,3,false,669));CHECK(midi_input_step(&input,3,false,670)==MIDI_CONTROL_PUSH);
 CHECK(!midi_input_step(&input,3,false,700));
 input=(midi_input_t){.ab=3};
 CHECK(!midi_input_step(&input,3,true,1000));CHECK(!midi_input_step(&input,3,true,1020));
 CHECK(!midi_input_step(&input,3,false,1100));CHECK(!midi_input_step(&input,3,false,1120));
 CHECK(!midi_input_step(&input,3,true,1300));CHECK(!midi_input_step(&input,3,true,1320));
 CHECK(!midi_input_step(&input,3,false,1400));CHECK(midi_input_step(&input,3,false,1420)==MIDI_CONTROL_DOUBLE);
 CHECK(!midi_input_step(&input,3,false,1900));
 input=(midi_input_t){.ab=3};
 CHECK(!midi_input_step(&input,3,true,2000));CHECK(!midi_input_step(&input,3,true,2020));
 CHECK(!midi_input_step(&input,3,true,3519));CHECK(midi_input_step(&input,3,true,3520)==MIDI_CONTROL_HOLD);
 CHECK(!midi_input_step(&input,3,true,4000));CHECK(!midi_input_step(&input,3,false,4010));CHECK(!midi_input_step(&input,3,false,4030));
 CHECK(!midi_input_step(&input,3,false,5000));
 /* Short then long emits HOLD only; no disconnect before entering SCAN. */
 input=(midi_input_t){.ab=3};
 CHECK(!midi_input_step(&input,3,true,0));CHECK(!midi_input_step(&input,3,true,20));
 CHECK(!midi_input_step(&input,3,false,100));CHECK(!midi_input_step(&input,3,false,120));
 CHECK(!midi_input_step(&input,3,true,200));CHECK(!midi_input_step(&input,3,true,220));
 CHECK(midi_input_step(&input,3,true,1720)==MIDI_CONTROL_HOLD);
 CHECK(!midi_input_step(&input,3,false,1800));CHECK(!midi_input_step(&input,3,false,1820));CHECK(!midi_input_step(&input,3,false,2200));
 /* Pending single deadline and double both work across the uint32 wrap. */
 input=(midi_input_t){.ab=3};uint32_t base=UINT32_MAX-100;
 CHECK(!midi_input_step(&input,3,true,base));CHECK(!midi_input_step(&input,3,true,base+20));
 CHECK(midi_input_step(&input,3,true,base+1520)==MIDI_CONTROL_HOLD);
 input=(midi_input_t){.ab=3};
 CHECK(!midi_input_step(&input,3,true,base));CHECK(!midi_input_step(&input,3,true,base+20));
 CHECK(!midi_input_step(&input,3,false,base+50));CHECK(!midi_input_step(&input,3,false,base+70));
 CHECK(midi_input_step(&input,3,false,base+420)==MIDI_CONTROL_PUSH);
 /* Remove middle/last from a full registry and restore persistence image. */
 removed=(midi_registry_t){.version=MIDI_REGISTRY_VERSION};
 for(unsigned i=0;i<4;i++){p=(midi_peer_t){.type=0,.addr={(uint8_t)i,1,2,3,4,5},.name="peer"};CHECK(midi_registry_add(&removed,&p)==(int)i);}
 CHECK(midi_registry_remove(&removed,1));CHECK(removed.count==3 && removed.peer[1].addr[0]==2 && removed.peer[2].addr[0]==3);
 CHECK(midi_registry_remove(&removed,2));CHECK(removed.count==2 && removed.peer[1].addr[0]==2);
 restored=removed;CHECK(midi_registry_valid(&restored));CHECK(midi_registry_find(&restored,&p)==-1);
 /* A double click across counter wrap emits no PUSH. */
 input=(midi_input_t){.ab=3};
 CHECK(!midi_input_step(&input,3,true,base));CHECK(!midi_input_step(&input,3,true,base+20));
 CHECK(!midi_input_step(&input,3,false,base+50));CHECK(!midi_input_step(&input,3,false,base+70));
 CHECK(!midi_input_step(&input,3,true,base+150));CHECK(!midi_input_step(&input,3,true,base+170));
 CHECK(!midi_input_step(&input,3,false,base+200));CHECK(midi_input_step(&input,3,false,base+220)==MIDI_CONTROL_DOUBLE);
 CHECK(!midi_input_step(&input,3,false,base+600));
 printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
