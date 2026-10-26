#include "UmiFtsCore.h"
#include <assert.h>
#include <stdio.h>

int main() {
    UmiTouchFrame frame = {};
    const uint8_t down[8] = {0x13, 0x01, 0xf4, 0x81, 0x3e, 0, 0, 1};
    assert(umi_fts_parse(&frame, down) == UMI_FTS_POINT);
    assert(frame.points[0].active && frame.points[0].x == 500 && frame.points[0].y == 1000);
    UmiTouchFrame before = frame;
    const uint8_t observed[][8] = {
        {0x0d,0,0x0c,0,0,0,0,0x0d},
        {0x0d,0,0x0c,0,0,0,0,0x0a},
        {0x0d,0,0x0c,0,0,0,0,0x19}
    };
    for (unsigned i = 0; i < 10000; ++i) {
        assert(umi_fts_parse(&frame, observed[i % 3]) == UMI_FTS_NONE);
        assert(!memcmp(&frame, &before, sizeof(frame)));
    }
    uint8_t packet[8] = {};
    for (unsigned code = 0; code < 16; ++code) {
        if (code == 3) continue;
        packet[0] = code;
        assert(umi_fts_parse(&frame, packet) == UMI_FTS_NONE);
        assert(!memcmp(&frame, &before, sizeof(frame)));
    }
    uint8_t invalid[8];
    memcpy(invalid, down, 8); invalid[1] = 0xa1;
    assert(umi_fts_parse(&frame, invalid) == UMI_FTS_BAD);
    memcpy(invalid, down, 8); invalid[2] = 0x38; invalid[3] = 0x84;
    assert(umi_fts_parse(&frame, invalid) == UMI_FTS_BAD); // x1080 stays invalid
    memcpy(invalid, down, 8); invalid[3] = 0x41; invalid[4] = 0x92;
    assert(umi_fts_parse(&frame, invalid) == UMI_FTS_BAD); // y2340 stays invalid
    memcpy(packet, down, 8); packet[0] = 0x33;
    assert(umi_fts_parse(&frame, packet) == UMI_FTS_POINT && !frame.points[0].active);
    assert(umi_fts_parse(&frame, down) == UMI_FTS_POINT);
    memset(packet, 0, 8); packet[0] = 3;
    assert(umi_fts_parse(&frame, packet) == UMI_FTS_RESET && !frame.points[0].active);
    assert(umi_fts_parse(&frame, down) == UMI_FTS_POINT);
    packet[0] = 0xf3;
    assert(umi_fts_parse(&frame, packet) == UMI_FTS_FAULT && !frame.points[0].active);
    puts("Observed native no-op packets preserve contacts across10000events; invalid contacts, release, READY and error semantics PASS");
}
