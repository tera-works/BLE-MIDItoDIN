#include "ble_midi_peer_policy.h"
#include <stdio.h>
int main(void)
{
    int checks=0, failures=0;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL %d\n",__LINE__);}} while(0)
    uint8_t addr[6]={0};
    CHECK(ble_midi_peer_stable(0,addr));
    CHECK(!ble_midi_peer_stable(1,addr));
    addr[5]=0x40;CHECK(!ble_midi_peer_stable(1,addr));
    addr[5]=0x80;CHECK(!ble_midi_peer_stable(1,addr));
    addr[5]=0xC0;CHECK(ble_midi_peer_stable(1,addr));
    CHECK(!ble_midi_peer_stable(2,addr));
    CHECK(!ble_midi_peer_stable(3,addr));
    for(unsigned midi=0;midi<2;midi++) for(unsigned known=0;known<2;known++) {
        CHECK(ble_midi_peer_accept(midi,known,true)==(bool)known);
        CHECK(ble_midi_peer_accept(midi,known,false)==(bool)(midi||known));
    }
    printf("%d checks, %d failures\n",checks,failures);
    return failures?1:0;
}
