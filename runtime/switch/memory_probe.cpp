#include <switch.h>
#include "guest_memory.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>

extern "C" PPC_FUNC(__imp__sub_828B6D00);
extern PPC_FUNC(test_physical_lwz);
extern PPC_FUNC(test_physical_stw);
extern PPC_FUNC(test_physical_lvx);
extern PPC_FUNC(test_physical_stvx);
extern PPC_FUNC(test_physical_lwarx);
extern PPC_FUNC(test_physical_stwcx);
extern PPC_FUNC(test_physical_dcbz);
static FILE* report;
static char first_failure[1024]{};
static void log(const char* format,...) {
    char message[4096];va_list args;va_start(args,format);
    std::vsnprintf(message,sizeof(message),format,args);va_end(args);
    if(!first_failure[0] && std::strncmp(message,"FAIL ",5)==0)
        std::snprintf(first_failure,sizeof(first_failure),"%s",message);
    std::printf("%s",message);
    if(report){std::fputs(message,report);std::fflush(report);}
    consoleUpdate(nullptr);
}
#include "startup_services.h"
static bool unmapped(void* address) {
    MemoryInfo info{};u32 page=0;const Result rc=svcQueryMemory(&info,&page,uintptr_t(address));
    return R_SUCCEEDED(rc) && (info.type&255)==0 && info.perm==Perm_None;
}
int main(int argc,char** argv) {
    consoleInit(nullptr);
    char directory[1024]="sdmc:/switch/airborne-nx-startup-v0.3";
    if(argc>0 && argv && argv[0]) {
        std::snprintf(directory,sizeof(directory),"%s",argv[0]);
        if(auto* slash=std::strrchr(directory,'/'))*slash=0;
        else std::snprintf(directory,sizeof(directory),"sdmc:/switch/airborne-nx-startup-v0.3");
    }
    char path[1200];std::snprintf(path,sizeof(path),"%s/result-v0.3.txt",directory);report=std::fopen(path,"w");
    log("AirborneNX ARM64 startup probe v0.3\nNot a playable game.\n");
    log("BUILD=%s\n",AIRBORNE_PROBE_BUILD_ID);
    log("HOS=%08x applet_type=%u process_handle=%08x\n",hosversionGet(),unsigned(appletGetAppletType()),envGetOwnProcessHandle());
    for(unsigned id:{2u,3u,4u,5u,6u,7u,12u,13u,14u,15u}) {
        u64 value=0;const Result rc=svcGetInfo(&value,id,CUR_PROCESS_HANDLE,0);
        log("PROCESS_INFO id=%u value=%016llx rc=%08x module=%u description=%u\n",id,(unsigned long long)value,rc,R_MODULE(rc),R_DESCRIPTION(rc));
    }
    log("SVC_HINT map_process=%u unmap_process=%u create_shared=%u\n",envIsSyscallHinted(0x74),envIsSyscallHinted(0x75),envIsSyscallHinted(0x50));
    log("SVC_HINT set_process_permission=%u map_code=%u unmap_code=%u create_thread=%u start_thread=%u\n",envIsSyscallHinted(0x73),envIsSyscallHinted(0x77),envIsSyscallHinted(0x78),envIsSyscallHinted(0x08),envIsSyscallHinted(0x09));
    log("PHASE_BEGIN native_services\n");
    const bool services=startup_services();
    if(!services)log("FAIL native startup services\n");
    log("PHASE native_services=%s\n",services ? "PASS":"FAIL");
    log("PHASE_BEGIN guest_memory\n");
    NxGuestMemory memory(log);bool passed=false;size_t checks=0;
    bool memory_phases_started=false;
    bool guest_workers_attempted=false,guest_workers_passed=false;
    do {
        Result rc=memory.open();
        if(R_FAILED(rc)){log("FAIL %s rc=%08x\n",memory.failure_stage,rc);break;}
        log("PASS VA reservation span=4294967296 base=%016llx\n",static_cast<unsigned long long>(uintptr_t(memory.base)));
        if(!unmapped(memory.base) || !unmapped(memory.base+0xffffffffull)){log("FAIL reservation not initially unmapped\n");break;}
        memory_phases_started=true;
        unsigned stack=0;bool image_ready=false;
        log("PHASE_BEGIN image_stack\n");
        do {
        unsigned image=0;
        if(R_FAILED(memory.create_backing(PPC_IMAGE_SIZE,image)) ||
           R_FAILED(memory.create_backing(0x40000,stack))) {log("FAIL %s rc=%08x\n",memory.failure_stage,memory.failure_result);break;}
        std::snprintf(path,sizeof(path),"%s/image.bin",directory);FILE* input=std::fopen(path,"rb");
        if(!input){log("FAIL cannot open image.bin\n");break;}
        const auto length=std::fread(memory.backing(image),1,PPC_IMAGE_SIZE,input);const bool extra=std::fgetc(input)!=EOF;std::fclose(input);
        if(length!=PPC_IMAGE_SIZE || extra){log("FAIL image size\n");break;}
        const uint8_t expected[32]={0xc9,0x34,0x9a,0x93,0x89,0xfb,0xee,0x7d,0x2b,0x83,0x81,0x89,0xdf,0x72,0x43,0x3d,0xf9,0x01,0xef,0x39,0x18,0x05,0x32,0x41,0xad,0xbb,0x3e,0x68,0x44,0x96,0x1c,0x48};
        uint8_t actual[32];sha256CalculateHash(actual,memory.backing(image),PPC_IMAGE_SIZE);
        if(std::memcmp(expected,actual,32)){log("FAIL image SHA256\n");break;}
        if(R_FAILED(memory.map(0x82000000,image)) || R_FAILED(memory.map(0x10010000,stack))) {
            log("FAIL image/stack %s rc=%08x\n",memory.failure_stage,memory.failure_result);break;
        }
        sha256CalculateHash(actual,memory.base+0x82000000,PPC_IMAGE_SIZE);
        if(std::memcmp(expected,actual,32)){log("FAIL mapped image SHA256\n");break;}
        log("PASS mapped image SHA256\n");
        bool good=true;
        for(unsigned trial=0;trial<64;++trial)for(unsigned index=0;index<12;++index) {
            PPCContext ctx{};ctx.r3.u64=index;ctx.r1.u64=0x1004ff00;ctx.r12.u64=trial;
            __imp__sub_828B6D00(ctx,memory.base);
            good &= ctx.r3.u32==0x820f56c0+4*index && ctx.r1.u64==0x1004ff00;++checks;
        }
        if(!good){log("FAIL mapped real leaf\n");break;}
        log("PASS mapped real guest function 828B6D00: 768 calls\n");
        image_ready=true;
        guest_workers_attempted=true;
        log("PHASE_BEGIN guest_workers\n");
        guest_workers_passed=startup_services(memory.base);
        if(!guest_workers_passed)log("FAIL guest startup workers\n");
        log("PHASE guest_workers=%s\n",guest_workers_passed ? "PASS":"FAIL");
        }while(false);
        log("PHASE image_stack=%s\n",image_ready ? "PASS":"FAIL");
        bool physical_ready=false;
        log("PHASE_BEGIN physical_aliases\n");
        do {
        unsigned physical=0;bool good=true;
        if(R_FAILED(memory.create_backing(0x10000,physical))) {log("FAIL physical backing %s rc=%08x\n",memory.failure_stage,memory.failure_result);break;}
        const uint32_t aliases[]={0xa0010000,0xc0010000,0xe000f000};
        for(auto alias:aliases)if(R_FAILED(memory.map(alias,physical))) {good=false;break;}
        if(!good){log("FAIL physical aliases %s rc=%08x\n",memory.failure_stage,memory.failure_result);break;}
        PPCContext ctx{};
        for(auto alias:aliases) {
            ctx.r4.u64=alias;ctx.r3.u64=0x12345678;test_physical_stw(ctx,memory.base);
            for(auto target:aliases) {
                ctx.r4.u64=target;test_physical_lwz(ctx,memory.base);
                good &= ctx.r3.u64==0x12345678;++checks;
                auto* p=ppc_guest_ptr(memory.base,target);good &= p[0]==0x12 && p[1]==0x34 && p[2]==0x56 && p[3]==0x78;++checks;
            }
            for(unsigned i=0;i<16;++i)ctx.v1.u8[i]=uint8_t(i+1);
            ctx.r4.u64=alias;test_physical_stvx(ctx,memory.base);
            for(auto target:aliases)for(unsigned i=0;i<16;++i){good &= ppc_guest_ptr(memory.base,target)[i]==16-i;++checks;}
            test_physical_lvx(ctx,memory.base);
            for(unsigned i=0;i<16;++i){good &= ctx.v1.u8[i]==i+1;++checks;}
            ctx.r3.u64=0x99887766;test_physical_stw(ctx,memory.base);test_physical_lwarx(ctx,memory.base);
            good &= ctx.r3.u32==0x99887766;++checks;ctx.r3.u64=0xdeadbeef;test_physical_stwcx(ctx,memory.base);
            good &= ctx.cr0.eq;++checks;
            for(auto target:aliases){ctx.r4.u64=target;test_physical_lwz(ctx,memory.base);good &= ctx.r3.u32==0xdeadbeef;++checks;}
            std::memset(ppc_guest_ptr(memory.base,alias),0x7a,64);ctx.r4.u64=alias+17;test_physical_dcbz(ctx,memory.base);
            for(auto target:aliases)for(unsigned i=0;i<64;++i){good &= ppc_guest_ptr(memory.base,target)[i]==(i<32 ? 0:0x7a);++checks;}
        }
        if(!good){log("FAIL physical emitted-instruction coherence checks\n");break;}
        log("PASS physical A/C/E aliases: scalar/vector/atomic/cache-zero\n");
        physical_ready=true;
        }while(false);
        log("PHASE physical_aliases=%s\n",physical_ready ? "PASS":"FAIL");
        bool heap_ready=false;
        log("PHASE_BEGIN guest_heap\n");
        do {
            unsigned heap=0;constexpr size_t bytes=4*1024*1024;
            if(R_FAILED(memory.create_backing(bytes,heap)) || R_FAILED(memory.map(0x20000000,heap))) {
                log("FAIL guest heap %s rc=%08x\n",memory.failure_stage,memory.failure_result);break;
            }
            bool good=true;auto* source=memory.backing(heap);auto* guest=ppc_guest_ptr(memory.base,0x20000000);
            for(size_t offset=0;offset<bytes;offset+=4096) {
                good &= source[offset]==0 && guest[offset+4095]==0;
                source[offset]=uint8_t(offset/4096);good &= guest[offset]==uint8_t(offset/4096);
                guest[offset+4095]=uint8_t((offset/4096)^0xa5);good &= source[offset+4095]==uint8_t((offset/4096)^0xa5);
            }
            MemoryInfo info{};u32 page=0;const Result rc=svcQueryMemory(&info,&page,uintptr_t(guest));
            good &= R_SUCCEEDED(rc) && info.perm==Perm_Rw;
            log("HEAP bytes=%zu pages=1024 page_checks=4096 query_rc=%08x type=%08x perm=%08x coherent=%u\n",bytes,rc,info.type,info.perm,good);
            if(!good){log("FAIL guest heap zero initialization or bidirectional coherence\n");break;}
            heap_ready=true;
        }while(false);
        log("PHASE guest_heap=%s\n",heap_ready ? "PASS":"FAIL");
        bool guards_ready=false;
        if(image_ready)do {
        if(!unmapped(memory.base+0x1000f000) || !unmapped(memory.base+0x10050000)) {log("FAIL stack guard query\n");break;}
        log("PASS stack guards=2\n");
        // Invalid API inputs must fail without replacing live mappings.
        bool good=R_FAILED(memory.map(0x10010000,stack)) && R_FAILED(memory.map(0x10010001,stack)) &&
             !NxGuestMemory::valid_range(0xfffff000,0x1000) && !NxGuestMemory::valid_range(0x20000000,0);
        if(!good){log("FAIL invalid mapping checks\n");break;}
        log("PASS rejected overlapping/unaligned/out-of-range mappings\n");
        guards_ready=true;
        }while(false);
        log("PHASE guards=%s\n",!image_ready ? "BLOCKED":guards_ready ? "PASS":"FAIL");
        passed=image_ready && physical_ready && heap_ready && guards_ready;
    }while(false);
    if(!memory_phases_started) {
        for(const char* phase:{"image_stack","physical_aliases","guest_heap","guards"})
            log("PHASE %s=BLOCKED dependency=guest_address_space\n",phase);
    }
    log("PHASE guest_memory=%s\n",passed ? "PASS":"FAIL");
    if(!guest_workers_attempted)log("PHASE guest_workers=BLOCKED dependency=image_stack_mapping\n");
    auto* old_base=memory.base;
    const Result cleanup=memory.close();
    bool cleanup_passed=R_SUCCEEDED(cleanup);
    if(R_FAILED(cleanup)){log("FAIL cleanup %s rc=%08x\n",memory.failure_stage,cleanup);passed=false;}
    else if(old_base && (!unmapped(old_base+0x82000000) || !unmapped(old_base+0xa0010000) || !unmapped(old_base+0xe0010000))) {
        log("FAIL mappings remained after cleanup\n");cleanup_passed=false;passed=false;
    }else log("PASS unmap and release\n");
    log("PHASE cleanup=%s\n",cleanup_passed ? "PASS":"FAIL");
    log("PHASE game_entry=BLOCKED dependency=full_switch_runtime_and_graphics_service\n");
    log("FIRST_FAILURE=%s",first_failure[0] ? first_failure:"none\n");
    log("CHECKS=%zu\nRESULT=%s\n",checks,(passed && cleanup_passed && services && guest_workers_passed) ? "PASS":"FAIL");
    if(report){std::fclose(report);report=nullptr;}
    std::printf("\nPress + to exit. Log: result-v0.3.txt\n");
    PadState pad;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);
    while(appletMainLoop()){padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_Plus)break;consoleUpdate(nullptr);}
    consoleExit(nullptr);
    if(R_FAILED(cleanup))svcExitProcess();
    return passed && cleanup_passed && services && guest_workers_passed ? 0:1;
}
