#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <cstring>

// Return the guest-visible result for an optional XEX header key. A key
// ending in 00 is an immediate; 01 points to the entry value; others point
// to header-relative data. Bounds are limited to the copied header region.
inline uint32_t xex_header_field(const uint8_t* bytes,size_t size,uint32_t guest,uint32_t key) {
    auto read=[&](size_t offset) {
        if(offset>size || size-offset<4)throw std::runtime_error("Truncated XEX header");
        return uint32_t(bytes[offset])<<24 | uint32_t(bytes[offset+1])<<16 |
            uint32_t(bytes[offset+2])<<8 | bytes[offset+3];
    };
    if(size<24 || std::memcmp(bytes,"XEX2",4))throw std::runtime_error("Invalid XEX header magic");
    const uint32_t count=read(20);
    if(count>(size-24)/8)throw std::runtime_error("Invalid XEX header count");
    for(uint32_t i=0;i<count;++i) {
        const uint32_t at=24+i*8;
        if(read(at)!=key)continue;
        uint32_t offset=0;
        if((key&255)==0)return read(at+4);
        if((key&255)==1)offset=at+4;
        else {
            offset=read(at+4);
            const uint64_t length=(key&255)==255 ? (offset<size ? read(offset) : 0) : uint64_t(key&255)*4;
            if(!length || offset>size || length>size-offset)throw std::runtime_error("Invalid XEX field bounds");
        }
        if(uint64_t(guest)+offset>UINT32_MAX)throw std::runtime_error("XEX guest address overflow");
        return guest+offset;
    }
    return 0;
}
