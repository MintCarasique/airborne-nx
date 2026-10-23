#include <switch.h>
#include "platform_compat.h"
#include "../host/diagnostic.h"
#include "runtime_bindings.h"
#include "runtime_pins.h"
#include "../host/xex_header.h"
#include "../host/virtual_memory.h"
#include "../host/address_space.h"
#include <cstdio>
#include <fstream>
#include <vector>
#include <unordered_set>
#include <bitset>
#include <memory>
#include <condition_variable>
#include <atomic>
#include <cstdarg>
#include <fcntl.h>
#include <unistd.h>
#include "../io/buffered_report.h"

static std::atomic<bool> stopping{false};
static std::atomic<unsigned> stop_status{UINT32_MAX};
static std::atomic<bool> report_failed{false};
// Only the supervisor owns consoleUpdate/consoleExit. A guest Vulkan call
// requests an exclusive handoff, then creates the VI surface on its own thread.
static std::atomic<unsigned> display_owner{0}; // 0 console, 1 requested, 2 Vulkan
static int crash_fd=-1;
static FILE* runtime_report=nullptr;
static airborne::BufferedReport report_writer;
static char bundle_path[1024];
static bool game_content_ready=false;
bool nx_runtime_stopping(){return stopping.load(std::memory_order_acquire);}
void nx_runtime_acquire_display() {
    unsigned expected=0;display_owner.compare_exchange_strong(expected,1);
    const auto started=armGetSystemTick();
    while(display_owner.load(std::memory_order_acquire)!=2) {
        if(nx_runtime_stopping() || armTicksToNs(armGetSystemTick()-started)>3000000000ull)
            throw DiagnosticStop("Native display ownership handoff cancelled/timed out");
        svcSleepThread(1000000);
    }
}
void nx_runtime_log(const char* format,...) __attribute__((format(printf,1,2)));
void nx_runtime_error(const char* format,...) __attribute__((format(printf,1,2)));
static void nx_report_failed() {
    if(!report_failed.exchange(true)) {
        const auto stats=report_writer.stats();char line[192];
        const int bytes=std::snprintf(line,sizeof(line),"REPORT_IO_FAIL first_errno=%d writes=%llu flushes=%llu\n",stats.first_error,(unsigned long long)stats.writes,(unsigned long long)stats.flushes);
        if(crash_fd>=0)write(crash_fd,line,size_t(bytes));
    }
    unsigned expected=UINT32_MAX;stop_status.compare_exchange_strong(expected,1);stopping.store(true);
}
void nx_runtime_log(const char* format,...) {
    const bool immediate=!std::strncmp(format,"FAIL",4) || !std::strncmp(format,"STARTUP_STOP",12) || !std::strncmp(format,"THREAD_STOP",11) || !std::strncmp(format,"WATCHDOG",8) ||
        !std::strncmp(format,"CLEANUP",7) || !std::strncmp(format,"SHUTDOWN",8) ||
        !std::strncmp(format,"EXIT_ACTION",11) || !std::strncmp(format,"RESULT",6);
    va_list args;va_start(args,format);const bool ok=report_writer.write(immediate,format,args);va_end(args);
    if(!ok)nx_report_failed();
}
void nx_runtime_error(const char* format,...) {
    va_list args;va_start(args,format);const bool ok=report_writer.write(true,format,args);va_end(args);
    if(!ok)nx_report_failed();
}
void nx_runtime_flush(bool immediate=true) {
    if(!report_writer.flush(immediate))nx_report_failed();
}
[[noreturn]] void nx_runtime_stop(unsigned status) {
    unsigned expected=UINT32_MAX;stop_status.compare_exchange_strong(expected,status);
    stopping.store(true,std::memory_order_release);threadExit();
}

#define STARTUP_PRINT(...) nx_runtime_log(__VA_ARGS__)
#define STARTUP_ERROR(...) nx_runtime_error(__VA_ARGS__)
#define STARTUP_FLUSH() nx_runtime_flush(false)
#include "../common/startup_services.inc"

extern "C" PPC_FUNC(nx_unwind_bridge);
extern "C" PPC_FUNC(nx_unwind_throw){throw DiagnosticStop("unwind_probe");}
static void require_hash(const std::vector<uint8_t>& bytes,const uint8_t* expected,const char* label) {
    uint8_t actual[32];sha256CalculateHash(actual,bytes.data(),bytes.size());
    if(std::memcmp(expected,actual,32))throw DiagnosticStop(std::string("SHA256 mismatch: ")+label);
    nx_runtime_log("INPUT_SHA256 %s=PASS bytes=%zu\n",label,bytes.size());
}

static void run_initial_guest(void*) {
    PPCContext ctx{};initialize_guest_fp(ctx);bool entered=false;
    try {
        register_initial_native_thread();
        if(!game_content_ready)throw DiagnosticStop("Game content missing; see GAME_CONTENT_CHECK paths in log");
        bool unwound=false;
        try{nx_unwind_bridge(ctx,nullptr);}catch(const DiagnosticStop& e){unwound=std::strcmp(e.what(),"unwind_probe")==0;}
        if(!unwound)throw DiagnosticStop("Generated-frame exception unwinding failed");
        nx_runtime_log("PREFLIGHT generated_frame_unwind=PASS\n");
        char path[1200];std::snprintf(path,sizeof(path),"%s/image.bin",bundle_path);
        auto image=read_file(path,PPC_IMAGE_SIZE);require_hash(image,runtime_image_sha256,"image");
        std::snprintf(path,sizeof(path),"%s/xex-header.bin",bundle_path);
        auto xex=read_file(path,0x4000);require_hash(xex,runtime_header_sha256,"xex_header");
        if(image.size()!=PPC_IMAGE_SIZE || xex.size()!=0x4000)throw DiagnosticStop("Input size mismatch");
        auto* memory=new GuestMemory;auto* base=memory->base;
        active_base=base;
        allocator=new GuestVirtualAllocator(base);
        physical_allocator=new GuestPhysicalAllocator(base);
        memory->commit(PPC_IMAGE_BASE,PPC_IMAGE_SIZE);
        std::memcpy(base+PPC_IMAGE_BASE,image.data(),image.size());
        memory->commit(table_address,(PPC_CODE_SIZE*2+4095)&~uint64_t(4095));
        for(uint64_t offset=0;offset<PPC_CODE_SIZE;offset+=4)
            PPC_LOOKUP_FUNC(base,PPC_CODE_BASE+offset)=unmapped_target;
        std::unordered_set<size_t> seen;
        for(auto* m=PPCFuncMappings;m->host;++m) {
            if(m->guest<PPC_CODE_BASE || m->guest>=PPC_CODE_BASE+PPC_CODE_SIZE ||
               m->guest%4 || !seen.insert(m->guest).second)throw DiagnosticStop("Invalid function mapping");
            PPC_LOOKUP_FUNC(base,m->guest)=m->host;
        }
        DWORD old_protection;
        if(!VirtualProtect(base+table_address,(PPC_CODE_SIZE*2+4095)&~uint64_t(4095),PAGE_READONLY,&old_protection))
            throw DiagnosticStop("Cannot protect lookup table");
        memory->commit(stack_bottom,stack_size); // Adjacent pages remain inaccessible.
        memory->commit(module_variable,0x2000);
        memory->commit(header,0x4000);
        memory->commit(initial_thread,0x3000);
        base[initial_thread]=6; // Thread dispatcher object type.
        write32(base,initial_thread+0x5c,stack_bottom+stack_size);
        write32(base,initial_thread+0x60,stack_bottom);
        write32(base,initial_thread+0x68,initial_tls);
        write32(base,initial_thread+0x14c,1);
        write32(base,initial_thread+0x150,0x82c5c310);
        write32(base,initial_pcr,initial_tls);
        write32(base,initial_pcr+0x30,initial_pcr);
        write32(base,initial_pcr+0x70,stack_bottom+stack_size);
        write32(base,initial_pcr+0x74,stack_bottom);
        write32(base,initial_pcr+0x100,initial_thread);
        std::memcpy(base+header,xex.data(),0x4000);
        bind_imports(base);
        size_t verified_bindings=0;
        for(const auto& binding:import_bindings) {
            if(binding.thunk && read32(base,binding.slot)!=binding.thunk)throw DiagnosticStop("Import thunk binding mismatch");
            if(!binding.thunk && read32(base,binding.slot)<module_variable)throw DiagnosticStop("Unresolved ordinal in variable slot");
            ++verified_bindings;
        }
        MEMORY_BASIC_INFORMATION guard{};
        for(uint64_t address : {uint64_t(stack_bottom)-0x1000,uint64_t(stack_bottom)+stack_size}) {
            if(!VirtualQuery(base+address,&guard,sizeof(guard)) || guard.State!=MEM_RESERVE)
                throw DiagnosticStop("Stack guard is accessible");
        }
        bool missing_stopped=false;
        try {
            PPCContext ctx{};(PPC_LOOKUP_FUNC(base,PPC_CODE_BASE+PPC_CODE_SIZE-4))(ctx,base);
        } catch(const DiagnosticStop&) {missing_stopped=true;}
        if(!missing_stopped)throw DiagnosticStop("Missing-target handler did not stop");
        // Exercise the full linked lookup table using the real guest leaf.
        for(unsigned index=0;index<12;++index) {
            PPCContext ctx{};ctx.r3.u64=index;
            (PPC_LOOKUP_FUNC(base,0x828b6d00))(ctx,base);
            if(ctx.r3.u32!=0x820f56c0+index*4)throw DiagnosticStop("Mapped leaf mismatch");
        }
        if(std::memcmp(base+PPC_IMAGE_BASE+0x1000,image.data()+0x1000,0x1000))throw DiagnosticStop("Unexpected image mutation");
        nx_runtime_log("MEMORY_READY image=%zu mappings=%zu stack=%u bindings=%zu leaf_cases=12 guards=2\n",image.size(),seen.size(),stack_size,verified_bindings);
        ctx.r1.u64=stack_bottom+stack_size-0x100;ctx.r13.u64=initial_pcr;
        active_context=&ctx;entered=true;
        nx_runtime_log("ENTRY 0x82c5c310 SP=0x%08x R13=0x%08x\n",ctx.r1.u32,ctx.r13.u32);
        (PPC_LOOKUP_FUNC(base,0x82c5c310))(ctx,base);
        nx_runtime_log("ENTRY_RETURN r3=0x%08x\n",ctx.r3.u32);
        nx_runtime_stop(11);
    }catch(const ThreadExit&) {
        threadExit();
    }catch(const std::exception& error) {
        nx_runtime_log("%s %s LR=0x%08x SP=0x%08x CTR=0x%08x\n",entered ? "STARTUP_STOP":"LOADER_ERROR",error.what(),uint32_t(ctx.lr),ctx.r1.u32,ctx.ctr.u32);
        nx_runtime_stop(entered ? 10:1);
    }
}

extern "C" {
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size=sizeof(__nx_exception_stack);
void __libnx_exception_handler(ThreadExceptionDump* dump) {
    // Separate O_APPEND descriptor avoids a FILE lock held by the faulted thread.
    char buffer[1800];int count=std::snprintf(buffer,sizeof(buffer),
        "NATIVE_FAULT description=%x esr=%08x pc=%016llx lr=%016llx sp=%016llx far=%016llx\n",
        dump->error_desc,dump->esr,(unsigned long long)dump->pc.x,(unsigned long long)dump->lr.x,
        (unsigned long long)dump->sp.x,(unsigned long long)dump->far.x);
    if(crash_fd>=0)write(crash_fd,buffer,size_t(count));
    if(active_base && dump->far.x>=uintptr_t(active_base) && dump->far.x-uintptr_t(active_base)<0x100000000ull) {
        count=std::snprintf(buffer,sizeof(buffer),"GUEST_FAULT window_offset=%08llx (E-window pointer bias may apply)\n",(unsigned long long)(dump->far.x-uintptr_t(active_base)));
        if(crash_fd>=0)write(crash_fd,buffer,size_t(count));
    }
    for(unsigned i=0;i<29;++i) {
        count=std::snprintf(buffer,sizeof(buffer),"NATIVE_REGISTER x%u=%016llx\n",i,(unsigned long long)dump->cpu_gprs[i].x);
        if(crash_fd>=0)write(crash_fd,buffer,size_t(count));
    }
    if(crash_fd>=0) {const char end[]="CLEANUP=PROCESS_EXIT\nRESULT=FAIL\n";write(crash_fd,end,sizeof(end)-1);fsync(crash_fd);}
    svcExitProcess();
}
}

int main(int argc,char** argv) {
    consoleInit(nullptr);
    bool console_active=true;
    std::snprintf(bundle_path,sizeof(bundle_path),"sdmc:/switch/airborne-nx");
    if(argc>0 && argv && argv[0]) {
        char candidate[1024];std::snprintf(candidate,sizeof(candidate),"%s",argv[0]);
        if(auto* slash=std::strrchr(candidate,'/')){*slash=0;std::snprintf(bundle_path,sizeof(bundle_path),"%s",candidate);}
    }
    game_files_root=std::string(bundle_path)+"/game";
    char result_path[1200];std::snprintf(result_path,sizeof(result_path),"%s/result.txt",bundle_path);
    std::printf("AirborneNX game entry diagnostic v0.7.4\nNot a playable game.\nWriting %s\n",result_path);consoleUpdate(nullptr);
    runtime_report=std::fopen(result_path,"w");
    if(!runtime_report) {std::printf("Cannot create result.txt\n");consoleUpdate(nullptr);svcExitProcess();}
    if(!report_writer.attach(runtime_report)){std::printf("Cannot buffer result.txt\n");consoleUpdate(nullptr);svcExitProcess();}
    fcntl(fileno(runtime_report),F_SETFL,O_APPEND);
    crash_fd=open(result_path,O_WRONLY|O_APPEND);
    nx_runtime_log("AirborneNX ARM64 game entry v0.7.4\nNot a playable game.\nBUILD=%s\n",AIRBORNE_ENTRY_BUILD_ID);
    {
        std::error_code error;
        const auto has_toc=[&](const std::string& path){error.clear();return std::filesystem::is_regular_file(std::filesystem::path(path)/"MOHAGame/Xbox360TOC.txt",error);};
        if(!has_toc(game_files_root)) {
            const auto previous=(std::filesystem::path(bundle_path).parent_path()/"airborne-nx-entry-v0.7/game").string();
            if(has_toc(previous))game_files_root=previous;
        }
        game_content_ready=true;
        for(const char* relative:{"MOHAGame/Xbox360TOC.txt","MOHAGame/Config/Xenon/Cooked/Coalesced.ini","MOHAGame/CookedMovies/MOHA_Shell_EAID.wmv"}) {
            const auto path=(std::filesystem::path(game_files_root)/relative).string();error.clear();
            const auto bytes=std::filesystem::file_size(path,error);
            const bool valid=!error && bytes>0;game_content_ready&=valid;
            nx_runtime_log("GAME_CONTENT_CHECK path=%s status=%s bytes=%llu error=%d\n",path.c_str(),valid?"FOUND":"MISSING",valid?(unsigned long long)bytes:0,error.value());
        }
        nx_runtime_log("GAME_CONTENT_ROOT path=%s ready=%u\n",game_files_root.c_str(),game_content_ready);
    }
    nx_runtime_log("HOS=%08x applet_type=%u process_handle=%08x\n",hosversionGet(),unsigned(appletGetAppletType()),envGetOwnProcessHandle());
    nx_runtime_log("NATIVE_ANCHOR symbol=nx_unwind_throw address=%016llx\n",(unsigned long long)uintptr_t(&nx_unwind_throw));
    nx_runtime_log("RUNTIME_LOCKS service_mutex=%016llx report_writer=%016llx\n",(unsigned long long)uintptr_t(&service_mutex),(unsigned long long)uintptr_t(&report_writer));
    for(unsigned id:{2u,3u,4u,5u,6u,7u,12u,13u,14u,15u}) {
        u64 value=0;const Result rc=svcGetInfo(&value,id,CUR_PROCESS_HANDLE,0);
        nx_runtime_log("PROCESS_INFO id=%u value=%016llx rc=%08x\n",id,(unsigned long long)value,rc);
    }
    if(!envIsSyscallHinted(0x02)){nx_runtime_log("FAIL SetMemoryPermission capability unavailable\nRESULT=FAIL\n");svcExitProcess();}
    Thread initial{};Result rc=threadCreate(&initial,run_initial_guest,nullptr,nullptr,4*1024*1024,nx_guest_priority,-2);
    nx_runtime_log("INITIAL_THREAD create_rc=%08x native_stack=4194304\n",rc);
    if(R_SUCCEEDED(rc)){rc=nx_configure_guest_thread(initial.handle,0);if(R_FAILED(rc)){const auto cleanup=threadClose(&initial);nx_runtime_log("CLEANUP unstarted_initial rc=%08x\n",cleanup);}}
    if(R_SUCCEEDED(rc)){rc=threadStart(&initial);nx_runtime_log("INITIAL_THREAD start_rc=%08x\n",rc);}
    if(R_FAILED(rc)){nx_runtime_log("FAIL initial thread rc=%08x\n",rc);stop_status.store(1);stopping.store(true);}
    const uint64_t started=armGetSystemTick();
    bool watchdog=false;
    while(appletMainLoop() && !nx_runtime_stopping()) {
        if(armTicksToNs(armGetSystemTick()-started)>120000000000ull) {unsigned expected=UINT32_MAX;watchdog=stop_status.compare_exchange_strong(expected,124);stopping.store(true);break;}
        if(display_owner.load(std::memory_order_acquire)==1) {
            consoleExit(nullptr);console_active=false;
            nx_runtime_log("DISPLAY_HANDOFF console_closed=1 owner=VULKAN\n");
            display_owner.store(2,std::memory_order_release);
        }
        if(console_active)consoleUpdate(nullptr);svcSleepThread(10000000);
    }
    stopping.store(true,std::memory_order_release);
    // Freeze first, then cooperatively wake/finish all workers before touching
    // guest mappings. No unsafe return to hbloader with borrowed heap pages.
    if(watchdog) {
        ThreadContext context{};const Result pause=threadPause(&initial);
        const Result snapshot=R_SUCCEEDED(pause) ? threadDumpContext(&context,&initial):pause;
        nx_runtime_log("WATCHDOG timeout_seconds=120 pause_rc=%08x snapshot_rc=%08x pc=%016llx sp=%016llx\n",pause,snapshot,(unsigned long long)context.pc.x,(unsigned long long)context.sp);
        if(R_SUCCEEDED(snapshot))nx_runtime_log("WATCHDOG_NATIVE fp=%016llx lr=%016llx x0=%016llx x1=%016llx x2=%016llx x3=%016llx\n",(unsigned long long)context.fp,(unsigned long long)context.lr,(unsigned long long)context.cpu_gprs[0].x,(unsigned long long)context.cpu_gprs[1].x,(unsigned long long)context.cpu_gprs[2].x,(unsigned long long)context.cpu_gprs[3].x);
        if(R_SUCCEEDED(pause)){const Result resumed=threadResume(&initial);nx_runtime_log("WATCHDOG resume_for_shutdown_rc=%08x\n",resumed);}
    }
    bool locked=false;
    for(unsigned retry=0;retry<100 && !(locked=service_mutex.try_lock());++retry)svcSleepThread(10000000);
    if(!locked)nx_runtime_log("FAIL service lock did not quiesce\n");
    if(locked){dispatcher_changed.notify_all();if(audio_client)audio_client->changed.notify_all();for(auto& pair:events)pair.second->changed.notify_all();for(auto& pair:event_objects)pair.second->changed.notify_all();for(auto& pair:semaphore_objects)pair.second->changed.notify_all();for(auto& pair:semaphore_handles)pair.second->changed.notify_all();}
    nx_runtime_log("CLEANUP_BEGIN mode=cooperative\nEXIT_STATUS=%u\nRESULT=%s\n",stop_status.load(),watchdog || stop_status.load()!=10 ? "FAIL":"STOPPED");
    if(locked)service_mutex.unlock();
    nx_runtime_flush();if(crash_fd>=0)fsync(crash_fd);
    if(!locked) {
        nx_runtime_log("CLEANUP=FAILED_SERVICE_LOCK\nEXIT_ACTION=HOME_REQUIRED\n");
        std::printf("Safe cleanup failed. Use HOME to close this application.\n");
        for(;;){if(console_active)consoleUpdate(nullptr);svcSleepThread(100000000);}
    }
    bool safe=locked;
    if(R_SUCCEEDED(rc)) {
        const Result joined=waitSingleHandle(initial.handle,3000000000ull);
        nx_runtime_log("SHUTDOWN initial_join_rc=%08x\n",joined);
        safe &= R_SUCCEEDED(joined);
        if(R_SUCCEEDED(joined)){const Result closed=threadClose(&initial);nx_runtime_log("SHUTDOWN initial_close_rc=%08x\n",closed);safe &= R_SUCCEEDED(closed);}
    }else if(initial.handle!=INVALID_HANDLE) {
        const Result closed=threadClose(&initial);safe &= R_SUCCEEDED(closed);
        nx_runtime_log("SHUTDOWN unstarted_initial_close_rc=%08x\n",closed);
    }
    for(auto& pair:threads) {
        auto* native=pair.second->handle;if(!native)continue;
        if(native->started) {
            const Result joined=waitSingleHandle(native->thread.handle,3000000000ull);
            nx_runtime_log("SHUTDOWN worker=%u join_rc=%08x\n",pair.second->id,joined);
            if(R_FAILED(joined)){safe=false;continue;}
        }
        const Result closed=threadClose(&native->thread);
        nx_runtime_log("SHUTDOWN worker=%u close_rc=%08x\n",pair.second->id,closed);
        if(R_SUCCEEDED(closed)){delete native;pair.second->handle=nullptr;}else safe=false;
    }
    if(safe && audio_client) {
        try{audio_client->output.close();nx_runtime_log("SHUTDOWN audio=PASS\n");}
        catch(const std::exception& error){nx_runtime_log("SHUTDOWN audio=FAIL reason=%s\n",error.what());safe=false;}
    }
    if(safe && graphics_device) {
        try{graphics_device->shutdown();nx_runtime_log("SHUTDOWN graphics=PASS\n");}
        catch(const std::exception& error){nx_runtime_log("SHUTDOWN graphics=FAIL reason=%s\n",error.what());safe=false;}
    }
    if(safe && !close_game_files())safe=false;
    if(safe && !socket_runtime.close()){nx_runtime_log("SHUTDOWN sockets=FAIL\n");safe=false;}
    if(safe && nx_arena) {
        const Result unmapped=nx_arena->close();
        nx_runtime_log("SHUTDOWN guest_memory_close_rc=%08x\n",unmapped);safe &= R_SUCCEEDED(unmapped);
    }
    if(!safe) {
        // Never unmap executable/heap data out from under a surviving worker.
        nx_runtime_log("CLEANUP=FAILED_RESOURCES_RETAINED\nEXIT_ACTION=HOME_REQUIRED\n");
        std::printf("Safe cleanup failed. Use HOME to close this application.\n");
        for(;;){if(console_active)consoleUpdate(nullptr);svcSleepThread(100000000);}
    }
    log_critical_import_totals();
    threads.clear();events.clear();event_objects.clear();semaphore_objects.clear();semaphore_handles.clear();
    // Raw guest critical-section locks can remain owned by an exited guest;
    // do not destroy an owned native mutex. Loader heap teardown reclaims them.
    critical_sections.clear();
    critical_section_users.clear();
    nx_runtime_log("CLEANUP=PASS\nEXIT_ACTION=RETURN_TO_LOADER\n");
    const auto report_stats=report_writer.stats();
    nx_runtime_log("REPORT_IO writes=%llu bytes=%llu explicit_flushes=%llu first_errno=%d buffer_bytes=65536 periodic_flush_ms=250\n",(unsigned long long)report_stats.writes,(unsigned long long)report_stats.bytes,(unsigned long long)report_stats.flushes,report_stats.first_error);
    nx_runtime_flush();
    // Never reopen the framebuffer while NVK still owns a VI swapchain.
    if(!console_active){consoleInit(nullptr);console_active=true;display_owner.store(0);}
    std::printf("\nGame startup stopped, status=%u.\nLog: result.txt\nPress + to exit.\n",stop_status.load());
    PadState pad;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);
    while(appletMainLoop()){padUpdate(&pad);if(padGetButtonsDown(&pad)&HidNpadButton_Plus)break;consoleUpdate(nullptr);svcSleepThread(16000000);}
    nx_runtime_flush();report_writer.detach();std::fclose(runtime_report);runtime_report=nullptr;
    if(crash_fd>=0){close(crash_fd);crash_fd=-1;}
    consoleExit(nullptr);
    return 0;
}
