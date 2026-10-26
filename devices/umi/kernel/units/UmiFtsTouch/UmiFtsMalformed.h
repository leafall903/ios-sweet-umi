#pragma once
#include <stdint.h>
struct UmiFtsMalformedWindow { uint64_t startNs; unsigned count; bool started; };
// Never turn invalid data into a contact. Permit at most three cancellations
// per second only after the qualified controller has entered sensing state.
static inline bool umi_fts_malformed_retry(UmiFtsMalformedWindow *w,unsigned stage,uint64_t now) {
    if(stage!=6)return false;
    if(!w->started || now<w->startNs || now-w->startNs>=1000000000ULL){w->startNs=now;w->count=0;w->started=true;}
    return ++w->count<=3;
}
