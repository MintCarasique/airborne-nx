#pragma once
// Test double for reservation/mapping API contracts, not Horizon emulation.
#include <cstdint>
#include <cstddef>
#include <cstdlib>
using Result=uint32_t;
using Handle=uint32_t;
using u32=uint32_t;
constexpr Handle INVALID_HANDLE=0;
constexpr unsigned Perm_Rw=3;
constexpr unsigned Perm_None=0;
constexpr unsigned Module_Libnx=345,LibnxError_BadInput=1,LibnxError_IncompatSysVer=2,LibnxError_OutOfMemory=3;
#define MAKERESULT(module,description) ((module)|((description)<<9))
#define R_FAILED(result) ((result)!=0)
#define R_SUCCEEDED(result) ((result)==0)
#define R_MODULE(result) ((result)&511)
#define R_DESCRIPTION(result) (((result)>>9)&8191)
struct VirtmemReservation { void* base;size_t size; };
struct MemoryInfo {uint64_t addr,size;uint32_t type,attr,perm,ipc_refcount,device_refcount,padding;};
bool envIsSyscallHinted(unsigned);
Handle envGetOwnProcessHandle();
void virtmemLock();
void virtmemUnlock();
void* virtmemFindAslr(size_t,size_t);
void* virtmemFindCodeMemory(size_t,size_t);
VirtmemReservation* virtmemAddReservation(void*,size_t);
void virtmemRemoveReservation(VirtmemReservation*);
Result svcMapProcessMemory(void*,Handle,uint64_t,uint64_t);
Result svcUnmapProcessMemory(void*,Handle,uint64_t,uint64_t);
Result svcMapProcessCodeMemory(Handle,uint64_t,uint64_t,uint64_t);
Result svcUnmapProcessCodeMemory(Handle,uint64_t,uint64_t,uint64_t);
Result svcSetProcessMemoryPermission(Handle,uint64_t,uint64_t,uint32_t);
Result svcQueryMemory(MemoryInfo*,uint32_t*,uint64_t);
Result svcSetMemoryPermission(void*,uint64_t,uint32_t);
// Alignment is the real allocator's responsibility; no pointers returned by
// this test double are passed to Horizon or dereferenced as guest addresses.
inline void* memalign(size_t,size_t bytes){return std::malloc(bytes);}
struct Thread {Handle handle=0;};
Result threadCreate(Thread*,void (*)(void*),void*,void*,size_t,int,int);
Result threadStart(Thread*);
Result threadClose(Thread*);
Result waitSingleHandle(Handle,uint64_t);
Result svcSetThreadPriority(Handle,int);
void svcSleepThread(int64_t);
#define KERNELRESULT(name) (0xea01u)
