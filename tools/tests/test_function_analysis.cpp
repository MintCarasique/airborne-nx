#include <function.h>
#include <array>
#include <cstdint>
#include <iostream>

int main() {
    constexpr size_t base = 0x10000;
    std::array<uint8_t, 0x40> code{};
    auto word = [&](size_t offset, uint32_t value) {
        for (int i=0;i<4;++i) code[offset+i]=uint8_t(value>>(24-i*8));
    };
    word(0,0x60000000); // nop
    word(4,0x60000000);
    word(8,0x4e800420); // bctr
    word(0x20,0x38600001); // li r3,1
    word(0x24,0x4e800020); // blr
    word(0x30,0x38600002); // li r3,2
    word(0x34,0x4e800020);
    auto plain=Function::Analyze(code.data(),code.size(),base);
    if (plain.size!=12) return 1;
    Function::BranchTargets targets{{base+8,{base+0x20,base+0x30,base+0x20}}};
    auto switched=Function::Analyze(code.data(),code.size(),base,&targets);
    if(switched.size!=0x38 || switched.SearchBlock(base+0x20)==size_t(-1) ||
       switched.SearchBlock(base+0x30)==size_t(-1)) return 2;
    // Invalid targets must not lead to reads outside the supplied code range.
    targets[base+8].insert(targets[base+8].end(),{base-4,base+0x40,base+0x21});
    auto bounded=Function::Analyze(code.data(),code.size(),base,&targets);
    if(bounded.size!=0x38) return 3;
    // Nested switch and a self edge should converge without losing successors.
    word(0x20,0x4e800420);
    targets[base+0x20]={base+0x20,base+0x30};
    auto nested=Function::Analyze(code.data(),code.size(),base,&targets);
    if(nested.size!=0x38) return 4;
    std::array<uint8_t,4> last{{0x38,0x60,0,1}};
    auto end=Function::Analyze(last.data(),last.size(),base);
    if(end.size!=4) return 5;
    // Forward tail calls must not absorb another known function after a switch.
    word(0x20,0x48000010); // b +0x10
    std::set<size_t> entries{base,base+0x30};
    targets[base+8]={base+0x20};
    targets.erase(base+0x20);
    auto tail=Function::Analyze(code.data(),code.size(),base,&targets,&entries);
    if(tail.size!=0x24) return 6;
    std::cout << "6 function-analysis scenarios passed\n";
}
