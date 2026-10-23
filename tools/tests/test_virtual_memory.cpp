#include "virtual_memory.h"
#include <cstdio>
#include <stdexcept>
int main() {
    auto* base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
    if(!base)return 2;
    size_t checks=0;
    auto check=[&](bool ok){++checks;if(!ok){std::printf("VM check %zu failed\n",checks);throw std::runtime_error("VM test");}};
    GuestVirtualAllocator vm(base);
    uint32_t a=0,n=0x100000;
    check(!vm.allocate(a,n,0x60002000,4));check(a==0x40000000 && n==0x100000);
    MEMORY_BASIC_INFORMATION info{};VirtualQuery(base+a,&info,sizeof(info));check(info.State==MEM_RESERVE);
    uint32_t b=a+7,s=1;check(!vm.allocate(b,s,0x60001000,4));check(b==a && s==65536);
    for(unsigned i=0;i<s;++i)check(base[b+i]==0);
    base[b]=0x5a;check(!vm.allocate(b,s,0x1000,4));check(base[b]==0x5a);
    uint32_t old=0;check(!vm.protect(b,s,2,old));check(old==4);
    VirtualQuery(base+b,&info,sizeof(info));check(info.Protect==PAGE_READONLY);
    check(!vm.protect(b,s,4,old));check(old==2);
    uint32_t uncommitted=a+65536,u=1;check(vm.protect(uncommitted,u,2,old)!=0);
    b=a;s=65536;check(!vm.free(b,s,0x4000));
    VirtualQuery(base+b,&info,sizeof(info));check(info.State==MEM_RESERVE);
    check(!vm.allocate(b,s,0x1000,4));check(base[b]==0);
    b=a;s=0;check(!vm.free(b,s,0x8000));check(s==0x100000);
    b=0;s=1;check(!vm.allocate(b,s,0x3000,4));check(b==0x20000000 && s==4096);
    base[b]=3;
    uint32_t c=0,t=1;check(!vm.allocate(c,t,0x103000,4));check(c==0x3ffff000);
    uint32_t d=0,v=0xfffff000;check(!vm.allocate(d,v,0x3000,4));check(v==4096 && d==b+4096);
    for(uint32_t flags:{0u,0x80000u,0x10000u,0x80003000u}) {
        uint32_t x=0,y=4096;check(vm.allocate(x,y,flags,4)!=0);check(x==0 && y==4096);
    }
    uint32_t x=b,y=4096;check(vm.allocate(x,y,0x3000,4)!=0);check(base[b]==3);
    x=0;y=0;check(vm.allocate(x,y,0x3000,4)!=0);
    x=0;y=0x40001000;check(vm.allocate(x,y,0x3000,4)!=0);
    x=0x70000000;y=1;check(vm.allocate(x,y,0x3000,4)!=0);
    x=0;y=4096;check(vm.allocate(x,y,0x3000,0x40)!=0);
    x=b;y=8192;check(vm.protect(x,y,2,old)!=0); // Cannot cross reservations.
    x=b+4;y=0;check(vm.free(x,y,0x8000)!=0);
    x=b;y=0;check(!vm.free(x,y,0x8000));check(y==4096);
    x=0;y=1;check(!vm.allocate(x,y,0x3000,4));check(x==b);
    x=0;y=1;check(!vm.allocate(x,y,0x60103000,4));check(x==0x5fff0000 && y==65536);
    VirtualFree(base,0,MEM_RELEASE);
    std::printf("%zu guest VM checks passed\n",checks);
}
