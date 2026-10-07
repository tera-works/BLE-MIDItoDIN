#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define MIDI_REGISTRY_MAX 4
#define MIDI_REGISTRY_VERSION 1
typedef struct { uint8_t type, addr[6]; char name[32]; } midi_peer_t;
typedef struct { uint32_t version; uint8_t count; midi_peer_t peer[MIDI_REGISTRY_MAX]; } midi_registry_t;
static inline bool midi_addr_stable(const midi_peer_t *p)
{ return p->type==0 || (p->type==1 && (p->addr[5]&0xc0)==0xc0); }
static inline bool midi_peer_same(const midi_peer_t *a,const midi_peer_t *b)
{ return a->type==b->type && memcmp(a->addr,b->addr,6)==0; }
static inline int midi_registry_find(const midi_registry_t *r,const midi_peer_t *p)
{
 for(unsigned i=0;i<r->count;i++) if(midi_peer_same(&r->peer[i],p)) return (int)i;
 /* Private-address fallback only with a unique nonempty saved name. */
 if(midi_addr_stable(p) || !p->name[0]) return -1;
 int found=-1;
 for(unsigned i=0;i<r->count;i++) if(!midi_addr_stable(&r->peer[i]) && strcmp(r->peer[i].name,p->name)==0) {
   if(found>=0) return -1;
   found=(int)i;
 }
 return found;
}
static inline bool midi_registry_valid(const midi_registry_t *r)
{
 if(r->version!=MIDI_REGISTRY_VERSION || r->count>MIDI_REGISTRY_MAX) return false;
 for(unsigned i=0;i<r->count;i++) {
  if(r->peer[i].type>1 || !memchr(r->peer[i].name,0,32) || (!midi_addr_stable(&r->peer[i]) && !r->peer[i].name[0])) return false;
  for(unsigned j=0;j<i;j++) if(midi_peer_same(&r->peer[i],&r->peer[j])) return false;
 }
 return true;
}
/* Returns an existing/new index, -1 full, -2 unidentified private peer. */
static inline int midi_registry_add(midi_registry_t *r,const midi_peer_t *p)
{
 int i=midi_registry_find(r,p); if(i>=0) return i;
 if(!midi_addr_stable(p) && !p->name[0]) return -2;
 if(r->count==MIDI_REGISTRY_MAX) return -1;
 i=r->count++; r->peer[i]=*p; r->peer[i].name[31]=0; return i;
}
static inline unsigned midi_selection_move(unsigned selected,unsigned count,int delta)
{
 if(!count) return 0;
 int next=(int)selected+delta;
 next%=(int)count; if(next<0) next+=(int)count; return (unsigned)next;
}
static inline bool midi_registry_remove(midi_registry_t *r,unsigned index)
{
 if(index>=r->count) return false;
 memmove(&r->peer[index],&r->peer[index+1],(r->count-index-1)*sizeof(r->peer[0]));
 memset(&r->peer[--r->count],0,sizeof(r->peer[0]));return true;
}
static inline int midi_registered_after_remove(int index,unsigned removed)
{ return index<0?index:index==(int)removed?-1:index>(int)removed?index-1:index; }
enum { MIDI_CONTROL_NONE, MIDI_CONTROL_NEXT, MIDI_CONTROL_PREV, MIDI_CONTROL_PUSH, MIDI_CONTROL_HOLD, MIDI_CONTROL_DOUBLE };
#define MIDI_DOUBLE_CLICK_MS 350u
typedef struct {
 uint8_t ab; int8_t edges; bool raw_down,down,held,click_pending,second_click;
 uint32_t changed_ms,pressed_ms,released_ms;
} midi_input_t;
/* Single click waits 350ms; double never emits a preceding single; hold cancels clicks. */
static inline int midi_input_step(midi_input_t *s,unsigned ab,bool down,uint32_t ms)
{
 static const int8_t q[16]={0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
 int event=MIDI_CONTROL_NONE;
 if(ab!=s->ab) {
  if((ab^s->ab)==3) s->edges=0;
  else s->edges+=q[(s->ab<<2)|ab];
  s->ab=(uint8_t)ab;
  if(s->edges>=4) {s->edges=0;event=MIDI_CONTROL_NEXT;}
  if(s->edges<=-4) {s->edges=0;event=MIDI_CONTROL_PREV;}
 }
 if(down!=s->raw_down) {s->raw_down=down;s->changed_ms=ms;}
 if(down!=s->down && (uint32_t)(ms-s->changed_ms)>=20) {
  s->down=down;
  if(down) {
   s->pressed_ms=ms;s->held=false;
   s->second_click=s->click_pending && (uint32_t)(s->changed_ms-s->released_ms)<MIDI_DOUBLE_CLICK_MS;
   if(s->click_pending && !s->second_click) {s->click_pending=false;event=MIDI_CONTROL_PUSH;}
  } else if(!s->held) {
   if(s->second_click) {s->click_pending=false;s->second_click=false;event=MIDI_CONTROL_DOUBLE;}
   else {s->click_pending=true;s->released_ms=ms;}
  }
 }
 if(s->down && s->raw_down && !s->held && (uint32_t)(ms-s->pressed_ms)>=1500) {
  s->held=true;s->click_pending=false;s->second_click=false;event=MIDI_CONTROL_HOLD;
 }
 if(s->click_pending && !s->down && !s->raw_down && (uint32_t)(ms-s->released_ms)>=MIDI_DOUBLE_CLICK_MS) {
  s->click_pending=false;event=MIDI_CONTROL_PUSH;
 }
 return event;
}
