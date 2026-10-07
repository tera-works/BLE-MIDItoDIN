#include "ble_midi_client.h"
#include "ble_midi_peer_policy.h"
#include "device_controls.h"
#include "midi_merge.h"
#include "midi_uart.h"
#include "esp_cpu.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "latency_debug.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"

_Static_assert(BLE_ADDR_PUBLIC == 0 && BLE_ADDR_RANDOM == 1, "peer policy address types");
static const char *TAG = "BLE_MIDI";

/* 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 (little endian) */
static const ble_uuid128_t UUID_MIDI_SVC =
    BLE_UUID128_INIT(0x00, 0xc7, 0xc4, 0x4e, 0xe3, 0x6c, 0x51, 0xa7, 0x33, 0x4b, 0xe8, 0xed, 0x5a, 0x0e, 0xb8, 0x03);
/* 7772E5DB-3868-4112-A1A9-F2669D106BF3 (little endian) */
static const ble_uuid128_t UUID_MIDI_CHR =
    BLE_UUID128_INIT(0xf3, 0x6b, 0x10, 0x9d, 0x66, 0xf2, 0xa9, 0xa1, 0x12, 0x41, 0x68, 0x38, 0xdb, 0xe5, 0x72, 0x77);

#define RX_COPY_MAX 512 /* only used when the notification is an mbuf chain */
#define SEEN_MAX    24

typedef struct {
    int registered;
    bool used;
    bool ready; /* notifications enabled */
    bool sec_tried;
    uint16_t conn;
    uint16_t svc_start, svc_end;
    uint16_t chr_val, chr_end;
    uint8_t chr_props;
    uint16_t cccd;
    bool svc_found, chr_found;
    midi_merge_source_t merge_source;
    ble_midi_parser_t parser;
} conn_slot_t;

static conn_slot_t s_slots[BLE_MIDI_MAX_CONN];
static midi_merge_t s_merge;
static uint8_t s_own_addr_type;
static bool s_connecting;
static ble_midi_client_stats_t s_link;
static ble_midi_client_stats_t s_snapshot;
static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static struct ble_npl_callout s_snapshot_co;
static QueueHandle_t s_ui_queue;
#if OLED_ENABLED
static StaticQueue_t s_ui_queue_control;
static uint8_t s_ui_queue_data[sizeof(ble_midi_client_stats_t)];
#endif
static void refresh_ui_list(void);

static void publish_ui(void)
{
    refresh_ui_list();
    if (s_ui_queue) xQueueOverwrite(s_ui_queue, &s_link);
}

static void set_state(ble_midi_link_state_t state)
{
    s_link.state = state;
    publish_ui();
}

static struct ble_npl_callout s_scan_co;
static uint8_t s_rx_copy[RX_COPY_MAX];
static uint32_t s_last_persist_activity;
static void mark_persist_activity(const uint8_t *data,size_t n)
{
 /* BLE header + timestamp + Active Sensing alone is not performance activity. */
 if(n!=3 || data[2]!=0xfe) __atomic_store_n(&s_last_persist_activity,esp_cpu_get_cycle_count(),__ATOMIC_RELAXED);
}

static struct {
    uint32_t notifications, notif_dropped, connections, disconnects;
} s_cs;

typedef struct {
    ble_addr_t addr;
    int8_t rssi;
    bool midi_logged;
    char name[32];
} seen_t;
static seen_t s_seen[SEEN_MAX];
static int s_seen_n;

static int gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);
static void begin_recovery(void);
static void snapshot_cb(struct ble_npl_event *ev);

/* ---- helpers ------------------------------------------------------------------ */

static conn_slot_t *slot_by_conn(uint16_t conn)
{
    for (int i = 0; i < BLE_MIDI_MAX_CONN; i++) {
        if (s_slots[i].used && s_slots[i].conn == conn) {
            return &s_slots[i];
        }
    }
    return NULL;
}

static conn_slot_t *slot_alloc(void)
{
    for (int i = 0; i < BLE_MIDI_MAX_CONN; i++) {
        if (!s_slots[i].used) {
            conn_slot_t *s = &s_slots[i];
            s->registered = -1;
            s->used = true;
            s->ready = false;
            s->sec_tried = false;
            s->svc_found = s->chr_found = false;
            s->cccd = 0;
            s->conn = BLE_HS_CONN_HANDLE_NONE;
            return s;
        }
    }
    return NULL;
}

static void addr_str(const ble_addr_t *a, char *out /* >= 18 */)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1],
             a->val[0]);
}

static void log_conn_params(const char *label, uint16_t conn)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) != 0) {
        return;
    }
    s_link.interval_units = d.conn_itvl;
    unsigned iv = d.conn_itvl * 125u;
    ESP_LOGI(TAG, "%s Connection Interval: %u.%02u ms (%u units), Peripheral Latency: %u, Supervision Timeout: %u ms",
             label, iv / 100, iv % 100, (unsigned)d.conn_itvl, (unsigned)d.conn_latency,
             (unsigned)d.supervision_timeout * 10u);
}

static void schedule_scan(uint32_t ms)
{
    ble_npl_callout_reset(&s_scan_co, ble_npl_time_ms_to_ticks32(ms));
}

static bool any_slot_free(void);
static bool scan_needed(void);

static void scan_co_cb(struct ble_npl_event *ev)
{
    (void)ev;
    if (!s_connecting && scan_needed()) {
        start_scan();
    }
}

static bool any_slot_free(void)
{
    for (int i = 0; i < BLE_MIDI_MAX_CONN; i++) {
        if (!s_slots[i].used) {
            return true;
        }
    }
    return false;
}

/* Registry and control state are owned by the NimBLE host task. */
static midi_registry_t s_registry;
static bool s_disabled[MIDI_REGISTRY_MAX];
static uint8_t s_device_state[MIDI_REGISTRY_MAX];
static uint32_t s_retry_after[MIDI_REGISTRY_MAX];
static bool s_scan_screen, s_registry_migrated, s_delete_confirm, s_delete_yes;
static unsigned s_delete_index;
static unsigned s_selected, s_saved_selection;
static char s_ui_message[24];
static bool s_synced;
static QueueHandle_t s_control_queue, s_save_queue;
static StaticQueue_t s_control_queue_storage, s_save_queue_storage;
static uint8_t s_control_queue_data[16*sizeof(int)];
typedef struct { midi_registry_t registry; uint32_t sequence; } save_request_t;
static uint8_t s_save_queue_data[sizeof(save_request_t)];
static uint32_t s_save_sequence;
static uint32_t s_saved_sequence;
static int s_save_result;
static struct ble_npl_event s_control_event, s_saved_event, s_serial_event;
static QueueHandle_t s_serial_commands,s_serial_replies;
static StaticQueue_t s_serial_command_storage,s_serial_reply_storage;
static uint8_t s_serial_command_data[sizeof(midi_serial_command_t)];
static uint8_t s_serial_reply_data[sizeof(midi_serial_response_t)];
static midi_serial_response_t s_serial_response;
static bool s_serial_remove_pending;
static midi_peer_t s_serial_remove_peer;
static void collect_stats(ble_midi_client_stats_t *out);
static uint32_t now_ms(void) { return (uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS); }
static midi_peer_t peer_from_addr(const ble_addr_t *addr,const char *name)
{
 midi_peer_t p={.type=addr->type}; memcpy(p.addr,addr->val,6);
 snprintf(p.name,sizeof p.name,"%s",name?name:""); return p;
}
static bool has_pending_devices(void)
{
 for(unsigned i=0;i<s_registry.count;i++) if(!s_disabled[i] && s_device_state[i]!=1) return true;
 return false;
}
static bool scan_needed(void) { return s_scan_screen || has_pending_devices(); }
static void save_registry(void)
{
 save_request_t save={.registry=s_registry,.sequence=++s_save_sequence};
 xQueueOverwrite(s_save_queue,&save);
}
static void saved_cb(struct ble_npl_event *event)
{
 (void)event;
 if(__atomic_load_n(&s_saved_sequence,__ATOMIC_ACQUIRE)==s_save_sequence) {
  if(__atomic_load_n(&s_save_result,__ATOMIC_ACQUIRE)!=ESP_OK) snprintf(s_ui_message,sizeof s_ui_message,"SAVE ERROR");
  else s_ui_message[0]=0;
  publish_ui();
 }
}
static bool save_quiet(void)
{
 uint32_t last=__atomic_load_n(&s_last_persist_activity,__ATOMIC_RELAXED);
 return (uint32_t)(esp_cpu_get_cycle_count()-last)>=500000u*CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ && midi_uart_is_idle();
}
/* Flash persistence is deferred and never performed by a GATT callback. */
static void save_task(void *arg)
{
 (void)arg; save_request_t save;
 for(;;) {
  if(xQueueReceive(s_save_queue,&save,portMAX_DELAY)!=pdTRUE) continue;
  while(!save_quiet()) { vTaskDelay(pdMS_TO_TICKS(20)); xQueueReceive(s_save_queue,&save,0); }
  xQueueReceive(s_save_queue,&save,0); /* coalesce edits before opening flash */
  nvs_handle_t handle; esp_err_t err=nvs_open("midi_peer",NVS_READWRITE,&handle);
  if(err==ESP_OK) {
   err=nvs_set_blob(handle,"registry",&save.registry,sizeof save.registry);
   if(err==ESP_OK) err=nvs_commit(handle);
   nvs_close(handle);
  }
  __atomic_store_n(&s_save_result,err,__ATOMIC_RELEASE);
  __atomic_store_n(&s_saved_sequence,save.sequence,__ATOMIC_RELEASE);
  ESP_LOGI(TAG,"Registry save: %u peers (%s)",save.registry.count,esp_err_to_name(err));
  ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&s_saved_event);
 }
}
static void load_registry(void)
{
 memset(&s_registry,0,sizeof s_registry);s_registry.version=MIDI_REGISTRY_VERSION;
 nvs_handle_t handle;
 if(nvs_open("midi_peer",NVS_READONLY,&handle)!=ESP_OK) return;
 midi_registry_t loaded; size_t length=sizeof loaded;
 if(nvs_get_blob(handle,"registry",&loaded,&length)==ESP_OK && length==sizeof loaded && midi_registry_valid(&loaded)) s_registry=loaded;
 else {
  /* Migrate the existing single-device target without erasing its registration. */
  ble_addr_t addr;length=sizeof addr;char name[32]={0};size_t size=sizeof name;
  if(nvs_get_blob(handle,"addr",&addr,&length)==ESP_OK && length==sizeof addr) {
   nvs_get_str(handle,"name",name,&size);
   midi_peer_t peer=peer_from_addr(&addr,name);
   if(midi_addr_stable(&peer)) {midi_registry_add(&s_registry,&peer);s_registry_migrated=true;}
  }
 }
 nvs_close(handle);
 ESP_LOGI(TAG,"Loaded %u registered peers",s_registry.count);
}
/* UI snapshot contains only the three visible rows; no radio/UI work in MIDI RX. */
static void refresh_ui_list(void)
{
 s_link.ui_rows=s_link.ui_connected=0;
 s_link.ui_scan=s_scan_screen;
 s_link.ui_delete_confirm=s_delete_confirm;s_link.ui_delete_yes=s_delete_yes;
 snprintf(s_link.ui_delete_name,sizeof s_link.ui_delete_name,"%s",s_delete_confirm?s_registry.peer[s_delete_index].name:"");
 s_link.ui_selected=0;
 memset(s_link.ui_names,0,sizeof s_link.ui_names);
 memset(s_link.ui_status,0,sizeof s_link.ui_status);
 snprintf(s_link.ui_message,sizeof s_link.ui_message,"%s",s_ui_message);
 s_link.ui_saving=s_save_sequence!=__atomic_load_n(&s_saved_sequence,__ATOMIC_ACQUIRE);
 for(unsigned i=0;i<s_registry.count;i++) if(s_device_state[i]==1) s_link.ui_connected++;
 s_link.connected=s_link.ui_connected>0;
 unsigned count=0;
 if(s_scan_screen) {for(int i=0;i<s_seen_n;i++) if(s_seen[i].midi_logged) count++;}
 else count=s_registry.count;
 if(s_selected>=count) s_selected=count?count-1:0;
 s_link.ui_total=(uint8_t)count;
 unsigned first=(s_selected/3)*3;
 s_link.ui_selected=(uint8_t)(s_selected-first);
 for(unsigned index=first;index<count && s_link.ui_rows<3;index++) {
  unsigned row=s_link.ui_rows++;
  if(s_scan_screen) {
   unsigned midi=0;int found=-1;
   for(int i=0;i<s_seen_n;i++) if(s_seen[i].midi_logged && midi++==index) {found=i;break;}
   if(found>=0) {
    snprintf(s_link.ui_names[row],32,"%s",s_seen[found].name[0]?s_seen[found].name:"Unknown MIDI");
    s_link.ui_rssi[row]=s_seen[found].rssi;
   }
  } else {
   const char *name=s_registry.peer[index].name;
   snprintf(s_link.ui_names[row],32,"%s",name[0]?name:"Unknown MIDI");
   s_link.ui_status[row]=s_device_state[index];
  }
 }
}

static conn_slot_t *slot_for_registered(unsigned index)
{
 for(int i=0;i<BLE_MIDI_MAX_CONN;i++) if(s_slots[i].used && s_slots[i].registered==(int)index) return &s_slots[i];
 return NULL;
}
static void connection_failed(conn_slot_t *slot)
{
 if(slot && slot->registered>=0) {
  s_device_state[slot->registered]=3;
  s_retry_after[slot->registered]=now_ms()+3000;
 }
}
static void connect_registered(unsigned index,const ble_addr_t *address)
{
 if(s_connecting || slot_for_registered(index) || !any_slot_free()) return;
 conn_slot_t *slot=slot_alloc(); if(!slot) return;
 slot->registered=(int)index;
 ble_gap_disc_cancel();
 ble_npl_callout_stop(&s_scan_co);
 const struct ble_gap_conn_params cp={.scan_itvl=BLE_FAST_SCAN_UNITS,.scan_window=BLE_FAST_SCAN_UNITS,
  .itvl_min=BLE_CONN_ITVL_MIN,.itvl_max=BLE_CONN_ITVL_MAX,.latency=BLE_CONN_LATENCY,.supervision_timeout=BLE_CONN_SUP_TIMEOUT};
 s_connecting=true;s_device_state[index]=2;
 snprintf(s_link.peer_name,sizeof s_link.peer_name,"%s",s_registry.peer[index].name);
 addr_str(address,s_link.peer_addr);s_link.interval_units=0;s_link.mtu=23;
 int rc=ble_gap_connect(s_own_addr_type,address,BLE_DIRECT_TIMEOUT_MS,&cp,gap_event,slot);
 if(rc) {connection_failed(slot);slot->used=false;s_connecting=false;schedule_scan(0);}
 set_state(BLE_MIDI_CONNECTING);
 ESP_LOGI(TAG,"Connect registered %u %s (rc=%d)",index,s_registry.peer[index].name,rc);
}
static seen_t *selected_scan_peer(void)
{
 unsigned index=0;
 for(int i=0;i<s_seen_n;i++) if(s_seen[i].midi_logged && index++==s_selected) return &s_seen[i];
 return NULL;
}
/* Host task owns removal and slot remapping; detached callbacks cannot index a removed peer. */
static bool delete_registered(unsigned index)
{
 if(index>=s_registry.count) return false;
 conn_slot_t *slot=slot_for_registered(index);
 if(slot) {
  int rc=slot->conn==BLE_HS_CONN_HANDLE_NONE?ble_gap_conn_cancel():ble_gap_terminate(slot->conn,BLE_ERR_REM_USER_CONN_TERM);
  if(rc) {snprintf(s_ui_message,sizeof s_ui_message,"DELETE ERROR");return false;}
 }
 ESP_LOGI(TAG,"Remove registered %u %s",index,s_registry.peer[index].name);
 midi_registry_remove(&s_registry,index);
 for(int i=0;i<BLE_MIDI_MAX_CONN;i++) s_slots[i].registered=midi_registered_after_remove(s_slots[i].registered,index);
 for(unsigned i=index;i<s_registry.count;i++) {
  s_disabled[i]=s_disabled[i+1];s_device_state[i]=s_device_state[i+1];s_retry_after[i]=s_retry_after[i+1];
 }
 s_disabled[s_registry.count]=false;s_device_state[s_registry.count]=0;s_retry_after[s_registry.count]=0;
 if(s_selected>=s_registry.count) s_selected=s_registry.count?s_registry.count-1:0;
 save_registry();
 if(!scan_needed()) {ble_gap_disc_cancel();ble_npl_callout_stop(&s_scan_co);}
 else if(!s_connecting) schedule_scan(0);
 return true;
}
static void control_cb(struct ble_npl_event *event)
{
 (void)event; int action;
 while(xQueueReceive(s_control_queue,&action,0)==pdTRUE) {
  s_ui_message[0]=0;
  if(s_delete_confirm) {
   if(action==MIDI_CONTROL_NEXT || action==MIDI_CONTROL_PREV) s_delete_yes=!s_delete_yes;
   else if(action==MIDI_CONTROL_HOLD) s_delete_confirm=false;
   else if(action==MIDI_CONTROL_PUSH) {
    if(!s_delete_yes || delete_registered(s_delete_index)) s_delete_confirm=false;
   }
   /* A double click inside confirmation never confirms deletion. */
   publish_ui();continue;
  }
  if(action==MIDI_CONTROL_DOUBLE) {
   if(!s_scan_screen && s_registry.count) {
    s_delete_index=s_selected;s_delete_yes=false;s_delete_confirm=true;
    ESP_LOGI(TAG,"Delete confirmation: %s",s_registry.peer[s_delete_index].name);
   }
  } else if(action==MIDI_CONTROL_NEXT || action==MIDI_CONTROL_PREV) {
   unsigned count=s_registry.count;
   if(s_scan_screen) {count=0;for(int i=0;i<s_seen_n;i++) if(s_seen[i].midi_logged) count++;}
   s_selected=midi_selection_move(s_selected,count,action==MIDI_CONTROL_NEXT?1:-1);
  } else if(action==MIDI_CONTROL_HOLD) {
   if(s_scan_screen) {s_scan_screen=false;s_selected=s_saved_selection;if(!scan_needed()) ble_gap_disc_cancel();}
   else {s_saved_selection=s_selected;s_selected=0;s_scan_screen=true;memset(s_seen,0,sizeof s_seen);s_seen_n=0;if(!s_connecting) start_scan();}
   ESP_LOGI(TAG,"UI screen: %s",s_scan_screen?"SCAN":"DEVICES");
  } else if(action==MIDI_CONTROL_PUSH && s_scan_screen) {
   seen_t *seen=selected_scan_peer();
   if(seen) {
    midi_peer_t peer=peer_from_addr(&seen->addr,seen->name);
    unsigned before=s_registry.count;int index=midi_registry_add(&s_registry,&peer);
    if(index<0) snprintf(s_ui_message,sizeof s_ui_message,index==-1?"REGISTRY FULL":"NAME REQUIRED");
    else {
     s_disabled[index]=false;s_retry_after[index]=0;
     if(s_registry.count!=before) {save_registry();ESP_LOGI(TAG,"Added registered peer %s",peer.name);}
     if(!s_connecting) connect_registered((unsigned)index,&seen->addr);
    }
   }
  } else if(action==MIDI_CONTROL_PUSH && s_registry.count) {
   conn_slot_t *slot=slot_for_registered(s_selected);
   if(slot) {
    s_disabled[s_selected]=true;
    int rc=slot->conn==BLE_HS_CONN_HANDLE_NONE?ble_gap_conn_cancel():ble_gap_terminate(slot->conn,BLE_ERR_REM_USER_CONN_TERM);
    if(rc) {s_disabled[s_selected]=false;snprintf(s_ui_message,sizeof s_ui_message,"DISCONNECT ERROR");}
   } else {
    s_disabled[s_selected]=false;s_retry_after[s_selected]=0;s_device_state[s_selected]=2;
    if(!s_connecting && midi_addr_stable(&s_registry.peer[s_selected])) {
     ble_addr_t addr={.type=s_registry.peer[s_selected].type};memcpy(addr.val,s_registry.peer[s_selected].addr,6);
     connect_registered(s_selected,&addr);
    } else if(!s_connecting) start_scan();
   }
  }
  publish_ui();
 }
}
void ble_midi_client_control(int action)
{
 if(!__atomic_load_n(&s_synced,__ATOMIC_ACQUIRE) || action<MIDI_CONTROL_NEXT || action>MIDI_CONTROL_DOUBLE) return;
 if(xQueueSend(s_control_queue,&action,0)==pdTRUE) ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&s_control_event);
}

static void serial_cb(struct ble_npl_event *event)
{
 (void)event;midi_serial_command_t command;
 if(xQueueReceive(s_serial_commands,&command,0)!=pdTRUE)return;
 midi_serial_response_t *r=&s_serial_response;memset(r,0,sizeof *r);r->ok=true;
 snprintf(r->message,sizeof r->message,"OK");unsigned index=command.index;
 if((command.op==SERIAL_CONNECT || command.op==SERIAL_DISCONNECT || command.op==SERIAL_REMOVE) && index>=s_registry.count){
  r->ok=false;snprintf(r->message,sizeof r->message,"Invalid registered index (use list)");
 } else if(command.op==SERIAL_SCAN_ON){
  if(!s_scan_screen){s_saved_selection=s_selected;s_selected=0;s_scan_screen=true;memset(s_seen,0,sizeof s_seen);s_seen_n=0;}
  if(!s_connecting)start_scan();
 } else if(command.op==SERIAL_SCAN_OFF){
  s_scan_screen=false;s_selected=s_saved_selection;if(!scan_needed())ble_gap_disc_cancel();
 } else if(command.op==SERIAL_CONNECT){
  s_disabled[index]=false;s_retry_after[index]=0;
  if(!slot_for_registered(index)){
   s_device_state[index]=2;
   if(!s_connecting && midi_addr_stable(&s_registry.peer[index])){
    ble_addr_t addr={.type=s_registry.peer[index].type};memcpy(addr.val,s_registry.peer[index].addr,6);connect_registered(index,&addr);
   }else if(!s_connecting)start_scan();
  }
 } else if(command.op==SERIAL_DISCONNECT){
  conn_slot_t *slot=slot_for_registered(index);bool previous=s_disabled[index];s_disabled[index]=true;
  int rc=slot?(slot->conn==BLE_HS_CONN_HANDLE_NONE?ble_gap_conn_cancel():ble_gap_terminate(slot->conn,BLE_ERR_REM_USER_CONN_TERM)):0;
  if(rc){s_disabled[index]=previous;r->ok=false;snprintf(r->message,sizeof r->message,"Disconnect failed rc=%d",rc);}
  else if(!slot)s_device_state[index]=0;
 } else if(command.op==SERIAL_ADD){
  seen_t *seen=NULL;unsigned n=0;for(int i=0;i<s_seen_n;i++)if(s_seen[i].midi_logged && n++==index){seen=&s_seen[i];break;}
  if(!seen){r->ok=false;snprintf(r->message,sizeof r->message,"Invalid scan index (use found)");}
  else {midi_peer_t peer=peer_from_addr(&seen->addr,seen->name);unsigned before=s_registry.count;int added=midi_registry_add(&s_registry,&peer);
   if(added<0){r->ok=false;snprintf(r->message,sizeof r->message,added==-1?"Registry full":"Private peer needs name");}
   else{s_disabled[added]=false;s_retry_after[added]=0;if(before!=s_registry.count)save_registry();if(!s_connecting)connect_registered((unsigned)added,&seen->addr);}
  }
 } else if(command.op==SERIAL_REMOVE){
  s_serial_remove_peer=s_registry.peer[index];s_serial_remove_pending=true;
  snprintf(r->message,sizeof r->message,"Remove %.31s? Type confirm or cancel",s_serial_remove_peer.name);
 } else if(command.op==SERIAL_CONFIRM){
  int found=-1;for(unsigned i=0;i<s_registry.count;i++)if(midi_peer_same(&s_registry.peer[i],&s_serial_remove_peer))found=(int)i;
  if(!s_serial_remove_pending || found<0){r->ok=false;s_serial_remove_pending=false;snprintf(r->message,sizeof r->message,"No valid pending removal");}
  else if(delete_registered((unsigned)found)){
   s_serial_remove_pending=false;s_delete_confirm=false;snprintf(r->message,sizeof r->message,"Removed; NVS save queued");
  } else{r->ok=false;snprintf(r->message,sizeof r->message,"%s",s_ui_message);}
 } else if(command.op==SERIAL_CANCEL){s_serial_remove_pending=false;snprintf(r->message,sizeof r->message,"Removal cancelled");}
 if(command.op==SERIAL_STATUS || command.op==SERIAL_LIST || command.op==SERIAL_FOUND)refresh_ui_list();else publish_ui();
 collect_stats(&r->stats);
 r->scan=s_scan_screen;r->saving=s_save_sequence!=__atomic_load_n(&s_saved_sequence,__ATOMIC_ACQUIRE);
 r->remove_pending=s_serial_remove_pending;snprintf(r->remove_name,sizeof r->remove_name,"%s",s_serial_remove_pending?s_serial_remove_peer.name:"");
 r->registered_count=s_registry.count;
 for(unsigned i=0;i<s_registry.count;i++){
  midi_serial_peer_t *peer=&r->registered[i];snprintf(peer->name,sizeof peer->name,"%s",s_registry.peer[i].name);
  ble_addr_t addr={.type=s_registry.peer[i].type};memcpy(addr.val,s_registry.peer[i].addr,6);addr_str(&addr,peer->addr);
  peer->state=s_device_state[i];peer->disabled=s_disabled[i];conn_slot_t *slot=slot_for_registered(i);
  if(slot && slot->conn!=BLE_HS_CONN_HANDLE_NONE){struct ble_gap_conn_desc d;if(!ble_gap_conn_find(slot->conn,&d)){peer->interval_units=d.conn_itvl;peer->mtu=ble_att_mtu(slot->conn);}}
 }
 for(int i=0;i<s_seen_n && r->found_count<24;i++)if(s_seen[i].midi_logged){midi_serial_peer_t *peer=&r->found[r->found_count++];snprintf(peer->name,sizeof peer->name,"%s",s_seen[i].name[0]?s_seen[i].name:"Unknown MIDI");addr_str(&s_seen[i].addr,peer->addr);peer->rssi=s_seen[i].rssi;}
 xQueueOverwrite(s_serial_replies,r);
}
bool ble_midi_client_serial(const midi_serial_command_t *command,midi_serial_response_t *response)
{
 if(!__atomic_load_n(&s_synced,__ATOMIC_ACQUIRE))return false;
 if(xQueueSend(s_serial_commands,command,0)!=pdTRUE)return false;
 ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&s_serial_event);
 return xQueueReceive(s_serial_replies,response,portMAX_DELAY)==pdTRUE;
}

static void link_fail(conn_slot_t *s, const char *what, int rc)
{
    connection_failed(s);
    ESP_LOGE(TAG, "%s failed (rc=%d) - dropping connection", what, rc);
    if (ble_gap_terminate(s->conn, BLE_ERR_REM_USER_CONN_TERM) != 0) {
        /* no DISCONNECT event will follow */
        s->used = false;
        s_connecting = false;
        schedule_scan(500);
    }
    set_state(BLE_MIDI_RETRYING);
}

/* ---- GATT discovery chain ---------------------------------------------------- */

static void write_cccd(conn_slot_t *s);

static int on_cccd_write(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    conn_slot_t *s = arg;
    if (error->status == 0) {
        s->ready = true;
        s_link.state = BLE_MIDI_READY;
        s_connecting = false;
        s_cs.connections++;
        if(s->registered>=0) s_device_state[s->registered]=1;
        if(scan_needed()) schedule_scan(0);
        ESP_LOGI(TAG, "Notifications enabled - BLE MIDI ready");
        log_conn_params("Actual", conn);
        publish_ui();
        ESP_LOGI(TAG, "PHY: default LE 1M (no PHY update requested)");
        return 0;
    }
    if ((error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
         error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) &&
        !s->sec_tried) {
        s->sec_tried = true;
        ESP_LOGW(TAG, "CCCD write needs encryption, starting pairing");
        int rc = ble_gap_security_initiate(conn);
        if (rc == 0) {
            return 0; /* retry in ENC_CHANGE */
        }
        link_fail(s, "security initiate", rc);
        return 0;
    }
    link_fail(s, "Notification enable (CCCD write)", error->status);
    return 0;
}

static void write_cccd(conn_slot_t *s)
{
    set_state(BLE_MIDI_SUBSCRIBING);
    static const uint8_t on[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(s->conn, s->cccd, on, sizeof(on), on_cccd_write, s);
    if (rc != 0) {
        link_fail(s, "CCCD write start", rc);
    }
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn;
    (void)chr_val_handle;
    conn_slot_t *s = arg;
    if (error->status == 0) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && BLE_UUID16(&dsc->uuid)->value == BLE_GATT_DSC_CLT_CFG_UUID16 &&
            s->cccd == 0) {
            s->cccd = dsc->handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (s->cccd == 0) {
            link_fail(s, "CCCD discovery (no 0x2902)", error->status);
        } else {
            write_cccd(s);
        }
    } else {
        link_fail(s, "Descriptor discovery", error->status);
    }
    return 0;
}

static void start_descriptor_discovery(conn_slot_t *s)
{
    int rc = ble_gattc_disc_all_dscs(s->conn, s->chr_val, s->chr_end, on_dsc, s);
    if (rc != 0) link_fail(s, "Descriptor discovery start", rc);
}

static int on_midi_read(uint16_t conn, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    if (error->status != 0) {
        ESP_LOGW(TAG, "MIDI handshake read failed (%d); trying notification subscription", error->status);
    }
    start_descriptor_discovery(arg);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    conn_slot_t *s = arg;
    if (error->status == 0) {
        if (s->chr_found && chr->def_handle > s->chr_val && chr->def_handle <= s->chr_end) {
            s->chr_end = chr->def_handle - 1;
        }
        if (!s->chr_found && ble_uuid_cmp(&chr->uuid.u, &UUID_MIDI_CHR.u) == 0) {
            s->chr_found = true;
            s->chr_val = chr->val_handle;
            s->chr_end = s->svc_end;
            s->chr_props = chr->properties;
            ESP_LOGI(TAG, "MIDI I/O characteristic: val_handle=%u props=0x%02x", (unsigned)chr->val_handle,
                     (unsigned)chr->properties);
            if (!(chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
                link_fail(s, "MIDI characteristic lacks NOTIFY property", BLE_HS_ENOTSUP);
                return BLE_HS_ENOTSUP;
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (!s->chr_found) {
            link_fail(s, "MIDI characteristic discovery", error->status);
            return 0;
        }
        /* BLE-MIDI's initial read handshake precedes CCCD subscription. */
        if ((s->chr_props & BLE_GATT_CHR_PROP_READ) &&
            ble_gattc_read(conn, s->chr_val, on_midi_read, s) == 0) return 0;
        start_descriptor_discovery(s);
    } else {
        link_fail(s, "Characteristic discovery", error->status);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
    conn_slot_t *s = arg;
    if (error->status == 0) {
        s->svc_found = true;
        s->svc_start = svc->start_handle;
        s->svc_end = svc->end_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!s->svc_found) {
            link_fail(s, "BLE MIDI service discovery", error->status);
            return 0;
        }
        ESP_LOGI(TAG, "BLE MIDI service handles %u..%u", (unsigned)s->svc_start, (unsigned)s->svc_end);
        int rc = ble_gattc_disc_all_chrs(conn, s->svc_start, s->svc_end, on_chr, s);
        if (rc != 0) {
            link_fail(s, "Characteristic discovery start", rc);
        }
    } else {
        link_fail(s, "Service discovery", error->status);
    }
    return 0;
}

static void start_svc_discovery(conn_slot_t *s)
{
    int rc = ble_gattc_disc_svc_by_uuid(s->conn, &UUID_MIDI_SVC.u, on_svc, s);
    if (rc != 0) {
        link_fail(s, "Service discovery start", rc);
    }
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    (void)conn;
    conn_slot_t *s = arg;
    if (error->status == 0) {
        s_link.mtu = mtu;
        ESP_LOGI(TAG, "MTU exchanged: %u", (unsigned)mtu);
    } else {
        ESP_LOGW(TAG, "MTU exchange failed (%d), continuing with default", error->status);
    }
    start_svc_discovery(s);
    return 0;
}

/* ---- scanning / connecting ---------------------------------------------------- */

static bool seen_before(const ble_addr_t *a, bool has_midi, bool *first_midi)
{
    *first_midi = false;
    for (int i = 0; i < s_seen_n; i++) {
        if (s_seen[i].addr.type == a->type && memcmp(s_seen[i].addr.val, a->val, 6) == 0) {
            if (has_midi && !s_seen[i].midi_logged) {
                s_seen[i].midi_logged = true;
                *first_midi = true;
            }
            return true;
        }
    }
    if (s_seen_n < SEEN_MAX) {
        memset(&s_seen[s_seen_n],0,sizeof(s_seen[s_seen_n]));
        s_seen[s_seen_n].addr = *a;
        s_seen[s_seen_n].midi_logged = has_midi;
        s_seen_n++;
    }
    if (has_midi) {
        *first_midi = true;
    }
    return false;
}

static void handle_disc(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return;
    }
    bool has_midi = false;
    for (int i = 0; i < f.num_uuids128; i++) {
        if (ble_uuid_cmp(&f.uuids128[i].u, &UUID_MIDI_SVC.u) == 0) {
            has_midi = true;
            break;
        }
    }

    if(!has_midi) {
        bool retained=false;
        for(int i=0;i<s_seen_n;i++) if(s_seen[i].addr.type==d->addr.type && memcmp(s_seen[i].addr.val,d->addr.val,6)==0) {retained=true;break;}
        midi_peer_t peer=peer_from_addr(&d->addr,"");
        if(!retained && midi_registry_find(&s_registry,&peer)<0) return;
    }
    bool first_midi;
    bool seen = seen_before(&d->addr, has_midi, &first_midi);
    bool ui_changed=first_midi;
    seen_t *entry = NULL;
    for (int i=0; i<s_seen_n; i++) {
        if (s_seen[i].addr.type==d->addr.type && memcmp(s_seen[i].addr.val,d->addr.val,6)==0) {
            entry=&s_seen[i];
            if (f.name && f.name_len) {
                size_t n=f.name_len<sizeof(entry->name)-1 ? f.name_len : sizeof(entry->name)-1;
                char name[32]={0}; memcpy(name,f.name,n);
                if (strcmp(name,entry->name)!=0 && entry->midi_logged) ui_changed=true;
                memcpy(entry->name,name,sizeof name);
            }
            break;
        }
    }
    if (!seen || first_midi) {
        char mac[18];
        char name[32] = "";
        if (f.name && f.name_len) {
            size_t n = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
            memcpy(name, f.name, n);
            name[n] = 0;
        }
        addr_str(&d->addr, mac);
        ESP_LOGI(TAG, "Scan: name=\"%s\" addr=%s(type %u) RSSI=%d BLE-MIDI-Service=%s", name, mac,
                 (unsigned)d->addr.type, d->rssi, has_midi ? "YES" : "no");
    }

    if(entry) entry->rssi=d->rssi;
    if(ui_changed && s_scan_screen) publish_ui();
    char name[32]={0};
    if(entry) snprintf(name,sizeof name,"%s",entry->name);
    midi_peer_t peer=peer_from_addr(&d->addr,name);
    int index=midi_registry_find(&s_registry,&peer);
    if(index<0 || s_disabled[index] || slot_for_registered((unsigned)index) || s_connecting || !any_slot_free()) return;
    if((int32_t)(now_ms()-s_retry_after[index])<0) return;
    if(d->event_type!=BLE_HCI_ADV_RPT_EVTYPE_ADV_IND && d->event_type!=BLE_HCI_ADV_RPT_EVTYPE_DIR_IND && d->event_type!=BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) return;
    connect_registered((unsigned)index,&d->addr);
}

static void start_scan(void)
{
    if(!scan_needed() || s_connecting) return;
    const struct ble_gap_disc_params dp = {
        .itvl = BLE_FAST_SCAN_UNITS,
        .window = BLE_FAST_SCAN_UNITS,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0, /* active: scan response may carry name / service UUID */
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed rc=%d, retrying", rc);
        schedule_scan(500);
    } else {
        set_state(BLE_MIDI_SCANNING);
        ESP_LOGI(TAG, "Scanning for BLE MIDI devices...");
    }
}

/* ---- GAP events --------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    /* ===== HOT PATH: GATT notification -> parser -> DIN MIDI ===== */
    if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
        latency_debug_rx_enter();
        conn_slot_t *s = slot_by_conn(event->notify_rx.conn_handle);
        if (s && s->ready && event->notify_rx.attr_handle == s->chr_val) {
            struct os_mbuf *om = event->notify_rx.om;
            s_cs.notifications++;
            if (SLIST_NEXT(om, om_next) == NULL) {
                mark_persist_activity(om->om_data,om->om_len);
                ble_midi_parser_feed(&s->parser, om->om_data, om->om_len);
            } else {
                uint16_t n = os_mbuf_len(om);
                if (n <= sizeof(s_rx_copy) && os_mbuf_copydata(om, 0, n, s_rx_copy) == 0) {
                    mark_persist_activity(s_rx_copy,n);
                    ble_midi_parser_feed(&s->parser, s_rx_copy, n);
                } else {
                    s_cs.notif_dropped++;
                }
            }
        } else {
            s_cs.notif_dropped++;
        }
        latency_debug_rx_exit();
        return 0; /* mbuf is freed by the host */
    }

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_disc(&event->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!s_connecting && scan_needed()) {
            schedule_scan(100);
        }
        return 0;

    case BLE_GAP_EVENT_CONNECT: {
        conn_slot_t *s = arg;
        if (event->connect.status != 0 || s == NULL) {
            ESP_LOGE(TAG, "Connection failed, status=%d", event->connect.status);
            if (s) {
                if(s->registered>=0 && s_disabled[s->registered]) s_device_state[s->registered]=0;
                else connection_failed(s);
                s->used = false;
            }
            publish_ui();
            s_connecting = false;
            schedule_scan(0);
            return 0;
        }
        s->conn = event->connect.conn_handle;
        if(s->registered<0) {
            /* A cancel can race with successful CONNECT for a deleted peer. */
            s_connecting=false;
            ble_gap_terminate(s->conn,BLE_ERR_REM_USER_CONN_TERM);
            if(scan_needed()) schedule_scan(0);
            return 0;
        }
        set_state(BLE_MIDI_DISCOVERING);
        s->sec_tried = false;
        ble_midi_parser_reset(&s->parser);
        ESP_LOGI(TAG, "Connected (handle %u)", (unsigned)s->conn);
        struct ble_gap_conn_desc peer;
        if (ble_gap_conn_find(s->conn, &peer) == 0) {
            char mac[18];
            addr_str(&peer.peer_ota_addr, mac);
            ESP_LOGI(TAG, "Connected device: %s", mac);
        }
        log_conn_params("Initial", s->conn);

        /* If the peripheral did not take 7.5 ms, ask again with an update. */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(s->conn, &d) == 0 && (d.conn_itvl > BLE_CONN_ITVL_MAX || d.conn_latency != 0)) {
            const struct ble_gap_upd_params up = {
                .itvl_min = BLE_CONN_ITVL_MIN,
                .itvl_max = BLE_CONN_ITVL_MAX,
                .latency = BLE_CONN_LATENCY,
                .supervision_timeout = BLE_CONN_SUP_TIMEOUT,
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            int rc = ble_gap_update_params(s->conn, &up);
            ESP_LOGI(TAG, "Connection parameter update requested (rc=%d)", rc);
        }
        int rc = ble_gattc_exchange_mtu(s->conn, on_mtu, s);
        if (rc != 0) {
            start_svc_discovery(s);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        conn_slot_t *s = slot_by_conn(event->disconnect.conn.conn_handle);
        ESP_LOGW(TAG, "Disconnected, reason=0x%x", (unsigned)event->disconnect.reason);
        bool was_ready=s && s->ready;
        if (s) {
            ble_midi_parser_reset(&s->parser); /* closes an open SysEx */
            if(s->registered>=0 && (s_disabled[s->registered] || s_device_state[s->registered]!=3)) s_device_state[s->registered]=0;
            s->used = false;
            s->ready = false;
        }
        s_link.interval_units = 0;
        s_cs.disconnects++;
        if(s && !was_ready) s_connecting = false;
        /* Start recovery first; UI only receives a nonblocking state copy. */
        begin_recovery();
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Connection update done, status=%d", event->conn_update.status);
        log_conn_params("Actual", event->conn_update.conn_handle);
        publish_ui();
        return 0;

    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
        /* IDF v6.1 supplies peer_params but NO self_params for this event.
         * Returning zero accepts the peer range as-is. Keep the proven low
         * latency interval rather than silently allowing a longer one. */
        const struct ble_gap_upd_params *p=event->conn_update_req.peer_params;
        if(!p || p->itvl_min!=BLE_CONN_ITVL_MIN || p->itvl_max!=BLE_CONN_ITVL_MAX || p->latency!=BLE_CONN_LATENCY) {
            ESP_LOGI(TAG,"Reject L2CAP interval change; retaining low-latency connection");
            return BLE_ERR_CONN_PARMS;
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        /* Peripheral asks for other parameters: counter with our low-latency ones. */
        *event->conn_update_req.self_params = *event->conn_update_req.peer_params;
        event->conn_update_req.self_params->itvl_min = BLE_CONN_ITVL_MIN;
        event->conn_update_req.self_params->itvl_max = BLE_CONN_ITVL_MAX;
        event->conn_update_req.self_params->latency = BLE_CONN_LATENCY;
        event->conn_update_req.self_params->supervision_timeout = BLE_CONN_SUP_TIMEOUT;
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_link.mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU update: %u", (unsigned)event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        conn_slot_t *s = slot_by_conn(event->enc_change.conn_handle);
        ESP_LOGI(TAG, "Encryption change status=%d", event->enc_change.status);
        if (s && !s->ready && s->cccd) {
            if (event->enc_change.status == 0) {
                write_cccd(s);
            } else {
                link_fail(s, "pairing", event->enc_change.status);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &d) == 0) {
            ble_store_util_delete_peer(&d.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        return 0;
    }
}


static void begin_recovery(void)
{
    if(!s_connecting && scan_needed()) start_scan();
    publish_ui();
}

/* ---- NimBLE bring-up ---------------------------------------------------------- */

static void on_reset(int reason)
{
    ble_npl_callout_stop(&s_scan_co);
    ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason);
    for (int i = 0; i < BLE_MIDI_MAX_CONN; i++) {
        ble_midi_parser_reset(&s_slots[i].parser);
        s_slots[i].used = false;
        s_slots[i].ready = false;
    }
    memset(s_device_state,0,sizeof s_device_state);
    __atomic_store_n(&s_synced,false,__ATOMIC_RELEASE);
    s_connecting = false; /* on_sync restarts discovery after host recovery */
    set_state(BLE_MIDI_RETRYING);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "address setup failed rc=%d", rc);
        return;
    }
    __atomic_store_n(&s_synced,true,__ATOMIC_RELEASE);
    begin_recovery();
    ble_npl_callout_reset(&s_snapshot_co, ble_npl_time_ms_to_ticks32(250));
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_midi_client_start(const ble_midi_sink_t *sink)
{
#if OLED_ENABLED
    s_ui_queue = xQueueCreateStatic(1, sizeof(ble_midi_client_stats_t),
                                    s_ui_queue_data, &s_ui_queue_control);
#endif
    set_state(BLE_MIDI_BOOT);
    s_merge.sink = *sink;
    for (int i = 0; i < BLE_MIDI_MAX_CONN; i++) {
        ble_midi_sink_t merged=midi_merge_sink(&s_slots[i].merge_source,&s_merge);
        ble_midi_parser_init(&s_slots[i].parser, &merged);
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) return err;
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    load_registry();
    s_scan_screen=s_registry.count==0;
    err = nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;

    s_control_queue=xQueueCreateStatic(16,sizeof(int),s_control_queue_data,&s_control_queue_storage);
    s_save_queue=xQueueCreateStatic(1,sizeof(save_request_t),s_save_queue_data,&s_save_queue_storage);
    s_serial_commands=xQueueCreateStatic(1,sizeof(midi_serial_command_t),s_serial_command_data,&s_serial_command_storage);
    s_serial_replies=xQueueCreateStatic(1,sizeof(midi_serial_response_t),s_serial_reply_data,&s_serial_reply_storage);
    ble_npl_event_init(&s_serial_event,serial_cb,NULL);
    ble_npl_event_init(&s_control_event,control_cb,NULL);
    ble_npl_event_init(&s_saved_event,saved_cb,NULL);
    if(xTaskCreate(save_task,"peer_save",3072,NULL,1,NULL)!=pdPASS) return ESP_ERR_NO_MEM;
    if(s_registry_migrated) save_registry(); /* migrate only once */
    ble_npl_callout_init(&s_snapshot_co, nimble_port_get_dflt_eventq(), snapshot_cb, NULL);
    ble_npl_callout_init(&s_scan_co, nimble_port_get_dflt_eventq(), scan_co_cb, NULL);

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

static void collect_stats(ble_midi_client_stats_t *out)
{
    *out = s_link;
    out->connected = false;
    out->notifications = s_cs.notifications;
    out->notif_dropped = s_cs.notif_dropped;
    out->connections = s_cs.connections;
    out->disconnects = s_cs.disconnects;
    for (int i = 0; i < BLE_MIDI_MAX_CONN; i++) {
        if (s_slots[i].used && s_slots[i].ready) {
            out->connected = true;
        }
    }
    memset(&out->parser,0,sizeof out->parser);
    for(int i=0;i<BLE_MIDI_MAX_CONN;i++) {
        const ble_midi_parser_stats_t *p=&s_slots[i].parser.stats;
#define ADD(field) out->parser.field+=p->field
        ADD(packets); ADD(short_msgs); ADD(realtime_msgs); ADD(sysex_chunks);
        ADD(err_malformed); ADD(err_orphan_data); ADD(err_incomplete); ADD(err_sysex_abort);
#undef ADD
    }
}

/* Runs in the NimBLE host event queue, serialized with parser and GAP events. */
static void snapshot_cb(struct ble_npl_event *ev)
{
    (void)ev;
    ble_midi_client_stats_t next;
    collect_stats(&next);
    portENTER_CRITICAL(&s_snapshot_lock);
    s_snapshot = next;
    portEXIT_CRITICAL(&s_snapshot_lock);
    ble_npl_callout_reset(&s_snapshot_co, ble_npl_time_ms_to_ticks32(250));
}

void ble_midi_client_get_stats(ble_midi_client_stats_t *out)
{
    portENTER_CRITICAL(&s_snapshot_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_lock);
}

void ble_midi_client_wait_ui(ble_midi_client_stats_t *out)
{
    (void)ble_midi_client_take_ui(out, UINT32_MAX);
}

bool ble_midi_client_take_ui(ble_midi_client_stats_t *out, uint32_t wait_ms)
{
    if (!s_ui_queue) return false;
    TickType_t ticks = wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
    return xQueueReceive(s_ui_queue, out, ticks) == pdTRUE;
}
