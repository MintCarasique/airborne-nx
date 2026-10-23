#include "../../runtime/gpu/shader_library.h"
#include <fstream>
#include <iterator>
#include <iostream>

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
    airborne::ShaderLibrary library;library.load(bytes);
    if(library.shaders().empty() || library.corpus()<library.shaders().size())return 3;
    const size_t count=library.shaders().size();
    size_t lookups=0,rejections=0;
    for(const auto& shader:library.shaders()) {
        if(library.find(shader.original.data(),shader.original.size())!=&shader)return 4;
        for(size_t i=0;i<shader.inputs.size();++i)if(shader.inputs[i].location!=i)return 6;
        ++lookups;
    }
    auto reject=[&](std::vector<uint8_t> bad) {
        bool failed=false;
        try{library.load(bad);}catch(const std::runtime_error&){failed=true;}
        if(!failed || library.shaders().size()!=count)throw std::runtime_error("Non-transactional shader load");
        ++rejections;
    };
    for(size_t size:{size_t(0),size_t(7),size_t(31),bytes.size()-1})
        reject(std::vector<uint8_t>(bytes.begin(),bytes.begin()+size));
    auto bad=bytes;bad[0]^=1;reject(bad);
    bad=bytes;bad.back()^=1;reject(bad);
    auto put=[](std::vector<uint8_t>& b,size_t at,uint32_t value){for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(value>>(8*i));};
    for(auto fault:std::vector<std::pair<size_t,uint32_t>>{{8,2},{16,65537},{28,1},{40,0xffffffff},{44,3},{48,2},{56,0},{64+16,1}}) {
        bad=bytes;put(bad,fault.first,fault.second);
        // Recompute outer CRC so inner entry validation must reject the fault.
        put(bad,24,airborne::ShaderLibrary::crc(bad.data()+32,bad.size()-32));reject(bad);
    }
    auto missing=library.shaders().front().original;missing.back()^=1;
    if(library.find(missing.data(),missing.size()))return 5;
    std::cout<<"Real shader library: "<<lookups<<" exact lookups, "<<rejections<<" malformed loads rejected, missing shader rejected; corpus="<<library.corpus()<<"\n";
}
