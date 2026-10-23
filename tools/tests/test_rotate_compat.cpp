#include "compiler_compat.h"
#include <cstdio>
#include <initializer_list>
template<class T> T reference(T value,unsigned count) {
    for(unsigned n=0;n<count%(sizeof(T)*8);++n)
        value=T((value<<1)|(value>>(sizeof(T)*8-1)));
    return value;
}
int main() {
    size_t checks=0;uint64_t seed=0xc135fa2395bafe11ull;
    for(unsigned input=0;input<1030;++input) {
        seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;
        uint64_t value=input==0 ? 0:input==1 ? ~0ull:input==2 ? 1:input==3 ? 1ull<<63:input==4 ? 0xaaaaaaaaaaaaaaaaull:input==5 ? 0x5555555555555555ull:seed;
        for(unsigned count=0;count<256;++count) {
            if(nx_ppc_rotl32(uint32_t(value),count)!=reference(uint32_t(value),count) ||
               nx_ppc_rotl64(value,count)!=reference(value,count))return 1;
            checks+=2;
        }
        for(unsigned count:{0xffffffffu,0xfffffffeu,0x80000000u}) {
            if(nx_ppc_rotl32(uint32_t(value),count)!=reference(uint32_t(value),count) ||
               nx_ppc_rotl64(value,count)!=reference(value,count))return 2;
            checks+=2;
        }
    }
    std::printf("%zu GCC rotate compatibility comparisons passed (bit-step reference)\n",checks);
}
