#include <file.h>
#include <image.h>
#include <disasm.h>
#include <iostream>
#include <iomanip>
int main(int argc,char** argv) {
    if(argc<4) return 2;
    auto bytes=LoadFile(argv[1]);
    auto image=Image::ParseImage(bytes.data(),bytes.size());
    if(!image.data) return 3;
    auto start=std::stoull(argv[2],nullptr,16);
    auto count=std::stoul(argv[3],nullptr,0);
    for(size_t i=0;i<count;++i) {
        auto addr=start+i*4;
        if(addr<image.base || addr+4>image.base+image.size) return 4;
        ppc_insn insn{};
        ppc::Disassemble(image.data.get()+addr-image.base,addr,insn);
        std::cout<<std::hex<<addr<<": ";
        if(insn.opcode) std::cout<<insn.opcode->name<<' '<<insn.op_str;
        else std::cout<<"undecoded";
        std::cout<<'\n';
    }
}
