#pragma once
#include <array>
#include <cstdint>
#include <utility>
namespace airborne {
// Guard the observed movie raster state. Other title draws remain unsupported.
inline bool video_draw_state(const std::array<uint32_t,0x5003>& r,uint32_t initiator) {
    if(initiator!=0x60084)return false;
    constexpr std::pair<uint32_t,uint32_t> values[]={
        {0x2000,0x14000500},{0x2001,0x2d0},{0x2002,0},{0x200e,0},{0x200f,0x20002000},
        {0x2080,0},{0x2081,0},{0x2082,0x2d00500},{0x2104,15},
        {0x2200,0x700734},{0x2201,0x10001},{0x2202,0x87000007},
        {0x2204,0x90000},{0x2205,0x18000},{0x2206,0x400},{0x2208,4},
        {0x2300,0x400},{0x2302,4},{0x2324,2}};
    for(auto value:values)if(r[value.first]!=value.second)return false;
    constexpr uint32_t constants[]={0x3f950a7f,0x400731db,0xbd800000,0xbf000000,0xbf0872f2,0xbe5a5b23,0x3f950a7f,0x3fe575a2,0,0,0,0};
    for(unsigned i=0;i<12;++i)if(r[0x47f4+i]!=constants[i])return false;
    for(unsigned i=0;i<3;++i){const auto at=0x4800+6*i;
        if(r[at]!=(i?0x06004802u:0x0a004802u) || (r[at+1]&0xfff)!=2 ||
           r[at+2]!=(i?0x2ce27fu:0x59e4ffu) || r[at+3]!=0x1281400 || r[at+4]!=3 || r[at+5]!=0x200)return false;
    }
    return (r[0x48be]&3)==3 && r[0x48bf]==0x1000007a;
}
}
