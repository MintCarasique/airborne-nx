#include "../../runtime/gpu/front_buffer.h"
#include "../../runtime/gpu/vulkan_device.h"
#include <cstdio>
#include <set>
#include <stdexcept>
int main() {
    auto require=[](bool ok){if(!ok)throw std::runtime_error("Front-buffer contract failed");};
    const uint32_t vectors[][3]={{0,0,0},{1,0,4},{4,0,32},{8,0,64},{16,0,128},{32,0,4096},{0,1,16},{0,2,256},{0,8,1152},{0,16,2048},{0,32,8192}};
    for(auto& v:vectors)require(airborne::tiled_rgba_offset(v[0],v[1],64)==v[2]);
    std::set<uint32_t> addresses;
    for(uint32_t y=0;y<64;++y)for(uint32_t x=0;x<64;++x)addresses.insert(airborne::tiled_rgba_offset(x,y,64));
    require(addresses.size()==4096 && *addresses.rbegin()==16380);
    for(bool tiled:{false,true})for(uint32_t endian=0;endian<4;++endian) {
        airborne::FrontBuffer layout{35,37,64,endian,tiled};
        std::vector<uint8_t> memory(layout.required_bytes()+16,0xa5);
        const uint8_t expected[]={17,34,51,68};
        const unsigned permutation[4][4]={{0,1,2,3},{1,0,3,2},{3,2,1,0},{2,3,0,1}};
        for(uint32_t y=0;y<layout.height;++y)for(uint32_t x=0;x<layout.width;++x)
            for(unsigned c=0;c<4;++c)memory[layout.offset(x,y)+permutation[endian][c]]=expected[c];
        auto pixels=layout.decode(memory.data(),memory.size()-16);
        for(size_t i=0;i<pixels.size();++i)require(pixels[i]==expected[i%4]);
        layout.swizzle=0xa0a; // Actual Airborne front-buffer B,G,R,one mapping.
        pixels=layout.decode(memory.data(),memory.size()-16);
        const uint8_t swapped[]={51,34,17,255};
        for(size_t i=0;i<pixels.size();++i)require(pixels[i]==swapped[i%4]);
        bool rejected=false;try{layout.decode(memory.data(),layout.required_bytes()-1);}catch(...){rejected=true;}require(rejected);
        for(size_t i=memory.size()-16;i<memory.size();++i)require(memory[i]==0xa5);
    }
    airborne::VulkanDevice device([](const char* line){std::puts(line);});device.initialize(1280,720);
    std::vector<uint8_t> pixels(1280*720*4);
    for(unsigned frame=0;frame<2;++frame) {
        for(uint32_t y=0;y<720;++y)for(uint32_t x=0;x<1280;++x) {
            const size_t at=(size_t(y)*1280+x)*4;
            pixels[at]=uint8_t(x+frame*71);pixels[at+1]=uint8_t(y);pixels[at+2]=uint8_t(x^y);pixels[at+3]=255;
        }
        device.display_rgba(pixels,1280,720);
    }
    bool rejected=false;try{device.display_rgba(pixels,1279,720);}catch(...){rejected=true;}require(rejected);
    require(device.has_display_frame());device.wait_vblank();device.wait_vblank();
    device.shutdown();device.shutdown();
    std::puts("Front-buffer address/endian/bounds and two real GPU transfer/readback frames passed; no game frame claimed");
}
