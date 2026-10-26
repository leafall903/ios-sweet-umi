#include "UmiImageSpan.h"
#include <assert.h>
#include <stdio.h>
int main() {
    uint64_t span = 0;
    assert(umi_image_minimum_span(256,1,2,1,1,672,0,&span) && span == 512);
    assert(umi_image_minimum_span(256,1,2,1,1,544,0,&span) && span == 512);
    assert(umi_image_minimum_span(28,65,4,1,1,128,0,&span) && span == 8304);
    assert(umi_image_minimum_span(28,28,4,1,1,128,0,&span) && span == 3568);
    assert(umi_image_minimum_span(6,6,4,1,1,32,0,&span) && span == 184);
    assert(umi_image_minimum_span(6,85,4,1,1,32,0,&span) && span == 2712);
    assert(umi_image_minimum_span(1,1,4,1,1,4096,0,&span) && span == 4);
    assert(umi_image_minimum_span(7,5,8,2,2,64,12,&span) && span == 172);
    assert(!umi_image_minimum_span(256,1,2,1,1,511,0,&span));
    assert(!umi_image_minimum_span(0,1,4,1,1,64,0,&span));
    assert(!umi_image_minimum_span(1,0,4,1,1,64,0,&span));
    assert(!umi_image_minimum_span(1,1,0,1,1,64,0,&span));
    assert(!umi_image_minimum_span(1,1,65,1,1,64,0,&span));
    assert(!umi_image_minimum_span(1,1,4,0,1,64,0,&span));
    assert(!umi_image_minimum_span(1,1,4,1,0,64,0,&span));
    assert(!umi_image_minimum_span(1,1,4,256,1,64,0,&span));
    assert(!umi_image_minimum_span(16385,1,4,1,1,65536,0,&span));
    assert(!umi_image_minimum_span(1,16385,4,1,1,64,0,&span));
    assert(!umi_image_minimum_span(1,16384,4,1,1,0xffffffff,0,&span));
    assert(!umi_image_minimum_span(1,1,4,1,1,64,0xffffffff,&span));
    assert(!umi_image_minimum_span(1,1,4,1,1,64,0,nullptr));
    puts("21 actual minimum-span cases PASS; observed512byte row accepted, undersized/malformed geometry rejected");
}
