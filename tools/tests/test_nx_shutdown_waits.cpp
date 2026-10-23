#include "platform_compat.h"
#include <atomic>
#include <thread>
#include <future>
#include <chrono>
#include <cstdio>
static std::atomic<bool> stopping{false};
static std::atomic<unsigned> calls{0};
static std::atomic<uint64_t> slept{0};
static Result wait_result=KERNELRESULT(TimedOut);
static unsigned checks=0;
bool nx_runtime_stopping(){return stopping.load();}
void nx_runtime_log(const char*,...){}
[[noreturn]] void nx_runtime_stop(unsigned){std::abort();}
void svcSleepThread(int64_t ns){++calls;slept+=ns;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
Result waitSingleHandle(Handle,uint64_t){++calls;std::this_thread::sleep_for(std::chrono::milliseconds(1));return wait_result;}
static void check(bool okay){++checks;if(!okay)std::abort();}
static void await_call(){for(unsigned n=0;calls.load()==0 && n<1000;++n)std::this_thread::sleep_for(std::chrono::milliseconds(1));check(calls.load()>0);}
int main(){
    Sleep(25);check(calls==3 && slept==25000000);
    calls=0;slept=0;Sleep(0);check(calls==1 && slept==0);
    calls=0;
    auto delayed=std::async(std::launch::async,[]{Sleep(INFINITE-1);return true;});
    await_call();stopping=true;check(delayed.wait_for(std::chrono::milliseconds(250))==std::future_status::ready);check(delayed.get());
    stopping=false;calls=0;
    NxNativeThread thread;thread.thread.handle=7;
    auto waiting=std::async(std::launch::async,[&]{return WaitForSingleObject(&thread,INFINITE);});
    await_call();stopping=true;check(waiting.wait_for(std::chrono::milliseconds(250))==std::future_status::ready);check(waiting.get()==WAIT_FAILED);
    stopping=false;
    check(WaitForSingleObject(&thread,0)==WAIT_TIMEOUT);
    wait_result=0;check(WaitForSingleObject(&thread,0)==WAIT_OBJECT_0);
    wait_result=123;check(WaitForSingleObject(&thread,100)==WAIT_FAILED);
    CRITICAL_SECTION lock;lock.lock();calls=0;
    auto locked=std::async(std::launch::async,[&]{try{EnterCriticalSection(&lock);}catch(const std::runtime_error&){return true;}return false;});
    await_call();stopping=true;check(locked.wait_for(std::chrono::milliseconds(250))==std::future_status::ready);check(locked.get());lock.unlock();
    std::printf("%u cooperative shutdown wait checks passed (native adapter with mocked SVCs)\n",checks);
}
