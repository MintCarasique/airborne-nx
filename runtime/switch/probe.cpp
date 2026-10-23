#include <switch.h>
#include "ppc_context.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

extern "C" PPC_FUNC(__imp__sub_828B6D00);
int run_half_tests();
static uint8_t* image;
static FILE* report;
static void log(const char* format,...) {
    va_list args;va_start(args,format);std::vprintf(format,args);va_end(args);
    if(report){va_start(args,format);std::vfprintf(report,format,args);va_end(args);std::fflush(report);}
}
uint8_t switch_probe_load8(uint32_t address) {
    if(address<0x82000000 || uint64_t(address)-0x82000000>=PPC_IMAGE_SIZE) {
        log("FAIL invalid guest byte read: %08x\n",address);std::abort();
    }
    return image[address-0x82000000];
}
int main(int argc,char** argv) {
    consoleInit(nullptr);
    const char* folder="sdmc:/switch/airborne-nx-probe";
    char directory[1024];std::snprintf(directory,sizeof(directory),"%s",folder);
    if(argc>0 && argv && argv[0]) {
        std::snprintf(directory,sizeof(directory),"%s",argv[0]);
        if(auto* slash=std::strrchr(directory,'/'))*slash=0;
        else std::snprintf(directory,sizeof(directory),"%s",folder);
    }
    char path[1200];std::snprintf(path,sizeof(path),"%s/result.txt",directory);report=std::fopen(path,"w");
    log("AirborneNX ARM64 CPU probe v0.1\nNot a playable game.\n");
    log("HOS=%08x applet_type=%u\n",hosversionGet(),unsigned(appletGetAppletType()));
    if(!report)log("NOTE: cannot create result.txt; record this screen.\n");
    bool passed=false;
    do {
        std::snprintf(path,sizeof(path),"%s/image.bin",directory);
        FILE* input=std::fopen(path,"rb");
        if(!input){log("FAIL cannot open image.bin beside NRO\n");break;}
        image=static_cast<uint8_t*>(std::malloc(PPC_IMAGE_SIZE));
        if(!image){std::fclose(input);log("FAIL image allocation (%llu bytes)\n",PPC_IMAGE_SIZE);break;}
        const size_t length=std::fread(image,1,PPC_IMAGE_SIZE,input);const bool extra=std::fgetc(input)!=EOF;std::fclose(input);
        if(length!=PPC_IMAGE_SIZE || extra){log("FAIL image size\n");break;}
        const uint8_t expected[32]={0xc9,0x34,0x9a,0x93,0x89,0xfb,0xee,0x7d,0x2b,0x83,0x81,0x89,0xdf,0x72,0x43,0x3d,0xf9,0x01,0xef,0x39,0x18,0x05,0x32,0x41,0xad,0xbb,0x3e,0x68,0x44,0x96,0x1c,0x48};
        uint8_t actual[32];sha256CalculateHash(actual,image,PPC_IMAGE_SIZE);
        if(std::memcmp(actual,expected,32)){log("FAIL normalized-image SHA256\n");break;}
        log("PASS image SHA256\n");
        alignas(32) uint8_t dummy[32]{};
        bool good=true;unsigned checks=0;
        for(unsigned trial=0;trial<64;++trial)for(unsigned index=0;index<12;++index) {
            PPCContext ctx{};ctx.r3.u64=index;ctx.r1.u64=0x12345678;ctx.r12.u64=trial;
            __imp__sub_828B6D00(ctx,dummy);
            good &= ctx.r3.u32==0x820f56c0+4*index && ctx.r1.u64==0x12345678;++checks;
        }
        if(!good){log("FAIL real leaf function\n");break;}
        log("PASS real guest function 828B6D00: %u calls\n",checks);
        log("Running emitted FLOAT16_2 suite...\n");consoleUpdate(nullptr);
        const int half=run_half_tests();
        if(half){log("FAIL FLOAT16_2 code=%d\n",half);break;}
        log("PASS FLOAT16_2: 1138867 generated executions\n");
        const auto before=ppc_read_timebase();svcSleepThread(10000000);const auto after=ppc_read_timebase();
        if(after<=before){log("FAIL monotonic timebase\n");break;}
        log("PASS timebase freq=%llu delta=%llu\n",static_cast<unsigned long long>(PPC_TIMEBASE_FREQUENCY),static_cast<unsigned long long>(after-before));
        passed=true;
    }while(false);
    log("RESULT=%s\n",passed ? "PASS":"FAIL");
    std::free(image);if(report){std::fclose(report);report=nullptr;}
    std::printf("\nPress + to exit. Result saved beside NRO.\n");
    PadState pad;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);
    while(appletMainLoop()){padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_Plus)break;consoleUpdate(nullptr);}
    consoleExit(nullptr);return passed ? 0:1;
}
