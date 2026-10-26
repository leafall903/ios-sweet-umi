// Local iOS 16 prototype. No IOHID binary ABI or vtable offsets.
#pragma once
#include "UmiFtsCore.h"
template<class Emit>
static unsigned umi_sweet_deliver(UmiTouchFrame &previous,const UmiTouchFrame &next,Emit emit) {
    unsigned sent=0;
    for(unsigned id=0;id<10;++id) {
        const auto &old=previous.points[id];const auto &now=next.points[id];
        if(old.active==now.active && (!now.active || (old.x==now.x && old.y==now.y)))continue;
        // A lift retains the last delivered coordinate for its stable ID.
        const auto &position=now.active?now:old;
        emit((int)id,now.active?1:0,(int)position.x,(int)position.y);++sent;
    }
    previous=next;return sent;
}
