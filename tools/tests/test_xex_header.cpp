#include "xex_header.h"
#include <array>
#include <cstdio>
#include <functional>

int main() {
    std::array<uint8_t,80> b{};
    std::memcpy(b.data(),"XEX2",4);
    auto write=[&](unsigned at,uint32_t v) {
        for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(v>>(24-8*i));
    };
    write(20,4);
    write(24,0x10100);write(28,0x82c5c310);
    write(32,0x10201);write(36,0x82000000);
    write(40,0x20104);write(44,64);
    write(48,0x183ff);write(52,56);write(56,8);
    constexpr uint32_t base=0x70010000;
    size_t checks=0;
    auto check=[&](bool v){++checks;if(!v)throw std::runtime_error("Header test failed");};
    auto field=[&](uint32_t key){return xex_header_field(b.data(),b.size(),base,key);};
    check(field(0x10100)==0x82c5c310); // Immediate value, not a pointer.
    check(field(0x10201)==base+36); // Pointer into table, not image base.
    check(field(0x20104)==base+64); // Valid payload ends exactly at bound.
    check(field(0x183ff)==base+56);
    check(field(0x20401)==0);
    auto reject=[&](const std::function<void()>& f) {
        bool rejected=false;try{f();}catch(const std::runtime_error&){rejected=true;}check(rejected);
    };
    reject([&]{xex_header_field(b.data(),23,base,0);});
    b[0]='Y';reject([&]{field(0);});b[0]='X';
    write(20,UINT32_MAX);reject([&]{field(0);});write(20,4);
    write(44,65);reject([&]{field(0x20104);});write(44,64);
    write(52,79);reject([&]{field(0x183ff);});write(52,56);
    write(56,25);reject([&]{field(0x183ff);});write(56,8);
    write(56,0);reject([&]{field(0x183ff);});write(56,8);
    reject([&]{xex_header_field(b.data(),b.size(),0xfffffff0,0x10201);});
    std::printf("%zu XEX header checks passed\n",checks);
}
