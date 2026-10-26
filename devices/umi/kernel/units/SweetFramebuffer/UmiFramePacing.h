#pragma once
#include <stdint.h>
// Advance a periodic absolute deadline, skipping missed frames. Work duration
// does not add to every period, and overdue work never creates a catch-up loop.
static inline uint64_t umi_vsync_next(uint64_t previous,uint64_t period,uint64_t now) {
 if(!period)period=1;
 if(!previous)return now>UINT64_MAX-period?UINT64_MAX:now+period;
 if(previous>UINT64_MAX-period)return UINT64_MAX;
 uint64_t next=previous+period;
 if(next<=now){uint64_t skipped=(now-next)/period;
  if(skipped==UINT64_MAX || skipped+1>(UINT64_MAX-next)/period)return UINT64_MAX;
  next+=(skipped+1)*period;
 }
 return next;
}
static inline bool umi_frame_due(uint64_t now,uint64_t start,uint64_t period) {
 return now>=start && now-start>=period;
}

// IOMobileFramebufferVsyncNotifyFunc reconstructs three64-bit numbers from
// six64-bit arguments holding32-bit halves: refresh time, presented time, counter.
static inline void umi_fb_vsync_values(uint64_t now,uint64_t presented,uint64_t count,uint64_t values[6]) {
 values[0]=(uint32_t)now;values[1]=(uint32_t)(now>>32);
 values[2]=(uint32_t)presented;values[3]=(uint32_t)(presented>>32);
 values[4]=(uint32_t)count;values[5]=(uint32_t)(count>>32);
}
static inline bool umi_fb_mode_block(uint64_t block,uint8_t *bytes,uint64_t length) {
 if(!bytes || length>4096 || (block==0x40 && length<12))return false;
 for(uint64_t i=0;i<length;++i)bytes[i]=0;
 if(block==0x40){bytes[4]=1;bytes[8]=1;}
 return true;
}

static inline bool umi_present_due(uint64_t now,uint64_t deadline) {
 return deadline==0 || now>=deadline;
}
