#pragma once
#include "ble_midi_parser.h"
typedef struct midi_merge_source midi_merge_source_t;
typedef struct { ble_midi_sink_t sink; midi_merge_source_t *owner; uint32_t interrupted; } midi_merge_t;
struct midi_merge_source { midi_merge_t *merge; bool suppressed; };
/* One DIN stream cannot carry overlapping SysEx streams. A non-realtime event
 * from another source closes the earlier SysEx; its remainder is suppressed.
 * Notes are never held waiting for another device's unfinished SysEx. */
static inline void midi_merge_close(midi_merge_t *m,uint16_t ts)
{
 if(m->owner) {
  const uint8_t end=0xf7;m->owner->suppressed=true;m->owner=NULL;m->interrupted++;
  m->sink.sysex(m->sink.ctx,&end,1,ts);
 }
}
static inline void midi_merge_short(void *ctx,const uint8_t *d,uint8_t len,uint16_t ts)
{
 midi_merge_source_t *source=ctx;midi_merge_t *m=source->merge;
 midi_merge_close(m,ts);m->sink.short_msg(m->sink.ctx,d,len,ts);
}
static inline void midi_merge_rt(void *ctx,uint8_t b,uint16_t ts)
{
 midi_merge_t *m=((midi_merge_source_t*)ctx)->merge;m->sink.realtime(m->sink.ctx,b,ts);
}
static inline void midi_merge_sysex(void *ctx,const uint8_t *d,size_t n,uint16_t ts)
{
 midi_merge_source_t *source=ctx;midi_merge_t *m=source->merge;
 if(!n) return;
 if(d[0]==0xf0) {
  midi_merge_close(m,ts);source->suppressed=false;m->owner=source;
 }
 if(source->suppressed || m->owner!=source) {
  if(d[n-1]==0xf7) source->suppressed=false;
  return;
 }
 m->sink.sysex(m->sink.ctx,d,n,ts);
 if(d[n-1]==0xf7) m->owner=NULL;
}
static inline ble_midi_sink_t midi_merge_sink(midi_merge_source_t *source,midi_merge_t *merge)
{
 source->merge=merge;source->suppressed=false;
 return (ble_midi_sink_t){.short_msg=midi_merge_short,.realtime=midi_merge_rt,.sysex=midi_merge_sysex,.ctx=source};
}
