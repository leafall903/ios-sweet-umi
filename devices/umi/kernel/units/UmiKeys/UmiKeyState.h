// Mi10 observer bits to the existing iOS16 HID consumer usages.
#pragma once
struct UmiKeyState {
    unsigned delivered=0, candidate=0, samples=0;
    bool synced=false;
    template<class Emit> void reset(Emit emit) {
        for(unsigned b=0;b<3;++b) if(delivered&(1U<<b)) emit(b==2?0:b==0?1:2,0);
        delivered=candidate=samples=0; synced=false;
    }
    template<class Emit> void update(unsigned mask,Emit emit) {
        mask&=7;
        // Startup/recovery keys held during boot must be released first.
        if(!synced) { if(mask==0) synced=true; return; }
        if(mask!=candidate) { candidate=mask; samples=1; return; }
        if(samples<3) ++samples;
        if(samples!=3 || mask==delivered) return;
        for(unsigned b=0;b<3;++b) if((mask^delivered)&(1U<<b))
            emit(b==2?0:b==0?1:2,(mask>>b)&1);
        delivered=mask;
    }
};
