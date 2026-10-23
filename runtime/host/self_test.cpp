#include "address_space.h"
#include "diagnostic.h"
#include <cstdio>
#include <chrono>
#include <thread>
#include <exception>
#include <fstream>
#include "../audio/pcm_conversion.h"
#include "../io/game_files.h"
bool host_tls_contention_check(uint8_t*);

size_t host_service_self_test(uint8_t* base) {
    constexpr uint32_t thread=0x70020000,tls=0x70022000,pcr=0x70021000;
    constexpr uint32_t cs=thread+0xc00,outputs=thread+0xd00;
    size_t checks=0;
    auto read=[&](uint32_t address){uint32_t b;std::memcpy(&b,base+address,4);return __builtin_bswap32(b);};
    auto check=[&](bool value){++checks;if(!value)throw DiagnosticStop("Host service self-test failed at check "+std::to_string(checks));};
    PPCContext ctx{};ctx.r13.u64=pcr;
    auto call=[&](const char* name,uint32_t r3=0,uint32_t r4=0) {
        ctx.r3.u64=r3;ctx.r4.u64=r4;host_import(name,ctx,base);return ctx.r3.u32;
    };
    check(read(pcr)==tls && read(pcr+0x100)==thread);
    check(read(thread+0x14c)==1);
    check(host_tls_contention_check(base));
    check(call("KeSetBasePriorityThread",thread,0)==0);
    check(call("KeSetBasePriorityThread",thread,-1)==0);
    check(call("KeSetBasePriorityThread",thread,0)==uint32_t(-1));
    for(unsigned i=0;i<64;++i) {check(call("KeTlsAlloc")==i);check(read(tls+i*4)==0);}
    check(call("KeTlsAlloc")==0xffffffff);
    check(call("KeTlsSetValue",63,0x87654321)==1);
    check(read(tls+63*4)==0x87654321 && call("KeTlsGetValue",63)==0x87654321);
    check(call("KeTlsSetValue",64,1)==0 && call("KeTlsGetValue",64)==0);
    for(unsigned i=0;i<64;++i)check(call("KeTlsFree",i)==1);
    check(call("KeTlsFree",0xffffffff)==0);
    call("RtlInitializeCriticalSection",cs);
    check(read(cs+16)==0xffffffff && read(cs+20)==0);
    call("RtlEnterCriticalSection",cs);call("RtlEnterCriticalSection",cs);
    check(read(cs+16)==1 && read(cs+20)==2 && read(cs+24)==thread);
    check(call("RtlTryEnterCriticalSection",cs)==1 && read(cs+20)==3);
    for(unsigned i=0;i<3;++i)call("RtlLeaveCriticalSection",cs);
    check(read(cs+16)==0xffffffff && read(cs+20)==0 && read(cs+24)==0);
    call("RtlInitializeCriticalSection",cs);check(read(cs+16)==0xffffffff && read(cs+20)==0 && read(cs+24)==0);
    call("RtlEnterCriticalSection",cs);bool live_reinit=false;
    try{call("RtlInitializeCriticalSection",cs);}catch(const DiagnosticStop&){live_reinit=true;}
    check(live_reinit && read(cs+20)==1);call("RtlLeaveCriticalSection",cs);
    bool invalid_release=false;
    try{call("RtlLeaveCriticalSection",cs);}catch(const DiagnosticStop&){invalid_release=true;}
    check(invalid_release);
    check(call("KeGetCurrentProcessType")==1);
    check(call("XGetAVPack")==6);
    check(call("XexCheckExecutablePrivilege",10)==1);
    check(call("XexCheckExecutablePrivilege",31)==0 && call("XexCheckExecutablePrivilege",32)==0);
    check(call("KeQueryPerformanceFrequency")==50000000);
    auto write=[&](uint32_t at,uint32_t value){value=__builtin_bswap32(value);std::memcpy(base+at,&value,4);};
    auto write64=[&](uint32_t at,uint64_t value){value=__builtin_bswap64(value);std::memcpy(base+at,&value,8);};
    {
        ctx.r5.u64=1;ctx.r6.u64=2;
        check(call("NtCreateSemaphore",outputs+208,0)==0);const auto handle=read(outputs+208);
        write64(outputs+216,0);ctx.r5.u64=0;ctx.r6.u64=outputs+216;
        check(call("NtWaitForSingleObjectEx",handle,1)==0);
        check(call("NtWaitForSingleObjectEx",handle,1)==0x102);
        ctx.r5.u64=outputs+212;check(call("NtReleaseSemaphore",handle,2)==0 && read(outputs+212)==0);
        write(outputs+212,0x12345678);check(call("NtReleaseSemaphore",handle,1)==0xc0000047 && read(outputs+212)==0x12345678);
        ctx.r5.u64=0;ctx.r6.u64=outputs+216;
        check(call("NtWaitForSingleObjectEx",handle,1)==0);check(call("NtWaitForSingleObjectEx",handle,1)==0);
        check(call("NtWaitForSingleObjectEx",handle,1)==0x102);
        check(call("NtClose",handle)==0);
        check(call("NtWaitForSingleObjectEx",handle,1)==0xc0000008);
    }
    // Movie/resource loading changes cache policy for a non-page-sized large
    // allocation. Protection must use the allocation's 64K granularity even
    // after its original large-page flag disappears from the new policy.
    {
        GuestPhysicalAllocator physical(base);
        const auto address=physical.allocate(0x11000,0x20000004,0,0xffffffff,65536);
        check(address!=0 && physical.allocated_bytes()==0x20000);
        check(physical.protect(address,0x11000,0x404));
        check(physical.query_protection(address)==0x404);
        check(physical.protect(address,0x11000,2));
        check(physical.contains(GuestPhysicalAllocator::physical(address),0x11000) && !physical.contains(GuestPhysicalAllocator::physical(address),4,true));
        check(physical.protect(address,0x11000,1));
        check(!physical.contains(GuestPhysicalAllocator::physical(address),4));
        check(physical.protect(address,0x11000,4));
        check(physical.free(address));
    }
    // The graphics interrupt path acquires both spinlock variants. Ownership
    // must be the guest PCR address, and Kf acquisition must preserve IRQL.
    write(outputs+48,0);base[pcr+0x18]=0;
    check(call("KfAcquireSpinLock",outputs+48)==0 && read(outputs+48)==pcr && base[pcr+0x18]==2);
    bool self_deadlock=false;try{call("KeAcquireSpinLockAtRaisedIrql",outputs+48);}catch(const DiagnosticStop&){self_deadlock=true;}
    check(self_deadlock && read(outputs+48)==pcr);
    call("KfReleaseSpinLock",outputs+48,0);check(!read(outputs+48) && !base[pcr+0x18]);
    bool low_irql=false;try{call("KeAcquireSpinLockAtRaisedIrql",outputs+48);}catch(const DiagnosticStop&){low_irql=true;}
    check(low_irql && !read(outputs+48));base[pcr+0x18]=2;
    call("KeAcquireSpinLockAtRaisedIrql",outputs+48);check(read(outputs+48)==pcr);
    call("KeReleaseSpinLockFromRaisedIrql",outputs+48);check(!read(outputs+48) && base[pcr+0x18]==2);base[pcr+0x18]=0;
    check(call("XMACreateContext",outputs+52)==0);const auto xma0=read(outputs+52);
    check(call("XMACreateContext",outputs+52)==0);const auto xma1=read(outputs+52);check(xma1==xma0+64);
    check(call("XMAReleaseContext",xma0)==0 && call("XMAReleaseContext",xma0)==0xc000000d);
    check(call("XMACreateContext",outputs+52)==0 && read(outputs+52)==xma0);
    check(call("XMAReleaseContext",xma0)==0 && call("XMAReleaseContext",xma1)==0);
    const uint32_t sem=outputs+128;
    ctx.r3.u64=sem;ctx.r4.u64=1;ctx.r5.u64=2;host_import("KeInitializeSemaphore",ctx,base);
    check(base[sem]==5 && read(sem+4)==1 && read(sem+16)==2);
    auto semaphore_wait=[&]{ctx.r3.u64=sem;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=0;ctx.r7.u64=outputs+32;
        write64(outputs+32,0);host_import("KeWaitForSingleObject",ctx,base);return ctx.r3.u32;};
    check(semaphore_wait()==0 && read(sem+4)==0);check(semaphore_wait()==0x102);
    ctx.r5.u64=2;ctx.r6.u64=0;check(call("KeReleaseSemaphore",sem)==0 && read(sem+4)==2);
    bool overflow=false;ctx.r5.u64=1;
    try{call("KeReleaseSemaphore",sem);}catch(const DiagnosticStop&){overflow=true;}
    check(overflow && read(sem+4)==2);
    check(semaphore_wait()==0 && semaphore_wait()==0 && semaphore_wait()==0x102);
    const uint32_t multi_event=outputs+160,multi_list=outputs+184;
    std::memset(base+multi_event,0,16);base[multi_event]=1;
    write(multi_list,sem);write(multi_list+4,multi_event);
    auto multi_wait=[&](uint32_t type,uint64_t timeout=0){ctx.r3.u64=2;ctx.r4.u64=multi_list;ctx.r5.u64=type;ctx.r6.u64=0;
        ctx.r7.u64=0;ctx.r8.u64=0;ctx.r9.u64=outputs+32;write64(outputs+32,timeout);
        host_import("KeWaitForMultipleObjects",ctx,base);return ctx.r3.u32;};
    check(multi_wait(1)==0x102);
    ctx.r5.u64=1;ctx.r6.u64=0;call("KeReleaseSemaphore",sem);
    check(multi_wait(0)==0x102 && read(sem+4)==1); // Failed wait-all consumes nothing.
    ctx.r5.u64=0;call("KeSetEvent",multi_event);
    check(multi_wait(1)==0 && read(sem+4)==0 && read(multi_event+4)==1);
    check(multi_wait(1)==1 && read(multi_event+4)==0);
    ctx.r5.u64=1;ctx.r6.u64=0;call("KeReleaseSemaphore",sem);
    ctx.r5.u64=0;call("KeSetEvent",multi_event);
    check(multi_wait(0)==0 && read(sem+4)==0 && read(multi_event+4)==0);
    write(multi_list+4,sem);bool duplicate=false;
    try{multi_wait(0);}catch(const DiagnosticStop&){duplicate=true;}check(duplicate);
    write(multi_list+4,multi_event);std::exception_ptr wake_error;
    std::thread wake([&]{try{
        std::this_thread::sleep_for(std::chrono::milliseconds(10));PPCContext other{};other.r13.u64=pcr;
        other.r3.u64=sem;other.r5.u64=1;host_import("KeReleaseSemaphore",other,base);
        other.r3.u64=multi_event;other.r5.u64=0;host_import("KeSetEvent",other,base);
    }catch(...){wake_error=std::current_exception();}});
    const auto awakened=multi_wait(0,uint64_t(-10000000ll));wake.join();if(wake_error)std::rethrow_exception(wake_error);
    check(awakened==0 && read(sem+4)==0 && read(multi_event+4)==0);
    std::array<uint8_t,6144> planar{};
    auto sample=[&](unsigned channel,unsigned index,uint32_t bits){bits=__builtin_bswap32(bits);std::memcpy(planar.data()+4*(channel*256+index),&bits,4);};
    sample(0,0,0x3f800000);sample(1,0,0xbf800000);sample(2,1,0x3f800000);
    sample(4,2,0x3f000000);sample(5,2,0xbf000000);sample(3,3,0x3f800000);
    sample(0,4,0x7fc00000);sample(1,4,0x7f800000);sample(0,5,0x40000000);
    const auto pcm=airborne::stereo_pcm(planar.data());
    check(pcm[0]==32767 && pcm[1]==-32768);check(pcm[2]==23170 && pcm[3]==23170);
    check(pcm[4]==11585 && pcm[5]==-11585);check(pcm[6]==0 && pcm[7]==0);
    check(pcm[8]==0 && pcm[9]==0 && pcm[10]==32767);
    auto event_wait=[&](uint32_t handle) {
        ctx.r3.u64=handle;ctx.r4.u64=1;ctx.r5.u64=0;ctx.r6.u64=outputs+32;
        write64(outputs+32,0);host_import("NtWaitForSingleObjectEx",ctx,base);return ctx.r3.u32;
    };
    for(unsigned manual=0;manual<2;++manual) {
        ctx.r3.u64=outputs;ctx.r4.u64=0;ctx.r5.u64=manual ? 0:1;ctx.r6.u64=0;
        host_import("NtCreateEvent",ctx,base);check(ctx.r3.u32==0);
        const auto event=read(outputs);check(event_wait(event)==0x102);
        check(call("NtSetEvent",event,outputs+4)==0 && read(outputs+4)==0);
        check(event_wait(event)==0);check(event_wait(event)==(manual ? 0:0x102));
        check(call("NtResetEvent",event,outputs+4)==0 && read(outputs+4)==manual);
        check(event_wait(event)==0x102);check(call("NtClose",event)==0);
        check(call("NtSetEvent",event)==0xc0000008);
    }
    check(call("KeTlsAlloc")==0);check(call("KeTlsSetValue",0,0x12345678)==1);
    for(unsigned i=0;i<4;++i) {
        ctx.r3.u64=outputs;ctx.r4.u64=0x4000;ctx.r5.u64=outputs+4;
        ctx.r6.u64=0;ctx.r7.u64=0x828b6d00;ctx.r8.u64=i;ctx.r9.u64=1;
        host_import("ExCreateThread",ctx,base);check(ctx.r3.u32==0);
        const auto handle=read(outputs);check(read(outputs+4)==i+2);
        ctx.r3.u64=handle;ctx.r4.u64=0;ctx.r5.u64=outputs+8;
        host_import("ObReferenceObjectByHandle",ctx,base);check(ctx.r3.u32==0);
        const auto object=read(outputs+8),thread_tls=read(object+0x68);
        check(thread_tls!=tls && read(thread_tls)==0);check(read(tls)==0x12345678);
        if(i==0)check(call("KeResumeThread",object)==1);
        check(call("NtResumeThread",handle,outputs+12)==0 && read(outputs+12)==(i==0?0u:1u));
        write64(outputs+32,uint64_t(-10000000ll));
        ctx.r3.u64=handle;ctx.r4.u64=1;ctx.r5.u64=0;ctx.r6.u64=outputs+32;
        host_import("NtWaitForSingleObjectEx",ctx,base);check(ctx.r3.u32==0);
        check(read(object+0x140)==0x820f56c0+i*4 && read(object+4)==1);
        call("ObDereferenceObject",object);check(call("NtClose",handle)==0);
    }
    check(call("KeTlsGetValue",0)==0x12345678);check(call("KeTlsFree",0)==1);
    // Byte widening/truncation and big-endian UTF-16 layout.
    base[outputs+64]='A';base[outputs+65]=0xe9;base[outputs+66]=0;
    ctx.r3.u64=outputs+80;ctx.r4.u64=4;ctx.r5.u64=outputs+16;ctx.r6.u64=outputs+64;ctx.r7.u64=3;
    host_import("RtlMultiByteToUnicodeN",ctx,base);
    check(ctx.r3.u32==0 && read(outputs+16)==4 && read(outputs+80)==0x004100e9);
    ctx.r3.u64=outputs+96;ctx.r4.u64=2;ctx.r5.u64=outputs+16;ctx.r6.u64=outputs+80;ctx.r7.u64=4;
    host_import("RtlUnicodeToMultiByteN",ctx,base);check(ctx.r3.u32==0 && read(outputs+16)==2 && base[outputs+96]=='A' && base[outputs+97]==0xe9);
    check(call("XamLoaderGetLaunchDataSize",outputs+16)==1168 && read(outputs+16)==0);
    check(call("XamLoaderGetLaunchDataSize",0)==87);
    check(call("XamLoaderGetLaunchData")==1168);
    call("KeQuerySystemTime",outputs);
    uint64_t time;std::memcpy(&time,base+outputs,8);time=__builtin_bswap64(time);
    check(time>116444736000000000ull); // Later than the Unix epoch in FILETIME.
    auto nanoseconds=[](){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
    uint64_t previous=0;
    for(unsigned i=0;i<1000;++i) {
        auto lower=nanoseconds()/20,ticks=ppc_read_timebase(),upper=nanoseconds()/20;
        check(lower<=ticks && ticks<=upper && ticks>=previous);previous=ticks;
    }
    const auto path=airborne::game_path("extracted/base","d:\\mohagame\\XBOX360TOC.TXT");
    check(!path.status && std::filesystem::is_regular_file(path.native));
    check(airborne::game_path("extracted/base","game:\\..\\default.xex").status==0xc0000033);
    check(airborne::game_path("extracted/base","ShaderDumpxe:\\CompareBackEnds").status==0xc000000f);
    const auto file_out=outputs+256,io=outputs+264,ansi=outputs+272,attrs=outputs+280,text=outputs+320,info=outputs+352;
    std::strcpy(reinterpret_cast<char*>(base+text),"D:\\MOHAGame\\Xbox360TOC.txt");
    call("RtlInitAnsiString",ansi,text);check(read(ansi)==0x001a001b && read(ansi+4)==text);
    write(attrs,0xfffffffd);write(attrs+4,ansi);write(attrs+8,0x40);
    auto open_file=[&](uint32_t access){ctx.r3.u64=file_out;ctx.r4.u64=access;ctx.r5.u64=attrs;ctx.r6.u64=io;ctx.r7.u64=0x60;
        host_import("NtOpenFile",ctx,base);return ctx.r3.u32;};
    check(open_file(0x40100080)==0xc0000022 && read(file_out)==0); // Asset mount stays read-only.
    check(open_file(0x80100080)==0);const auto file=read(file_out);
    ctx.r3.u64=file;ctx.r4.u64=io;ctx.r5.u64=info;ctx.r6.u64=56;ctx.r7.u64=34;host_import("NtQueryInformationFile",ctx,base);
    check(ctx.r3.u32==0 && read(io+4)==56 && read(info+44)==std::filesystem::file_size(path.native));
    auto read_file=[&](uint64_t offset,uint32_t bytes){write64(outputs+416,offset);ctx.r3.u64=file;ctx.r4.u64=0;ctx.r5.u64=0;ctx.r6.u64=0;
        ctx.r7.u64=io;ctx.r8.u64=outputs+448;ctx.r9.u64=bytes;ctx.r10.u64=outputs+416;host_import("NtReadFile",ctx,base);return ctx.r3.u32;};
    check(read_file(0x100000007ull,8)==0xc0000011 && read(io+4)==0); // Seek must not truncate to 32 bits.
    check(read_file(0,16)==0 && read(io+4)==16);
    std::ifstream original(path.native,std::ios::binary);char expected[16]{};original.read(expected,16);
    check(!std::memcmp(expected,base+outputs+448,16));
    write64(outputs+416,7);ctx.r3.u64=file;ctx.r4.u64=io;ctx.r5.u64=outputs+416;ctx.r6.u64=8;ctx.r7.u64=14;
    host_import("NtSetInformationFile",ctx,base);check(ctx.r3.u32==0 && read(io+4)==0);
    check(read_file(UINT64_MAX-1,8)==0 && !std::memcmp(expected+7,base+outputs+448,8));
    check(call("NtClose",file)==0);
    check(read_file(0,1)==0xc0000008 && read(io+4)==0);
    write(info,8);check(call("MmQueryStatistics",info)==0xc0000023 && read(info)==8);
    write(info,104);check(call("MmQueryStatistics",info)==0 && read(info+4)==0x20000 && read(info+100)==0x1ffff);
    check(uint64_t(read(info+8))+read(info+12)+read(info+24)+read(info+32)+read(info+36)+read(info+44)==0x20000);
    const auto params=outputs+416;std::memset(base+params,0,13);base[params]=12;
    check(call("NetDll_XNetStartup",1,params)==10022);base[params]=13;base[params+1]=1;
    check(call("NetDll_XNetStartup",1,params)==0 && call("NetDll_XNetStartup",1,params)==0);
    base[params+1]=2;check(call("NetDll_XNetStartup",1,params)==10037);
    check(call("NetDll_XNetCleanup",1)==0 && call("NetDll_XNetCleanup",1)==0 && call("NetDll_XNetCleanup",1)==10093);
    auto config=[&](uint32_t setting,uint32_t output,uint32_t size) {
        ctx.r3.u64=3;ctx.r4.u64=setting;ctx.r5.u64=output;ctx.r6.u64=size;ctx.r7.u64=outputs+68;
        host_import("ExGetXConfigSetting",ctx,base);return ctx.r3.u32;
    };
    check(config(9,0,0)==0 && (read(outputs+68)>>16)==4);
    write(outputs+64,0xdeadbeef);check(config(9,outputs+64,2)==0xc0000023 && read(outputs+64)==0xdeadbeef);
    check(config(9,outputs+64,4)==0 && read(outputs+64)==1);
    check(config(14,outputs+64,1)==0 && read(outputs+64)==0x67000001 && (read(outputs+68)>>16)==1);
    check(config(1,outputs+64,4)==0 && read(outputs+64)==0);check(call("XGetGameRegion")==0xffff);
    for(auto ticks:{uint64_t(0),uint64_t(116444736000000000ull),uint64_t(125962560000000000ull)}) {
        write64(outputs,ticks);call("RtlTimeToTimeFields",outputs,outputs+64);
        const auto year=ticks==0?1601u:ticks==116444736000000000ull?1970u:2000u;
        check((read(outputs+64)>>16)==year);
    }
    check(read(outputs+64)==0x07d00002 && read(outputs+68)==0x001d0000 && read(outputs+76)==2); // 2000-02-29 Tuesday.
    write(outputs+48,0);write(outputs+52,4096);ctx.r3.u64=outputs+48;ctx.r4.u64=outputs+52;ctx.r5.u64=0x3000;ctx.r6.u64=4;ctx.r7.u64=0;
    host_import("NtAllocateVirtualMemory",ctx,base);check(ctx.r3.u32==0);const auto queried=read(outputs+48);
    check(call("MmQueryAddressProtect",queried+123)==4);ctx.r3.u64=queried;ctx.r4.u64=4096;ctx.r5.u64=2;host_import("MmSetAddressProtect",ctx,base);
    check(call("MmQueryAddressProtect",queried+123)==2);
    ctx.r3.u64=outputs+48;ctx.r4.u64=outputs+52;ctx.r5.u64=0x8000;ctx.r6.u64=0;host_import("NtFreeVirtualMemory",ctx,base);
    check(ctx.r3.u32==0 && call("MmQueryAddressProtect",queried)==0);
    PPCContext clock{};clock.r1.u64=0x12345678;clock.lr=0x87654321;clock.r31.u64=0x1122334455667788;
    sub_82C5B7D8(clock,base);const auto before_tick=clock.r3.u32;std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (PPC_LOOKUP_FUNC(base,0x82c5b7d8))(clock,base);
    check(uint32_t(clock.r3.u32-before_tick)>=1 && clock.r1.u32==0x12345678 && clock.lr==0x87654321 && clock.r31.u64==0x1122334455667788);
    constexpr uint32_t wsa=0x10011000;std::memset(base+wsa,0x5a,400);write(wsa+396,0xcafebabe);
    auto sockets=[&](uint32_t version){ctx.r3.u64=1;ctx.r4.u64=version;ctx.r5.u64=wsa;host_import("NetDll_WSAStartup",ctx,base);return ctx.r3.u32;};
    check(sockets(0x101)==10092 && read(wsa)==0x5a5a5a5a);
    check(sockets(0x202)==0 && read(wsa)==0x02020202 && read(wsa+396)==0xcafebabe && (read(wsa+392)&65535)==0x5a5a);
    check(sockets(0x202)==0);check(call("NetDll_WSACleanup",1)==0 && call("NetDll_WSACleanup",1)==0 && call("NetDll_WSACleanup",1)==10093);
    constexpr uint32_t exception=wsa+512,name=wsa+1024;std::memset(base+exception,0,80);std::strcpy(reinterpret_cast<char*>(base+name),"Initial guest");
    write(exception,0x406d1388);write(exception+16,4);write(exception+20,0x1000);write(exception+24,name);write(exception+28,1);
    call("RtlRaiseException",exception);check(read(exception)==0x406d1388);
    write(exception,0xe06d7363);bool cpp_stop=false;try{call("RtlRaiseException",exception);}catch(const DiagnosticStop&){cpp_stop=true;}check(cpp_stop);
    std::printf("HOST_SERVICE_TESTS checks=%zu\n",checks);return checks;
}
