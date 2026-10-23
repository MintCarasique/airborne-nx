#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>

namespace airborne {
class ShaderLibrary {
public:
    struct VertexInput {uint32_t location,fetch_instruction,original_metadata;};
    struct Shader {
        uint64_t key;
        bool vertex;
        uint32_t spec_constants;
        std::vector<uint8_t> original;
        std::vector<uint32_t> spirv;
        std::vector<VertexInput> inputs;
    };
    static uint64_t key(const uint8_t* data,size_t bytes) {
        uint64_t value=0xcbf29ce484222325ull;
        for(size_t i=0;i<bytes;++i)value=(value^data[i])*0x100000001b3ull;
        return value;
    }
    static uint32_t crc(const uint8_t* data,size_t bytes) {
        uint32_t value=0xffffffff;
        for(size_t i=0;i<bytes;++i) {
            value^=data[i];
            for(unsigned bit=0;bit<8;++bit)value=(value>>1)^(0xedb88320u&uint32_t(-int32_t(value&1)));
        }
        return ~value;
    }
    // Transactional: failed loads retain all previous shaders and lookup pointers.
    void load(const std::vector<uint8_t>& data) {
        const auto range=[&](size_t at,size_t size) {
            if(at>data.size() || size>data.size()-at)throw std::runtime_error("Truncated shader library");
        };
        const auto u32=[&](size_t at) {
            range(at,4);return uint32_t(data[at])|(uint32_t(data[at+1])<<8)|
                (uint32_t(data[at+2])<<16)|(uint32_t(data[at+3])<<24);
        };
        range(0,32);
        if(std::memcmp(data.data(),"AIRSPV01",8) || u32(8)!=1 || u32(12)!=data.size() ||
           u32(28) || u32(16)>65536 || u32(16)>u32(20) || u32(20)>65536 ||
           crc(data.data()+32,data.size()-32)!=u32(24))throw std::runtime_error("Invalid shader library header/checksum");
        std::vector<Shader> pending;
        size_t at=32;
        for(uint32_t i=0;i<u32(16);++i) {
            range(at,32);
            const uint64_t hash=u32(at)|(uint64_t(u32(at+4))<<32);
            const uint32_t original_size=u32(at+8),spv_size=u32(at+12),stage=u32(at+16),mask=u32(at+20);
            const uint32_t original_crc=u32(at+24),spv_crc=u32(at+28);
            if(!original_size || original_size>16*1024*1024 || spv_size<20 ||
               spv_size>16*1024*1024 || spv_size%4 || stage>1)
                throw std::runtime_error("Invalid shader library entry sizes/stage");
            at+=32;range(at,original_size);
            const uint8_t* original=data.data()+at;
            if(key(original,original_size)!=hash || crc(original,original_size)!=original_crc)
                throw std::runtime_error("Original shader checksum mismatch");
            at+=(size_t(original_size)+3)&~size_t(3);range(at,spv_size);
            if(u32(at)!=0x07230203 || !u32(at+12) || u32(at+16) || crc(data.data()+at,spv_size)!=spv_crc)
                throw std::runtime_error("SPIR-V checksum/header mismatch");
            for(size_t word=20;word<spv_size;) {
                const uint32_t count=u32(at+word)>>16;
                if(!count || size_t(count)*4>spv_size-word)throw std::runtime_error("Invalid SPIR-V instruction bounds");
                word+=size_t(count)*4;
            }
            Shader shader{hash,stage==1,mask,{original,original+original_size},{},{}};
            shader.spirv.resize(spv_size/4);
            for(size_t word=0;word<shader.spirv.size();++word)shader.spirv[word]=u32(at+word*4);
            if(shader.vertex) {
                const auto be=[&](size_t offset) {
                    if(offset>shader.original.size() || 4>shader.original.size()-offset)
                        throw std::runtime_error("Truncated original vertex interface");
                    const auto* p=shader.original.data()+offset;
                    return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
                };
                if(be(0)!=0x102a1101 || uint64_t(be(4))+be(8)!=shader.original.size())
                    throw std::runtime_error("Invalid original vertex container");
                const size_t offset=be(24),virtual_size=be(4);
                if(offset>virtual_size || 36>virtual_size-offset)
                    throw std::runtime_error("Vertex interface outside virtual container");
                const size_t skip=be(offset+24),elements=be(offset+28);
                if(skip>(virtual_size-offset-36)/4 || elements>(virtual_size-offset-36)/4-skip)
                    throw std::runtime_error("Vertex element table outside container");
                for(uint32_t element=0;element<elements;++element) {
                    const auto metadata=be(offset+36+(skip+element)*4);
                    const auto fetch=metadata&0xfff;
                    for(const auto& input:shader.inputs)if(input.fetch_instruction==fetch)
                        throw std::runtime_error("Duplicate vertex fetch interface key");
                    shader.inputs.push_back({element,fetch,metadata});
                }
            }
            pending.push_back(std::move(shader));at+=spv_size;
        }
        if(at!=data.size())throw std::runtime_error("Trailing shader library data");
        shaders_=std::move(pending);corpus_=u32(20);
    }
    const Shader* find(const uint8_t* original,size_t bytes) const {
        const auto hash=key(original,bytes);
        for(const auto& shader:shaders_)if(shader.key==hash && shader.original.size()==bytes &&
            !std::memcmp(shader.original.data(),original,bytes))return &shader;
        return nullptr;
    }
    const std::vector<Shader>& shaders() const{return shaders_;}
    uint32_t corpus() const{return corpus_;}
private:
    std::vector<Shader> shaders_;
    uint32_t corpus_=0;
};
}
