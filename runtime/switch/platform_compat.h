#pragma once
// Narrow platform adapter for the existing diagnostic startup services. These
// names describe the previously tested Windows backend, not a Win32 emulator.
#include "mapped_arena.h"
#include <mutex>
#include <ctime>
#include <limits>
using DWORD=uint32_t;
#define WINAPI
constexpr DWORD MEM_COMMIT=0x1000,MEM_RESERVE=0x2000,MEM_DECOMMIT=0x4000,MEM_RELEASE=0x8000;
constexpr DWORD PAGE_NOACCESS=1,PAGE_READONLY=2,PAGE_READWRITE=4;
constexpr DWORD INFINITE=0xffffffff,WAIT_OBJECT_0=0,WAIT_TIMEOUT=258,WAIT_FAILED=0xffffffff;
constexpr DWORD CREATE_SUSPENDED=4;
constexpr int THREAD_PRIORITY_ABOVE_NORMAL=1,THREAD_PRIORITY_NORMAL=0,THREAD_PRIORITY_BELOW_NORMAL=-1;
void nx_runtime_log(const char*,...);
[[noreturn]] void nx_runtime_stop(unsigned);
bool nx_runtime_stopping();
#include "thread_policy.h"
inline NxMappedArena* nx_arena=nullptr;
struct FILETIME {DWORD dwLowDateTime,dwHighDateTime;};
inline void GetSystemTimeAsFileTime(FILETIME* result) {
    timespec now{};if(clock_gettime(CLOCK_REALTIME,&now))throw std::runtime_error("System clock read failed");
    const uint64_t ticks=(uint64_t(now.tv_sec)+11644473600ull)*10000000ull+now.tv_nsec/100;
    result->dwLowDateTime=uint32_t(ticks);result->dwHighDateTime=uint32_t(ticks>>32);
}
inline void Sleep(DWORD milliseconds) {
    if(!milliseconds){svcSleepThread(0);return;}
    while(milliseconds && !nx_runtime_stopping()) {
        const DWORD chunk=std::min<DWORD>(milliseconds,10);svcSleepThread(int64_t(chunk)*1000000);milliseconds-=chunk;
    }
}
[[noreturn]] inline void ExitProcess(DWORD status){nx_runtime_stop(status);}
using CRITICAL_SECTION=std::recursive_mutex;
inline void InitializeCriticalSection(CRITICAL_SECTION*){}
inline void EnterCriticalSection(CRITICAL_SECTION* lock) {
    while(!lock->try_lock()) {
        if(nx_runtime_stopping())throw std::runtime_error("Diagnostic shutdown during lock wait");
        svcSleepThread(1000000);
    }
}
inline bool TryEnterCriticalSection(CRITICAL_SECTION* lock){return lock->try_lock();}
inline void LeaveCriticalSection(CRITICAL_SECTION* lock){lock->unlock();}
struct NxNativeThread {
    Thread thread{};DWORD (*entry)(void*)=nullptr;void* argument=nullptr;
    unsigned suspended=1;bool started=false;
};
using HANDLE=NxNativeThread*;
inline HANDLE CreateThread(void*,size_t,DWORD (*entry)(void*),void* argument,DWORD flags,void*) {
    auto* result=new NxNativeThread;result->entry=entry;result->argument=argument;
    auto trampoline=+[](void* argument) {
        auto* thread=static_cast<NxNativeThread*>(argument);thread->entry(thread->argument);
    };
    Result rc=threadCreate(&result->thread,trampoline,result,nullptr,2*1024*1024,nx_guest_priority,-2);
    nx_runtime_log("NATIVE_THREAD operation=create stack=2097152 rc=%08x\n",rc);
    if(R_FAILED(rc)){delete result;return nullptr;}
    rc=nx_configure_guest_thread(result->thread.handle,0);
    if(R_FAILED(rc)){nx_runtime_log("FAIL worker thread policy rc=%08x\n",rc);const auto cleanup=threadClose(&result->thread);nx_runtime_log("CLEANUP unstarted_worker rc=%08x\n",cleanup);delete result;return nullptr;}
    if(!(flags&CREATE_SUSPENDED)) {
        rc=threadStart(&result->thread);
        if(R_FAILED(rc)){threadClose(&result->thread);delete result;return nullptr;}
        result->started=true;result->suspended=0;
    }
    return result;
}
inline DWORD ResumeThread(HANDLE thread) {
    const DWORD previous=thread->suspended;
    if(previous) {
        const Result rc=threadStart(&thread->thread);
        nx_runtime_log("NATIVE_THREAD operation=start handle=%08x rc=%08x\n",thread->thread.handle,rc);
        if(R_FAILED(rc))return DWORD(-1);
        thread->suspended=0;thread->started=true;
    }
    return previous;
}
inline DWORD WaitForSingleObject(HANDLE thread,DWORD milliseconds) {
    uint64_t remaining=milliseconds==INFINITE ? UINT64_MAX:uint64_t(milliseconds)*1000000;
    do {
        const uint64_t chunk=std::min<uint64_t>(remaining,10000000);
        const Result rc=waitSingleHandle(thread->thread.handle,chunk);
        if(R_SUCCEEDED(rc))return WAIT_OBJECT_0;
        if(rc!=KERNELRESULT(TimedOut) || nx_runtime_stopping())return WAIT_FAILED;
        if(remaining!=UINT64_MAX)remaining-=chunk;
    }while(remaining);
    return WAIT_TIMEOUT;
}
inline bool SetThreadPriority(HANDLE thread,int priority) {
    const Result rc=nx_configure_guest_thread(thread->thread.handle,priority);
    nx_runtime_log("NATIVE_THREAD operation=priority value=%d rc=%08x\n",nx_guest_priority,rc);
    return R_SUCCEEDED(rc);
}
inline void* VirtualAlloc(void* address,size_t bytes,DWORD flags,DWORD protection) {
    if(!nx_arena || !address || flags!=MEM_COMMIT || (protection!=1 && protection!=2 && protection!=4))return nullptr;
    const auto offset=uintptr_t(address)-uintptr_t(nx_arena->base());
    const unsigned permission=protection==1 ? 0:protection==2 ? 1:3;
    return nx_arena->commit(offset,bytes,permission) ? address:nullptr;
}
inline bool VirtualFree(void* address,size_t bytes,DWORD flags) {
    if(!nx_arena || flags!=MEM_DECOMMIT)return false;
    return nx_arena->decommit(uintptr_t(address)-uintptr_t(nx_arena->base()),bytes);
}
inline bool VirtualProtect(void* address,size_t bytes,DWORD protection,DWORD* old) {
    if(!nx_arena || (protection!=1 && protection!=2 && protection!=4))return false;
    MemoryInfo info{};u32 page=0;const Result rc=svcQueryMemory(&info,&page,uintptr_t(address));
    if(R_FAILED(rc))return false;
    const unsigned permission=protection==1 ? 0:protection==2 ? 1:3;
    if(!nx_arena->protect(uintptr_t(address)-uintptr_t(nx_arena->base()),bytes,permission))return false;
    *old=info.perm==0 ? PAGE_NOACCESS:info.perm==1 ? PAGE_READONLY:PAGE_READWRITE;return true;
}
struct MEMORY_BASIC_INFORMATION {DWORD State;};
inline size_t VirtualQuery(void* address,MEMORY_BASIC_INFORMATION* output,size_t) {
    MemoryInfo info{};u32 page=0;const Result rc=svcQueryMemory(&info,&page,uintptr_t(address));
    if(R_FAILED(rc))return 0;
    output->State=(info.type&255)==0 ? MEM_RESERVE:MEM_COMMIT;return sizeof(*output);
}
class NxRuntimeGuestMemory {
    NxMappedArena arena_{nx_runtime_log};
public:
    uint8_t* base=nullptr;
    NxRuntimeGuestMemory() {
        const Result rc=arena_.open();if(R_FAILED(rc))throw std::runtime_error("Guest address reservation failed");
        nx_arena=&arena_;base=arena_.base();
    }
    void commit(uint64_t offset,uint64_t bytes) {
        if(!VirtualAlloc(base+offset,bytes,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("Guest commit failed");
    }
    // The supervisor always exits the entire process with guest workers alive.
    // Explicit close is available for loader failures before any worker starts.
    Result close(){const auto rc=arena_.close();if(R_SUCCEEDED(rc)){nx_arena=nullptr;base=nullptr;}return rc;}
};
