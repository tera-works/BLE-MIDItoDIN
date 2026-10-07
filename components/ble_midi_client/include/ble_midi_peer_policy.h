#pragma once
#include <stdbool.h>
#include <stdint.h>
/* NimBLE address types 0=Public, 1=Random. RPA/NRPA must not be fixed targets. */
static inline bool ble_midi_peer_stable(uint8_t type, const uint8_t addr[6])
{ return type==0 || (type==1 && (addr[5] & 0xC0)==0xC0); }
static inline bool ble_midi_peer_accept(bool has_midi, bool known, bool known_only)
{ return (!known_only || known) && (has_midi || known); }
