#include <cstdint>
#include <initializer_list>
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
using Handle=uint32_t;using Result=uint32_t;using u64=uint64_t;using s32=int32_t;
constexpr int InfoType_PriorityMask=1,Module_Libnx=345,LibnxError_BadInput=11;
constexpr Handle CUR_PROCESS_HANDLE=0xffff8001;
#define MAKERESULT(m,d) ((m)|((d)<<9))
#define R_FAILED(r) ((r)!=0)
void nx_runtime_log(const char*,...){}
static int step=0,fail_step=0,sets=0;static u64 priorities=1ull<<59,cores=1;static int observed=59;
Result status(){return ++step==fail_step?1234:0;}
Result svcGetInfo(u64* out,int type,Handle handle,u64){if(type!=1 || handle!=CUR_PROCESS_HANDLE)throw std::runtime_error("bad query");*out=priorities;return status();}
Result svcGetThreadCoreMask(s32* core,u64* mask,Handle){*core=0;*mask=cores;return status();}
Result svcSetThreadPriority(Handle,int priority){if(priority!=59)throw std::runtime_error("non-preemptive priority");++sets;return status();}
Result svcGetThreadPriority(s32* out,Handle){*out=observed;return status();}
#include "../../runtime/switch/thread_policy.h"
int main(){
 auto require=[](bool ok){if(!ok)throw std::runtime_error("thread policy contract");};
 for(int requested:{-1,0,1}){step=sets=0;require(nx_configure_guest_thread(42,requested)==0 && sets==1 && step==4);}
 for(int failure=1;failure<=4;++failure){step=sets=0;fail_step=failure;require(nx_configure_guest_thread(42,0)==1234 && step==failure);}
 fail_step=0;priorities=0;step=sets=0;require(nx_configure_guest_thread(42,0)!=0 && sets==0);
 priorities=1ull<<59;for(u64 mask:{0ull,8ull,15ull}){cores=mask;step=sets=0;require(nx_configure_guest_thread(42,0)!=0 && sets==0);}
 cores=1;observed=44;step=0;require(nx_configure_guest_thread(42,0)!=0);
 std::puts("THREAD_POLICY_PASS normal/guest-priority requests, disallowed priorities/cores, every syscall failure, readback mismatch; no target scheduling claim");
}

