#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace airborne {
// Xenos 32-bit texel address permutation (32x32 macrotiles). The bit
// arrangement is documented by Xenia's texture/util.cpp GetTiledOffset2D
// and the original Xbox 360 tiling research linked there.
inline uint32_t tiled_rgba_offset(uint32_t x,uint32_t y,uint32_t pitch) {
    const uint32_t macro=((x>>5)+(y>>5)*(pitch>>5))<<9;
    const uint32_t micro=((x&7)+((y&14)<<2))<<2;
    const uint32_t offset=macro+((micro&~15u)<<1)+(micro&15)+((y&1)<<4);
    return ((offset&~511u)<<3)+((y&16)<<7)+((offset&448)<<2)+
        (((((y&8)>>2)+(x>>3))&3)<<6)+(offset&63);
}
struct FrontBuffer {
    uint32_t width,height,pitch,endian;
    bool tiled;
    uint32_t swizzle=0x688; // XYZW, 3 bits per component; 4=zero, 5=one.
    void validate() const {
        if(!width || !height || width>4096 || height>4096 || pitch<width || pitch>16352 || pitch%32 || endian>3)
            throw std::runtime_error("Invalid Xenos front-buffer geometry/endian");
        if(swizzle>4095)throw std::runtime_error("Invalid Xenos front-buffer swizzle");
        for(unsigned c=0;c<4;++c)if(((swizzle>>(c*3))&7)>5)throw std::runtime_error("Reserved Xenos front-buffer swizzle component");
    }
    uint32_t offset(uint32_t x,uint32_t y) const {return tiled?tiled_rgba_offset(x,y,pitch):(y*pitch+x)*4;}
    uint32_t required_bytes() const {
        validate();uint32_t extent=0;
        if(!tiled)return ((height-1)*pitch+width)*4;
        // Each 32x32 macrotile owns a disjoint 4096-byte interval, ordered
        // by (y/32)*(pitch/32)+x/32. Only the last occupied tile can hold
        // the highest byte, but its partial rows/columns must all be checked.
        for(uint32_t y=(height-1)&~31u;y<height;++y)for(uint32_t x=(width-1)&~31u;x<width;++x)
            if(offset(x,y)+4>extent)extent=offset(x,y)+4;
        return extent;
    }
    std::vector<uint8_t> decode(const uint8_t* source,size_t bytes) const {
        if(!source || bytes<required_bytes())throw std::runtime_error("Truncated Xenos front buffer");
        std::vector<uint8_t> result(size_t(width)*height*4);
        // Format 6 stores R,G,B,A in successive bytes after endian conversion.
        const uint32_t permutation[4][4]={{0,1,2,3},{1,0,3,2},{3,2,1,0},{2,3,0,1}};
        for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
            const auto input=source+offset(x,y);auto output=result.data()+(size_t(y)*width+x)*4;
            for(unsigned c=0;c<4;++c) {
                const auto component=(swizzle>>(c*3))&7;
                output[c]=component<4?input[permutation[endian][component]]:component==4?0:255;
            }
        }
        return result;
    }
};
}
