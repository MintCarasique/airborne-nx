#pragma once
#include <cstdint>
#include <type_traits>

struct PPCContext;
extern "C" uint64_t airborne_gpu_mmio_read(PPCContext&,uint32_t,unsigned);
extern "C" void airborne_gpu_mmio_write(PPCContext&,uint32_t,uint64_t,unsigned);

namespace airborne {
inline bool gpu_mmio(uint32_t address){const auto block=address&0xffff0000u;return block==0x7fc80000u || block==0x7fea0000u;}
template<class T> inline T swap_guest(T value) {
    if constexpr(sizeof(T)==1)return value;
    if constexpr(sizeof(T)==2)return __builtin_bswap16(value);
    if constexpr(sizeof(T)==4)return __builtin_bswap32(value);
    if constexpr(sizeof(T)==8)return __builtin_bswap64(value);
}
template<class T> inline T guest_load(uint8_t* base,PPCContext& ctx,uint32_t address) {
    if(gpu_mmio(address))return T(airborne_gpu_mmio_read(ctx,address,sizeof(T)));
    auto* pointer=base+uint64_t(address)+(address>=0xe0000000u ? 0x1000ull:0);
    return swap_guest(*reinterpret_cast<volatile T*>(pointer));
}
template<class T> inline void guest_store(uint8_t* base,PPCContext& ctx,uint32_t address,T value) {
    if(gpu_mmio(address)){airborne_gpu_mmio_write(ctx,address,value,sizeof(T));return;}
    auto* pointer=base+uint64_t(address)+(address>=0xe0000000u ? 0x1000ull:0);
    *reinterpret_cast<volatile T*>(pointer)=swap_guest(value);
}
}
#define PPC_LOAD_U8(x) airborne::guest_load<uint8_t>(base,ctx,uint32_t(x))
#define PPC_LOAD_U16(x) airborne::guest_load<uint16_t>(base,ctx,uint32_t(x))
#define PPC_LOAD_U32(x) airborne::guest_load<uint32_t>(base,ctx,uint32_t(x))
#define PPC_LOAD_U64(x) airborne::guest_load<uint64_t>(base,ctx,uint32_t(x))
#define PPC_STORE_U8(x,y) airborne::guest_store<uint8_t>(base,ctx,uint32_t(x),uint8_t(y))
#define PPC_STORE_U16(x,y) airborne::guest_store<uint16_t>(base,ctx,uint32_t(x),uint16_t(y))
#define PPC_STORE_U32(x,y) airborne::guest_store<uint32_t>(base,ctx,uint32_t(x),uint32_t(y))
#define PPC_STORE_U64(x,y) airborne::guest_store<uint64_t>(base,ctx,uint32_t(x),uint64_t(y))
