#pragma once
#include <cfenv>

// Only services needed by the guest startup workers. No privileged CreateEvent.
// Workers never write the report; the main thread records their joined results.
static bool startup_services(uint8_t* base=nullptr) {
    struct Worker {
        UEvent gate{},done{};
        s32 slot=-1; unsigned id=0;
        uint8_t* base=nullptr;
        bool good=false; unsigned calls=0;
        Result wait_rc=0;
        uintptr_t tls_value=0; int rounding=-1;
    } workers[2];
    Thread threads[2]{};bool created[2]{},started[2]{};
    const s32 slot=threadTlsAlloc(nullptr);
    log("SERVICE tls_alloc slot=%d guest_memory=%u\n",slot,base!=nullptr);
    if(slot<0)return false;
    threadTlsSet(slot,reinterpret_cast<void*>(uintptr_t(0x1234)));
    const int main_round=std::fegetround();bool good=true;
    auto entry=+[](void* argument) {
        auto& w=*static_cast<Worker*>(argument);
        w.good=threadTlsGet(w.slot)==nullptr;
        threadTlsSet(w.slot,reinterpret_cast<void*>(uintptr_t(0x100+w.id)));
        const int mode=w.id ? FE_DOWNWARD:FE_UPWARD;
        w.good &= std::fesetround(mode)==0;
        w.wait_rc=waitSingle(waiterForUEvent(&w.gate),2000000000ull);
        w.good &= R_SUCCEEDED(w.wait_rc);
        if(w.base && w.good) {
            const uint32_t stack=0x10010000+w.id*0x20000;
            const uint32_t pcr=stack+0x1000;
            auto* p=ppc_guest_ptr(w.base,pcr);p[0]=uint8_t(w.id+1);
            for(unsigned n=0;n<128;++n) {
                PPCContext ctx{};ctx.r1.u64=stack+0x1ff00;ctx.r13.u64=pcr;ctx.r3.u64=n%12;
                __imp__sub_828B6D00(ctx,w.base);
                w.good &= ctx.r3.u32==0x820f56c0+4*(n%12) && ctx.r1.u64==stack+0x1ff00 && ctx.r13.u64==pcr;
                ++w.calls;
            }
            w.good &= p[0]==w.id+1;
        }
        w.tls_value=uintptr_t(threadTlsGet(w.slot));w.rounding=std::fegetround();
        w.good &= w.tls_value==0x100+w.id && w.rounding==mode;
        ueventSignal(&w.done);
    };
    for(unsigned i=0;i<2;++i) {
        auto& w=workers[i];w.id=i;w.slot=slot;w.base=base;
        ueventCreate(&w.gate,false);ueventCreate(&w.done,true);
        Result rc=threadCreate(&threads[i],entry,&w,nullptr,0x10000,0x2c,-2);
        log("SERVICE worker=%u operation=threadCreate stack=65536 priority=44 core=-2 rc=%08x module=%u description=%u\n",i,rc,R_MODULE(rc),R_DESCRIPTION(rc));
        if(R_FAILED(rc)){good=false;continue;}created[i]=true;
        rc=threadStart(&threads[i]);
        log("SERVICE worker=%u operation=threadStart rc=%08x\n",i,rc);
        started[i]=R_SUCCEEDED(rc);good &= started[i];
    }
    for(unsigned i=0;i<2;++i)if(started[i])ueventSignal(&workers[i].gate);
    for(unsigned i=0;i<2;++i) {
        if(started[i]) {
            Result rc=waitSingleHandle(threads[i].handle,3000000000ull);
            log("SERVICE worker=%u operation=join timeout_ns=3000000000 rc=%08x\n",i,rc);
            if(R_FAILED(rc)) {
                // Stack-local worker state cannot outlive this function.
                log("FAIL worker did not terminate; process exit required\nRESULT=FAIL\n");
                if(report)std::fflush(report);
                svcExitProcess();
            }
            auto& w=workers[i];
            const Result done=waitSingle(waiterForUEvent(&w.done),0);
            const Result consumed=waitSingle(waiterForUEvent(&w.done),0);
            good &= w.good && R_SUCCEEDED(done) && consumed==KERNELRESULT(TimedOut);
            log("SERVICE worker=%u good=%u gate_rc=%08x tls=%llx rounding=%d guest_calls=%u done_rc=%08x consumed_rc=%08x\n",
                i,w.good,w.wait_rc,(unsigned long long)w.tls_value,w.rounding,w.calls,done,consumed);
        }
        if(created[i]) {const Result rc=threadClose(&threads[i]);log("SERVICE worker=%u operation=threadClose rc=%08x\n",i,rc);good &= R_SUCCEEDED(rc);}
    }
    good &= threadTlsGet(slot)==reinterpret_cast<void*>(uintptr_t(0x1234)) && std::fegetround()==main_round;
    threadTlsSet(slot,nullptr);threadTlsFree(slot);
    UEvent event;ueventCreate(&event,false);
    const Result empty=waitSingle(waiterForUEvent(&event),0);
    ueventSignal(&event);
    const Result first=waitSingle(waiterForUEvent(&event),0),second=waitSingle(waiterForUEvent(&event),0);
    ueventClear(&event);const Result cleared=waitSingle(waiterForUEvent(&event),0);
    good &= empty==KERNELRESULT(TimedOut) && R_SUCCEEDED(first) && R_SUCCEEDED(second) && cleared==KERNELRESULT(TimedOut);
    log("SERVICE manual_event empty=%08x first=%08x second=%08x cleared=%08x main_rounding=%d\n",empty,first,second,cleared,main_round);
    const uint64_t before=ppc_read_timebase();svcSleepThread(10000000);
    const uint64_t delta=ppc_read_timebase()-before;
    good &= delta>0;log("SERVICE timebase frequency=50000000 sleep_ns=10000000 delta=%llu\n",(unsigned long long)delta);
    return good;
}
