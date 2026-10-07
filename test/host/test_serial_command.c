#include "serial_command.h"
#include <stdio.h>
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %d %s\n",__LINE__,#x);}}while(0)
int main(void){midi_serial_command_t c;
 const char *valid[]={"help","?","status","list","scan","scan on","scan off","found","connect 1","disconnect 4","add 24","remove 2","confirm","cancel","log on","log off","  list  ","add\t2"};
 for(unsigned i=0;i<sizeof valid/sizeof valid[0];i++)CHECK(midi_serial_parse(valid[i],&c));
 CHECK(midi_serial_parse("connect 1",&c)&&c.op==SERIAL_CONNECT&&c.index==0);
 CHECK(midi_serial_parse("add 24",&c)&&c.op==SERIAL_ADD&&c.index==23);
 CHECK(midi_serial_parse("remove 2",&c)&&c.op==SERIAL_REMOVE&&c.index==1);
 CHECK(midi_serial_parse("scan off",&c)&&c.op==SERIAL_SCAN_OFF);
 CHECK(midi_serial_parse("log off",&c)&&c.op==SERIAL_LOG_OFF);
 const char *invalid[]={""," ","remove","confirm 1","remove 0","add 25","connect -1","connect +1","add 1x","connect 99999999999999999999","list x","scan yes","log","remove 1 confirm","confirm;remove 2","STATUS"};
 for(unsigned i=0;i<sizeof invalid/sizeof invalid[0];i++)CHECK(!midi_serial_parse(invalid[i],&c));
 char longline[90];for(unsigned i=0;i<89;i++)longline[i]='x';longline[89]=0;CHECK(!midi_serial_parse(longline,&c));
 printf("%u checks, %u failures\n",checks,failures);return failures?1:0;}
