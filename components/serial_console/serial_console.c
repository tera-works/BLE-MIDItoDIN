#include "serial_console.h"
#include "ble_midi_client.h"
#include "config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <stdio.h>
static bool s_stats=true;
static midi_serial_response_t s_reply;
bool serial_console_stats_enabled(void){return __atomic_load_n(&s_stats,__ATOMIC_RELAXED);}
static void help(void){printf("\nCommands (case sensitive):\nhelp | status | list | found\nscan [on|off]\nconnect N | disconnect N | add N\nremove N -> confirm | cancel\nlog on | log off\nN starts at 1: list for registered, found for scan.\n");}
static void peers(const midi_serial_peer_t *p,unsigned n,bool scan){
 static const char *states[]={"OFFLINE","CONNECTED","CONNECTING","ERROR"};
 printf("%s: %u\n",scan?"FOUND":"REGISTERED",n);
 for(unsigned i=0;i<n;i++){
  printf("%u: %s [%s]",i+1,p[i].name[0]?p[i].name:"Unknown MIDI",p[i].addr);
  if(scan)printf(" RSSI=%d\n",p[i].rssi);
  else printf(" %s auto=%s CI=%u.%02ums MTU=%u\n",states[p[i].state<4?p[i].state:3],p[i].disabled?"off":"on",p[i].interval_units*125u/100,p[i].interval_units*125u%100,p[i].mtu);
 }
}
static void command(const char *line){
 midi_serial_command_t cmd;if(!midi_serial_parse(line,&cmd)){printf("ERR invalid command; type help\n");return;}
 if(cmd.op==SERIAL_HELP){help();return;}
 if(cmd.op==SERIAL_LOG_ON || cmd.op==SERIAL_LOG_OFF){__atomic_store_n(&s_stats,cmd.op==SERIAL_LOG_ON,__ATOMIC_RELAXED);printf("OK periodic stats %s\n",serial_console_stats_enabled()?"on":"off");return;}
 if(!ble_midi_client_serial(&cmd,&s_reply)){printf("ERR BLE not ready or busy; retry\n");return;}
 printf("%s %s\n",s_reply.ok?"OK":"ERR",s_reply.message);
 if(cmd.op==SERIAL_STATUS){
  printf("FW=%s mode=%s registered=%u connected=%u saving=%s\n",FW_VERSION,s_reply.scan?"SCAN":"DEVICES",s_reply.registered_count,s_reply.stats.ui_connected,s_reply.saving?"yes":"no");
  printf("MIDI notif=%lu dropped=%lu short=%lu realtime=%lu sysex=%lu parser_errors=%lu\n",(unsigned long)s_reply.stats.notifications,(unsigned long)s_reply.stats.notif_dropped,(unsigned long)s_reply.stats.parser.short_msgs,(unsigned long)s_reply.stats.parser.realtime_msgs,(unsigned long)s_reply.stats.parser.sysex_chunks,(unsigned long)(s_reply.stats.parser.err_malformed+s_reply.stats.parser.err_orphan_data+s_reply.stats.parser.err_incomplete+s_reply.stats.parser.err_sysex_abort));
 }
 if(cmd.op==SERIAL_LIST || cmd.op==SERIAL_STATUS)peers(s_reply.registered,s_reply.registered_count,false);
 if(cmd.op==SERIAL_FOUND)peers(s_reply.found,s_reply.found_count,true);
 if(s_reply.remove_pending && cmd.op!=SERIAL_REMOVE)printf("Pending removal: %s (confirm / cancel)\n",s_reply.remove_name);
 if(s_reply.saving)printf("Saving... wait until status reports saving=no before power off.\n");
}
static void task(void *arg){
 (void)arg;char line[80];unsigned used=0;bool overflow=false;
 printf("\nBLE MIDI serial console %s (115200 8N1). Type help.\n> ",FW_VERSION);fflush(stdout);
 for(;;){uint8_t c;if(uart_read_bytes(UART_NUM_0,&c,1,portMAX_DELAY)!=1)continue;
  if(c==13 || c==10){
   if(!used && !overflow){continue;}printf("\n");line[used]=0;
   if(overflow)printf("ERR line too long or non-ASCII; discarded\n");else command(line);
   used=0;overflow=false;printf("> ");fflush(stdout);
  }else if(c==3){used=0;overflow=false;printf("^C\n");command("cancel");printf("> ");fflush(stdout);}
  else if(c==8 || c==127){if(used && !overflow){used--;printf("\b \b");fflush(stdout);}}
  else if(c>=32 && c<127){if(used<sizeof(line)-1 && !overflow){line[used++]=(char)c;putchar(c);fflush(stdout);}else overflow=true;}
  else overflow=true;
 }
}
void serial_console_start(void){
 esp_err_t err=uart_driver_install(UART_NUM_0,512,0,0,NULL,0);
 if(err!=ESP_OK){ESP_LOGW("CONSOLE","UART0 input unavailable: %s",esp_err_to_name(err));return;}
 if(xTaskCreate(task,"serial_console",4096,NULL,1,NULL)!=pdPASS)ESP_LOGW("CONSOLE","Task allocation failed");
}
