#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
enum { SERIAL_HELP, SERIAL_STATUS, SERIAL_LIST, SERIAL_SCAN_ON, SERIAL_SCAN_OFF,
 SERIAL_FOUND, SERIAL_CONNECT, SERIAL_DISCONNECT, SERIAL_ADD, SERIAL_REMOVE,
 SERIAL_CONFIRM, SERIAL_CANCEL, SERIAL_LOG_ON, SERIAL_LOG_OFF };
typedef struct { int op; unsigned index; } midi_serial_command_t;
/* No heap, exact grammar; indexes shown to users start at 1. */
static inline bool midi_serial_parse(const char *line,midi_serial_command_t *out)
{
 char copy[80];size_t n=strlen(line);if(n>=sizeof copy)return false;memcpy(copy,line,n+1);
 char *words[3]={0};unsigned count=0;char *s=copy;
 while(*s){while(*s==' '||*s=='	')s++;if(!*s)break;if(count==3)return false;words[count++]=s;while(*s && *s!=' ' && *s!='	')s++;if(*s)*s++=0;}
 if(!count)return false;
 const char *single[]={"help","status","list", "scan", "", "found", "", "", "", "", "confirm","cancel"};
 if(count==1){if(!strcmp(words[0],"?")){*out=(midi_serial_command_t){SERIAL_HELP,0};return true;}
  for(unsigned i=0;i<sizeof(single)/sizeof(single[0]);i++) { if(single[i][0]&&!strcmp(words[0],single[i])){*out=(midi_serial_command_t){(int)i,0};return true;} } return false;}
 if(count!=2)return false;
 if(!strcmp(words[0],"scan")||!strcmp(words[0],"log")){
  int base=!strcmp(words[0],"scan")?SERIAL_SCAN_ON:SERIAL_LOG_ON;
  if(!strcmp(words[1],"on")){*out=(midi_serial_command_t){base,0};return true;}
  if(!strcmp(words[1],"off")){*out=(midi_serial_command_t){base+1,0};return true;}return false;}
 int op=!strcmp(words[0],"connect")?SERIAL_CONNECT:!strcmp(words[0],"disconnect")?SERIAL_DISCONNECT:
        !strcmp(words[0],"add")?SERIAL_ADD:!strcmp(words[0],"remove")?SERIAL_REMOVE:-1;
 if(op<0)return false;
 for(const char *q=words[1];*q;q++)if(*q<'0'||*q>'9')return false;
 char *end;unsigned long index=strtoul(words[1],&end,10);if(*end||index<1||index>24)return false;
 *out=(midi_serial_command_t){op,(unsigned)index-1};return true;
}
