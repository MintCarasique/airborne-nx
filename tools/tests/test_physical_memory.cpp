#include "address_space.h"
#include "ppc_context.h"
#include <cstdio>
#include <string>
extern PPC_FUNC(test_physical_lwz);
extern PPC_FUNC(test_physical_stw);
extern PPC_FUNC(test_physical_lvx);
extern PPC_FUNC(test_physical_stvx);
extern PPC_FUNC(test_physical_lwarx);
extern PPC_FUNC(test_physical_stwcx);
extern PPC_FUNC(test_physical_dcbz);
int main() {
    GuestMemory memory;auto* base=memory.base;GuestPhysicalAllocator physical(base);
    size_t checks=0;
    auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Physical test failed at "+std::to_string(checks));};
    const uint32_t guest=physical.allocate(4096,0x404,0,0xffffffff,32);
    check(guest>=0xe0000000);
    const uint32_t offset=GuestPhysicalAllocator::physical(guest);
    check(offset && offset%4096==0);
    const uint32_t aliases[]={0xa0000000+offset,0xc0000000+offset,guest};
    PPCContext ctx{};
    for(auto address:aliases) {
        ctx.r4.u64=address;ctx.r3.u64=0x12345678;test_physical_stw(ctx,base);
        for(auto target:aliases) {
            auto* p=ppc_guest_ptr(base,target);check(p[0]==0x12 && p[1]==0x34 && p[2]==0x56 && p[3]==0x78);
            ctx.r4.u64=target;test_physical_lwz(ctx,base);check(ctx.r3.u64==0x12345678);
        }
    }
    for(auto address:aliases) {
        for(unsigned i=0;i<16;++i)ctx.v1.u8[i]=uint8_t(i+1);
        ctx.r4.u64=address;test_physical_stvx(ctx,base);
        for(auto target:aliases)for(unsigned i=0;i<16;++i)check(ppc_guest_ptr(base,target)[i]==16-i);
        for(unsigned i=0;i<16;++i)ppc_guest_ptr(base,address)[i]=uint8_t(i+32);
        ctx.r4.u64=address;test_physical_lvx(ctx,base);
        for(unsigned i=0;i<16;++i)check(ctx.v1.u8[i]==47-i);
        ctx.r3.u64=0x99887766;test_physical_stw(ctx,base);test_physical_lwarx(ctx,base);check(ctx.r3.u32==0x99887766);
        ctx.r3.u64=0xdeadbeef;test_physical_stwcx(ctx,base);check(ctx.cr0.eq);
        for(auto target:aliases){ctx.r4.u64=target;test_physical_lwz(ctx,base);check(ctx.r3.u32==0xdeadbeef);}
        ctx.r4.u64=address+17;std::memset(ppc_guest_ptr(base,address),0x7a,64);test_physical_dcbz(ctx,base);
        for(auto target:aliases)for(unsigned i=0;i<64;++i)check(ppc_guest_ptr(base,target)[i]==(i<32 ? 0:0x7a));
    }
    check(physical.allocate(0,4,0,0xffffffff,0)==0);
    check(physical.allocate(4096,0x40,0,0xffffffff,0)==0);
    check(physical.allocate(4096,4,0x10000,0x10fff,0)==0xe000f000);
    check(physical.allocate(8192,4,0x10000,0x10fff,0)==0);
    auto large=physical.allocate(1,0x20000004,0,0xffffffff,0);
    check(large>=0xa0000000 && large<0xc0000000 && (large&65535)==0);
    auto huge=physical.allocate(1,0x80000004,0,0xffffffff,0);
    check(huge>=0xc0000000 && huge<0xe0000000 && (huge&0xffffff)==0);
    check(physical.free(guest));check(!physical.free(guest));
    MEMORY_BASIC_INFORMATION info{};VirtualQuery(base+0xa0000000ull+offset,&info,sizeof(info));check(info.Protect==PAGE_NOACCESS);
    std::printf("%zu physical allocation/alias/emitted memory-instruction checks passed\n",checks);
}
