#ifndef UMI_CALENDAR_H
#define UMI_CALENDAR_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
static bool umi_calendar_policy(uint32_t enabled,uint32_t cpus,uint64_t epoch,const uint8_t *model,size_t size)
{
 static const char wanted[]="xiaomi,umi";unsigned matches=0;
 if(enabled!=1||(cpus!=1&&cpus!=4&&cpus!=8)||epoch<1577836800ULL||epoch>4102444800ULL||!model||!size||size>4096)return false;
 for(size_t at=0;at<size;){size_t end=at;while(end<size&&model[end])end++;if(end==size)return false;
  if(end-at==sizeof(wanted)-1){size_t i=0;while(i<sizeof(wanted)-1&&model[at+i]==(uint8_t)wanted[i])i++;if(i==sizeof(wanted)-1)matches++;}at=end+1;}
 return matches==1;
}
#endif
