// Windows x64 startup probe. Deliberately incomplete Xbox runtime.
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include "diagnostic.h"
#include "runtime_bindings.h"
#include "xex_header.h"
#include "virtual_memory.h"
#include "address_space.h"
#include <cstdio>
#include <fstream>
#include <vector>
#include <unordered_set>
#include <bitset>
#include <mutex>
#include <memory>
#include <condition_variable>
#include <thread>

#include "../common/startup_services.inc"

namespace {
LONG CALLBACK fault_report(EXCEPTION_POINTERS* info) {
    const DWORD code=info->ExceptionRecord->ExceptionCode;
    if(code!=EXCEPTION_ACCESS_VIOLATION && code!=EXCEPTION_ILLEGAL_INSTRUCTION &&
       !(code>=EXCEPTION_FLT_DENORMAL_OPERAND && code<=EXCEPTION_FLT_UNDERFLOW) &&
       code!=EXCEPTION_INT_DIVIDE_BY_ZERO && code!=EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    std::fprintf(stderr,"HOST_FAULT code=0x%08lx",code);
    std::fprintf(stderr," native_pc=%p mxcsr=%08lx",info->ExceptionRecord->ExceptionAddress,info->ContextRecord->MxCsr);
    if(code==EXCEPTION_ACCESS_VIOLATION) {
        const auto address=info->ExceptionRecord->ExceptionInformation[1];
        std::fprintf(stderr," access=%llu address=0x%llx",(unsigned long long)info->ExceptionRecord->ExceptionInformation[0],(unsigned long long)address);
        if(address>=uintptr_t(active_base) && address-uintptr_t(active_base)<PPC_MEMORY_SIZE) {
            std::fprintf(stderr," guest=0x%08llx",(unsigned long long)(address-uintptr_t(active_base)));
            const auto guest=address-uintptr_t(active_base);
            for(const auto& binding:import_bindings)if(!binding.thunk) {
                const auto target=read32(active_base,binding.slot);
                if(target>=0x71000000 && guest>=target && guest<uint64_t(target)+0x10000)
                    std::fprintf(stderr," unsupported_variable=%s",binding.name);
            }
        }
    }
    if(active_context) std::fprintf(stderr," LR=0x%08x SP=0x%08x CTR=0x%08x",uint32_t(active_context->lr),active_context->r1.u32,active_context->ctr.u32);
    std::fprintf(stderr,"\n");std::fflush(stderr);ExitProcess(70);
}
}

bool host_tls_contention_check(uint8_t* base) {
    std::atomic<unsigned> done{0};std::atomic<bool> correct{true};
    std::unique_lock<std::mutex> blocked_services(service_mutex);
    std::thread workers[2];
    for(unsigned index=0;index<2;++index)workers[index]=std::thread([&,index]{
        current_tls_address=initial_tls+0x400+index*0x200;
        PPCContext context{};initialize_guest_fp(context);context.r13.u64=initial_pcr;
        for(unsigned i=0;i<2000;++i) {
            const auto value=0x55000000u+index*0x10000+i;
            context.r3.u64=63;context.r4.u64=value;host_import("KeTlsSetValue",context,base);
            if(context.r3.u32!=1)correct.store(false);
            context.r3.u64=63;host_import("KeTlsGetValue",context,base);
            if(context.r3.u32!=value)correct.store(false);
        }
        done.fetch_add(1);
    });
    const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    while(done.load()!=2 && std::chrono::steady_clock::now()<until)std::this_thread::yield();
    const bool independent=done.load()==2;blocked_services.unlock();
    for(auto& worker:workers)worker.join();
    std::printf("TLS_CONTENTION_CHECK service_mutex_held=1 workers_completed=%u separate_tls_values=%u operations=8000\n",independent?2:0,unsigned(correct.load()));
    return independent && correct.load();
}

int main(int argc,char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if(argc!=4 || (std::strcmp(argv[3],"--validate") && std::strcmp(argv[3],"--entry")))return 2;
    try {
        auto image=read_file(argv[1],PPC_IMAGE_SIZE);
        auto xex=read_file(argv[2],0x1000000);
        if(image.size()!=PPC_IMAGE_SIZE || image[0]!='M' || image[1]!='Z' ||
           xex.size()<0x4000 || std::memcmp(xex.data(),"XEX2",4))throw DiagnosticStop("Wrong input image");
        GuestMemory memory;auto* base=memory.base;
        active_base=base;
        GuestVirtualAllocator virtual_allocator(base);allocator=&virtual_allocator;
        GuestPhysicalAllocator physical(base);physical_allocator=&physical;
        memory.commit(PPC_IMAGE_BASE,PPC_IMAGE_SIZE);
        std::memcpy(base+PPC_IMAGE_BASE,image.data(),image.size());
        memory.commit(table_address,PPC_CODE_SIZE*2);
        for(uint64_t offset=0;offset<PPC_CODE_SIZE;offset+=4)
            PPC_LOOKUP_FUNC(base,PPC_CODE_BASE+offset)=unmapped_target;
        std::unordered_set<size_t> seen;
        for(auto* m=PPCFuncMappings;m->host;++m) {
            if(m->guest<PPC_CODE_BASE || m->guest>=PPC_CODE_BASE+PPC_CODE_SIZE ||
               m->guest%4 || !seen.insert(m->guest).second)throw DiagnosticStop("Invalid function mapping");
            PPC_LOOKUP_FUNC(base,m->guest)=m->host;
        }
        DWORD old_protection;
        if(!VirtualProtect(base+table_address,PPC_CODE_SIZE*2,PAGE_READONLY,&old_protection))
            throw DiagnosticStop("Cannot protect lookup table");
        memory.commit(stack_bottom,stack_size); // Adjacent pages remain inaccessible.
        memory.commit(module_variable,0x2000);
        memory.commit(header,0x4000);
        memory.commit(initial_thread,0x3000);
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
        register_initial_native_thread();
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
        std::printf("MEMORY_READY image=%zu mappings=%zu stack=%u bindings=%zu leaf_cases=12 guards=2\n",image.size(),seen.size(),stack_size,verified_bindings);
        if(!std::strcmp(argv[3],"--validate")) {host_service_self_test(base);return 0;}
        PPCContext ctx{};initialize_guest_fp(ctx);ctx.r1.u64=stack_bottom+stack_size-0x100;ctx.lr=0;ctx.r13.u64=initial_pcr;
        active_base=base;active_context=&ctx;
        auto handler=AddVectoredExceptionHandler(1,fault_report);
        host_entry_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
        try {
            std::printf("ENTRY 0x82c5c310 SP=0x%08x R13=0x%08x (single guest thread)\n",ctx.r1.u32,ctx.r13.u32);
            std::fflush(stdout);
            (PPC_LOOKUP_FUNC(base,0x82c5c310))(ctx,base);
        } catch(const std::exception& e) {
            std::fprintf(stderr,"STARTUP_STOP %s LR=0x%08x SP=0x%08x CTR=0x%08x\n",e.what(),uint32_t(ctx.lr),ctx.r1.u32,ctx.ctr.u32);
            {
                std::lock_guard<std::mutex> guard(service_mutex);
                imports_stopping=true;
                  log_critical_import_totals();
                  if(initial_native_thread){const auto closed=CloseHandle(initial_native_thread);initial_native_thread=nullptr;std::printf("INITIAL_THREAD_HANDLE_CLEANUP rc=%u\n",unsigned(closed));}
                  close_game_files();
                  if(!socket_runtime.close())std::fprintf(stderr,"SOCKET_CLEANUP_ERROR\n");
                  dispatcher_changed.notify_all();
                  if(audio_client) {
                      audio_client->active=false;audio_client->changed.notify_all();
                      try{audio_client->output.close();}
                      catch(const std::exception& error){std::fprintf(stderr,"AUDIO_CLEANUP_ERROR %s\n",error.what());}
                  }
                if(graphics_device)try{graphics_device->shutdown();}
                catch(const std::exception& error){std::fprintf(stderr,"GPU_CLEANUP_ERROR %s\n",error.what());}
            }
            if(!threads.empty()){std::fflush(stdout);std::fflush(stderr);ExitProcess(10);}
            if(handler)RemoveVectoredExceptionHandler(handler);
            active_context=nullptr;return 10;
        } catch(...) {
            if(handler)RemoveVectoredExceptionHandler(handler);
            active_context=nullptr;throw;
        }
        if(handler)RemoveVectoredExceptionHandler(handler);
        active_context=nullptr;
        std::printf("ENTRY_RETURN r3=0x%08x\n",ctx.r3.u32);return 11;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"LOADER_ERROR %s\n",e.what());return 1;
    }
}
