#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
extern "C" float roundevenf(float) noexcept;
static uint32_t bits(float v) { uint32_t b; std::memcpy(&b,&v,4); return b; }
static float value(uint32_t b) { float v; std::memcpy(&v,&b,4); return v; }
static uint32_t reference(uint32_t b) {
    if ((b & 0x7fffffffu) >= 0x4b000000u) return b;
    const double x=std::fabs(double(value(b)));
    double n=std::floor(x), fraction=x-n;
    if(fraction>0.5 || (fraction==0.5 && std::fmod(n,2.0)!=0)) ++n;
    return bits(float(n)) | (b & 0x80000000u);
}
int main() {
    size_t count=0;
    // Every exponent, both signs, endpoints and mantissas around halfway.
    for(int mode : {FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
        std::fesetround(mode);
        uint32_t random=0x39c4a187;
        for(unsigned i=0;i<300000;++i) {
            random^=random<<13; random^=random>>17; random^=random<<5;
            const uint32_t b=i<8192 ? ((i/32)<<23)|(i%2 ? 0x80000000u:0u)|
                ((i%32)/2==0 ? 0u : ((i%32)/2==1 ? 0x7fffffu : ((i%32)/2)*0x80000u-1)) : random;
            volatile float input=value(b);
            const uint32_t actual=bits(roundevenf(input));
            if(actual!=reference(b) || std::fegetround()!=mode) {
                std::printf("Mismatch %08x -> %08x expected %08x mode %d\n",b,actual,reference(b),mode);
                return 1;
            }
            ++count;
        }
        for(int n=-1000;n<=1000;++n) {
            const float x=float(n)+0.5f;
            for(float v : {std::nextafter(x,-INFINITY),x,std::nextafter(x,INFINITY)}) {
                if(bits(roundevenf(v))!=reference(bits(v))) return 2;
                ++count;
            }
        }
    }
    std::printf("%zu roundevenf checks passed across four rounding modes\n",count);
}
