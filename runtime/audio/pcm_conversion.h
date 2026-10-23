#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
namespace airborne {
// XAudio's render frame is 256 samples of six big-endian planar floats.
// Output is stereo PCM16, with centre/surround mixed at -3dB and LFE omitted.
inline std::array<int16_t,512> stereo_pcm(const uint8_t* source) {
    std::array<int16_t,512> result{};
    auto sample=[&](unsigned channel,unsigned index) {
        const auto* p=source+4*(channel*256+index);
        const uint32_t bits=(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
        float value;std::memcpy(&value,&bits,4);return std::isfinite(value)?value:0.0f;
    };
    for(unsigned i=0;i<256;++i)for(unsigned side=0;side<2;++side) {
        float value=sample(side,i)+0.70710678f*(sample(2,i)+sample(4+side,i));
        value=std::fmax(-1.0f,std::fmin(1.0f,value));
        result[i*2+side]=int16_t(std::lround(value*(value<0?32768.0f:32767.0f)));
    }
    return result;
}

}
