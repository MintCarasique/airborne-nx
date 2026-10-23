#pragma once
#include <array>
#include <vector>
#include <functional>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <map>

namespace airborne {
// Xenos PM4 consumer. Physical callbacks transfer raw little-endian host words;
// the command stream and read-pointer writeback are big-endian. No draw is
// acknowledged until a renderer implements it. Unsupported operations fail at
// their packet, preserving the last completely consumed primary packet.
class CommandRing {
public:
    using Read=std::function<uint32_t(uint32_t)>;
    using Write=std::function<void(uint32_t,uint32_t)>;
    using Log=std::function<void(const char*)>;
    using Present=std::function<void(uint32_t,const std::array<uint32_t,6>&)>;
    void set_present(Present present){present_=std::move(present);}
    struct ShaderCode {uint32_t address=0;std::vector<uint32_t> words;};
    using Clear=std::function<bool(const std::array<uint32_t,0x5003>&,const std::array<ShaderCode,2>&,uint32_t)>;
    void set_clear(Clear clear){clear_=std::move(clear);}
    std::vector<uint32_t> take_interrupts(){auto result=std::move(interrupts_);interrupts_.clear();return result;}
    bool suspended() const{return suspended_;}
    bool waiting_display() const{return display_wait_;}
    void enable_display_wait(bool enable){display_wait_enabled_=enable;}
    void resume(){if(!suspended_)throw std::runtime_error("GPU ring is not suspended");submit(write_index_);}
    CommandRing(Read read,Write write,Log log):read_(read),write_(write),log_(log){}
    void initialize(uint32_t physical,uint32_t encoded_size) {
        if(ready_ && (executing_ || suspended_ || read_index_!=write_index_ || !interrupts_.empty() || !indirect_progress_.empty() || !first_error_.empty()))
            throw std::runtime_error("GPU ring reinitialization requires a drained healthy queue");
        if(encoded_size>23 || physical%4)throw std::runtime_error("Invalid GPU ring parameters");
        const uint32_t bytes=uint32_t(1)<<(encoded_size+3);
        if(bytes<8 || physical>=0x20000000 || bytes>0x20000000-physical)
            throw std::runtime_error("GPU ring outside physical memory");
        // Rebinding a drained command queue resets its pointers/writeback,
        // while Xenos register and shader state belong to the GPU and persist.
        if(ready_)log_("GPU_RING_REBIND previous_queue=DRAINED register_state=PRESERVED");
        origin_=physical;words_=bytes/4;read_index_=write_index_=0;writeback_enabled_=false;ready_=true;
        char line[160];std::snprintf(line,sizeof(line),"GPU_RING_INIT physical=%08x encoded_size=%u bytes=%u words=%u",physical,encoded_size,bytes,words_);log_(line);
    }
    void enable_writeback(uint32_t physical,uint32_t encoded_block) {
        if(!ready_ || physical%4 || physical>0x1ffffffc || encoded_block>19)
            throw std::runtime_error("Invalid GPU ring writeback parameters");
        write_(physical,__builtin_bswap32(read_index_));
        writeback_=physical;writeback_enabled_=true;writeback_block_=encoded_block;
    }
    uint32_t read_index() const{return read_index_;}
    uint32_t byte_size() const{return words_*4;}
    uint64_t packets() const{return packets_;}
    const std::string& first_error() const{return first_error_;}
    const ShaderCode& shader(unsigned stage) const{return shaders_.at(stage);}
    uint32_t read_register(uint32_t index) const {
        if(index>=registers_.size())throw std::runtime_error("GPU register index outside register file");
        if(index==0x1c4)return read_index_;
        if(index==0x1c5)return write_index_;
        return registers_[index];
    }
    void write_register(uint32_t index,uint32_t value) {
        if(index>=registers_.size())throw std::runtime_error("GPU register index outside register file");
        if(index==0x1c5){if(suspended_)throw std::runtime_error("New GPU submission during interrupt suspension");submit(value);return;}
        registers_[index]=value;
        if(index>=0x578 && index<=0x57f && (registers_[0x1dc]&(1u<<(index-0x578)))) {
            const uint64_t address=uint64_t(registers_[0x1dd])+4*(index-0x578);
            if(address>0x1ffffffc)throw std::runtime_error("GPU scratch writeback outside physical memory");
            write_(uint32_t(address),__builtin_bswap32(value));
        }
    }
    void submit(uint32_t index) {
        if(!ready_ || index>=words_)throw std::runtime_error("Invalid GPU write pointer");
        if(!first_error_.empty())throw std::runtime_error(first_error_);
        if(executing_)throw std::runtime_error("Recursive GPU ring submission from a packet");
        executing_=true;
        suspended_=false;
        display_wait_=false;
        write_index_=index;
        const uint32_t mask=words_-1;
        try {
            uint32_t budget=1u<<20;
            while(read_index_!=write_index_) {
                const uint32_t available=(write_index_-read_index_)&mask;
                auto fetch=[&](uint32_t word){return __builtin_bswap32(read_(origin_+((read_index_+word)&mask)*4));};
                const uint32_t count=execute_packet(fetch,available,origin_+read_index_*4,0,budget);
                read_index_=(read_index_+count)&mask;
                if(writeback_enabled_)write_(writeback_,__builtin_bswap32(read_index_));
            }
            executing_=false;
        }catch(const InterruptYield&) {
            executing_=false;suspended_=true;log_("GPU_RING_SUSPEND reason=PENDING_INTERRUPT_BEFORE_WAIT");
        }catch(const DisplayYield&) {
            executing_=false;suspended_=true;display_wait_=true;log_("GPU_RING_SUSPEND reason=WAIT_EXTERNAL_DISPLAY_EVENT");
        }catch(const std::exception& error) {
            executing_=false;
            char line[384];std::snprintf(line,sizeof(line),"GPU_RING_STOP physical=%08x read=%u write=%u packets=%llu reason=%s",
                origin_,read_index_,write_index_,(unsigned long long)packets_,error.what());
            first_error_=line;log_(line);throw std::runtime_error(first_error_);
        }
    }
private:
    Read read_;Write write_;Log log_;Present present_;Clear clear_;
    std::array<uint32_t,0x5003> registers_{};
    std::array<ShaderCode,2> shaders_;
    std::vector<uint32_t> micro_engine_initialization_;
    uint32_t origin_=0,words_=0,read_index_=0,write_index_=0,writeback_=0,writeback_block_=0;
    bool ready_=false,writeback_enabled_=false,executing_=false;
    uint64_t packets_=0;
    uint64_t bin_mask_=0,bin_select_=0;
    std::vector<uint32_t> interrupts_;
    struct InterruptYield{};
    struct DisplayYield{};
    bool display_wait_enabled_=false,display_wait_=false;
    bool suspended_=false;
    std::map<uint64_t,uint32_t> indirect_progress_;
    std::string first_error_;
    static uint32_t gpu_swap(uint32_t value,uint32_t mode) {
        switch(mode&3) {
            case 0:return value;
            case 1:return ((value&0x00ff00ff)<<8)|((value&0xff00ff00)>>8);
            case 2:return __builtin_bswap32(value);
            default:return (value<<16)|(value>>16);
        }
    }
    static bool compare(uint32_t function,uint32_t value,uint32_t reference) {
        switch(function&7) {
            case 0:return false;case 1:return value<reference;case 2:return value<=reference;
            case 3:return value==reference;case 4:return value!=reference;case 5:return value>=reference;
            case 6:return value>reference;default:return true;
        }
    }
    bool local_shader_only(unsigned stage) const {
        // Conservative proof for the driver's startup warm-up draw: only
        // straight-line ALU, no fetches, branches or memory-export registers.
        const auto& code=shaders_[stage].words;
        if(code.empty() || code.size()%3)return false;
        const uint32_t blocks=uint32_t(code.size()/3);uint32_t limit=blocks;
        bool ended=false,executed=false;
        for(uint32_t block=0;block<limit;++block) {
            const uint32_t a=code[block*3],b=code[block*3+1],c=code[block*3+2];
            for(uint64_t cf:{uint64_t(a)|(uint64_t(b&0xffff)<<32),uint64_t(b>>16)|(uint64_t(c)<<16)}) {
                const uint32_t op=uint32_t(cf>>44);
                if(op==0 || op==12)continue;
                if(op!=1 && op!=2)return false;
                const uint32_t first=cf&0xfff,count=(cf>>12)&7;
                if(first<=block || count>6 || first>=blocks || count>blocks-first)return false;
                limit=std::min(limit,first);ended|=op==2;
                const uint32_t sequence=(cf>>16)&0xfff;
                for(uint32_t i=0;i<count;++i) {
                    if(sequence&(1u<<(i*2)))return false;
                    const uint32_t instruction=code[(first+i)*3],destination=instruction&63;
                    if((instruction&0x8000) && ((stage==0 && destination>=16 && destination!=62 && destination!=63) ||
                        (stage==1 && destination>3 && destination!=61)))return false;
                    executed=true;
                }
            }
        }
        return ended && executed;
    }
    void log_shader(uint32_t stage,bool immediate) {
        const auto& shader=shaders_[stage];const auto count=uint32_t(shader.words.size());
        char info[192];std::snprintf(info,sizeof(info),"GPU_SHADER_LOAD stage=%u physical=%08x dwords=%u immediate=%u",stage,shader.address,count,immediate);log_(info);
        for(uint32_t first=0;first<count;first+=8) {
            std::string line="GPU_SHADER_WORDS stage="+std::to_string(stage)+" offset="+std::to_string(first);
            for(uint32_t i=first;i<std::min(first+8,count);++i){char value[12];std::snprintf(value,sizeof(value)," %08x",shader.words[i]);line+=value;}
            log_(line.c_str());
        }
    }
    void register_range(uint32_t first,uint32_t count) const {
        if(first>=registers_.size() || count>registers_.size()-first)
            throw std::runtime_error("PM4 register range overflow");
    }
    uint32_t execute_packet(const std::function<uint32_t(uint32_t)>& fetch,uint32_t available,uint32_t address,unsigned depth,uint32_t& budget) {
        if(!available || !budget--)throw std::runtime_error("PM4 execution budget exhausted");
        const uint32_t header=fetch(0),type=header>>30;
        const uint32_t payload=header==0 || type==2 ? 0 : type==1 ? 2 : ((header>>16)&0x3fff)+1;
        if(payload>=available)throw std::runtime_error("PM4 submission ends in a partial packet");
        char trace[192];std::snprintf(trace,sizeof(trace),"GPU_PACKET physical=%08x header=%08x payload=%u depth=%u",address,header,payload,depth);log_(trace);
        if(header==0 || type==2){++packets_;return 1;}
        if(type==0) {
            const uint32_t first=header&0x7fff;
            register_range(first,(header&0x8000)?1:payload);
            for(uint32_t i=0;i<payload;++i)write_register(first+((header&0x8000)?0:i),fetch(i+1));
        }else if(type==1) {
            const uint32_t first=header&0x7ff,second=(header>>11)&0x7ff;
            write_register(first,fetch(1));write_register(second,fetch(2));
        }else {
            if((header&1) && !(bin_select_&bin_mask_)){++packets_;return payload+1;}
            const uint32_t opcode=(header>>8)&0x7f;
            auto exact=[&](uint32_t expected){if(payload!=expected)throw std::runtime_error("Unexpected PM4 packet payload size");};
            switch(opcode) {
                case 0x50:case 0x51:{
                    exact(2);const uint64_t value=(uint64_t(fetch(1))<<32)|fetch(2);
                    (opcode==0x50?bin_mask_:bin_select_)=value;break;
                }
                case 0x60:case 0x61:case 0x62:case 0x63:{
                    exact(1);auto& value=opcode<0x62?bin_mask_:bin_select_;
                    value=(opcode&1)?((value&0xffffffffull)|(uint64_t(fetch(1))<<32)):((value&0xffffffff00000000ull)|fetch(1));break;
                }
                case 0x10:break; // NOP payload is intentionally ignored.
                case 0x48: // Native backend replaces the hardware micro-engine.
                    micro_engine_initialization_.clear();
                    for(uint32_t i=1;i<=payload;++i)micro_engine_initialization_.push_back(fetch(i));
                    break;
                case 0x21:{
                    exact(3);const auto info=fetch(1),mask=fetch(2),bits=fetch(3);
                    write_register(info&0x1fff,(read_register(info&0x1fff)&((info>>31)?read_register(mask&0x1fff):mask))|
                        ((info&0x40000000)?read_register(bits&0x1fff):bits));break;
                }
                case 0x3d:{
                    const uint32_t destination=fetch(1);
                    if((uint64_t(destination&~3u)+(payload-1)*4)>0x20000000)throw std::runtime_error("PM4 MEM_WRITE address overflow");
                    for(uint32_t i=2;i<=payload;++i)write_((destination&~3u)+(i-2)*4,gpu_swap(fetch(i),destination&3));break;
                }
                case 0x3e:{exact(2);const uint32_t destination=fetch(2);
                    write_(destination&~3u,gpu_swap(read_register(fetch(1)),destination&3));break;}
                case 0x2d:case 0x55:case 0x56:{
                    const auto selector=fetch(1);uint32_t first;
                    if(opcode==0x2d){
                        static constexpr uint32_t bases[]={0x4000,0x4800,0x4900,0x4908,0x2000};
                        const uint32_t kind=(selector>>16)&255;
                        if(kind>=5)throw std::runtime_error("Unsupported PM4 constant bank");
                        first=bases[kind]+(selector&0x7ff);
                    }else first=selector&0xffff;
                    register_range(first,payload-1);
                    for(uint32_t i=2;i<=payload;++i)write_register(first+i-2,fetch(i));break;
                }
                case 0x2f:{
                    exact(3);const auto source=fetch(1)&0x3fffffff,selector=fetch(2),count=fetch(3)&0xfff;
                    static constexpr uint32_t bases[]={0x4000,0x4800,0x4900,0x4908,0x2000};
                    const auto kind=(selector>>16)&255,index=selector&0x7ff;
                    if(kind>=5 || source%4 || source>=0x20000000 || uint64_t(source)+uint64_t(count)*4>0x20000000)
                        throw std::runtime_error("Invalid PM4 memory constant load");
                    const auto first=bases[kind]+index;register_range(first,count);
                    // Validate/read the entire source before applying register
                    // effects: an invalid range must not leave partial state.
                    std::vector<uint32_t> values;values.reserve(count);
                    for(uint32_t i=0;i<count;++i)values.push_back(__builtin_bswap32(read_(source+i*4)));
                    for(uint32_t i=0;i<count;++i)write_register(first+i,values[i]);
                    char line[192];std::snprintf(line,sizeof(line),"GPU_CONSTANT_MEMORY_LOAD source=%08x first_register=%04x dwords=%u bank=%u",source,first,count,kind);log_(line);break;
                }
                case 0x37:case 0x3f:{
                    exact(2);const uint32_t pointer=fetch(1)&0x1fffffff,length=fetch(2);
                    if(depth>=8 || pointer%4 || length>0xfffff || uint64_t(pointer)+uint64_t(length)*4>0x20000000)
                        throw std::runtime_error("Invalid PM4 indirect buffer/depth");
                    const uint64_t key=(uint64_t(depth)<<32)|address;
                    auto& consumed=indirect_progress_[key];
                    while(consumed<length) {
                        auto indirect=[&](uint32_t i){return __builtin_bswap32(read_(pointer+(consumed+i)*4));};
                        consumed+=execute_packet(indirect,length-consumed,pointer+consumed*4,depth+1,budget);
                    }indirect_progress_.erase(key);break;
                }
                case 0x3c:{
                    exact(5);const uint32_t mode=fetch(1),pointer=fetch(2),reference=fetch(3),mask=fetch(4);
                    const uint32_t value=(mode&0x10)?gpu_swap(read_(pointer&~3u),pointer&3):read_register(pointer);
                    if(!compare(mode,value&mask,reference)) {
                        if(!interrupts_.empty())throw InterruptYield{};
                        char wait[192];std::snprintf(wait,sizeof(wait),"PM4 WAIT_REG_MEM blocked mode=%08x pointer=%08x observed=%08x reference=%08x mask=%08x; asynchronous resumption required",mode,pointer,value,reference,mask);
                        if(display_wait_enabled_){log_(wait);throw DisplayYield{};}
                        throw std::runtime_error(wait);
                    }
                    break;
                }
                case 0x45:{
                    exact(6);const uint32_t mode=fetch(1),pointer=fetch(2),reference=fetch(3),mask=fetch(4),destination=fetch(5),data=fetch(6);
                    const uint32_t observed=(mode&0x10)?gpu_swap(read_(pointer&~3u),pointer&3):read_register(pointer);
                    if(compare(mode,observed&mask,reference)) {
                        if(mode&0x100)write_(destination&~3u,gpu_swap(data,destination&3));else write_register(destination,data);
                    }break;
                }
                case 0x27:{
                    exact(2);const uint32_t source=fetch(1),layout=fetch(2),stage=source&3,count=layout&0xffff;
                    if(stage>1 || (layout>>16) || !count || uint64_t(source&~3u)+uint64_t(count)*4>0x20000000)
                        throw std::runtime_error("Invalid PM4 shader load");
                    ShaderCode pending;pending.address=source&~3u;pending.words.reserve(count);
                    for(uint32_t i=0;i<count;++i)pending.words.push_back(__builtin_bswap32(read_(pending.address+i*4)));
                    shaders_[stage]=std::move(pending);log_shader(stage,false);break;
                }
                case 0x2b:{
                    if(payload<3)throw std::runtime_error("Truncated PM4 immediate shader header");
                    const uint32_t stage=fetch(1),layout=fetch(2),count=layout&0xffff;
                    if(stage>1 || (layout>>16) || !count || count!=payload-2)
                        throw std::runtime_error("Invalid PM4 immediate shader size/stage");
                    ShaderCode pending;pending.address=address+12;pending.words.reserve(count);
                    for(uint32_t i=0;i<count;++i)pending.words.push_back(fetch(i+3));
                    shaders_[stage]=std::move(pending);
                    log_shader(stage,true);
                    break;
                }
                case 0x3b:exact(1);break; // No derived GPU cache exists yet.
                case 0x46:exact(1);write_register(0x21f9,fetch(1)&63);break;
                case 0x58:{
                    exact(3);const uint32_t initiator=fetch(1),destination=fetch(2),value=fetch(3);
                    if(initiator&0x80000000)throw std::runtime_error("GPU event counter write requires video clock implementation");
                    // All preceding accepted operations currently complete on
                    // the CPU. Any real raster draw stops before retirement.
                    write_register(0x21f9,initiator&63);
                    write_(destination&~3u,gpu_swap(value,destination&3));break;
                }
                case 0x22:case 0x36:{
                    if(opcode==0x36 && payload==1 && registers_[0x2208]==4 &&
                       !(registers_[0x2000]&0x3fff) && !registers_[0x2104] && !registers_[0x2200] &&
                       !registers_[0x2324] && local_shader_only(0) && local_shader_only(1)) {
                        log_("GPU_DRAW_SKIPPED reason=NO_RASTER_NO_MEMORY_EXPORT (verified shader/register state)");break;
                    }
                    if(((opcode==0x36 && payload==1) || (opcode==0x22 && payload==2 && fetch(1)==0)) &&
                       clear_ && clear_(registers_,shaders_,fetch(payload)))break;
                    for(uint32_t i=1;i<=payload;++i){char line[96];std::snprintf(line,sizeof(line),"GPU_DRAW_WORD index=%u value=%08x",i-1,fetch(i));log_(line);}
                    for(uint32_t index=0;index<registers_.size();++index)if(registers_[index]) {
                        char line[96];std::snprintf(line,sizeof(line),"GPU_DRAW_REGISTER index=%04x value=%08x",index,registers_[index]);log_(line);
                    }
                    for(uint32_t slot=0;slot<96;++slot) {
                        const uint32_t descriptor=registers_[0x4800+slot*2],format=registers_[0x4801+slot*2];
                        if(slot%3==0 && (descriptor&3)==2) {
                            char line[192];std::snprintf(line,sizeof(line),"GPU_DRAW_TEXTURE_FETCH slot=%u words=%08x,%08x,%08x,%08x,%08x,%08x",slot/3,
                                descriptor,format,registers_[0x4802+slot*2],registers_[0x4803+slot*2],registers_[0x4804+slot*2],registers_[0x4805+slot*2]);log_(line);
                            slot+=2;continue; // One texture occupies three two-word vertex descriptor slots.
                        }
                        if((descriptor&3)!=3)continue;
                        const uint32_t base=descriptor&~3u,size=(format>>2)&0xffffff;
                        char line[192];std::snprintf(line,sizeof(line),"GPU_DRAW_VERTEX_BUFFER slot=%u physical=%08x dwords=%u endian=%u",slot,base,size,format&3);log_(line);
                        try {
                            for(uint32_t word=0;word<std::min(size,128u);++word) {
                                const uint32_t value=gpu_swap(read_(base+word*4),format&3);
                                std::snprintf(line,sizeof(line),"GPU_DRAW_VERTEX_WORD slot=%u word=%u value=%08x",slot,word,value);log_(line);
                            }
                        }catch(const std::exception& error){std::snprintf(line,sizeof(line),"GPU_DRAW_VERTEX_CAPTURE_FAILED slot=%u reason=%s",slot,error.what());log_(line);}
                    }
                    throw std::runtime_error("PM4 draw requires Vulkan pipeline/resource binding");
                }
                case 0x54:{
                    exact(1);const uint32_t mask=fetch(1);
                    if(mask&~63u || interrupts_.size()>4090)throw std::runtime_error("Invalid/excessive PM4 interrupt mask");
                    for(uint32_t cpu=0;cpu<6;++cpu)if(mask&(1u<<cpu))interrupts_.push_back(cpu);
                    char line[96];std::snprintf(line,sizeof(line),"GPU_INTERRUPT_PENDING cpu_mask=%02x",mask);log_(line);break;
                }
                case 0x64:{
                    // Private HLE packet emitted only by this runtime's VdSwap.
                    exact(4);
                    if((header&1) || fetch(1)!=0x41495258 || !present_)throw std::runtime_error("Invalid/unavailable Airborne display packet");
                    std::array<uint32_t,6> texture{};for(unsigned i=0;i<6;++i)texture[i]=registers_[0x4800+i];
                    if(fetch(2)!=(texture[1]&0xfffff000) || fetch(3)!=(texture[2]&8191)+1 || fetch(4)!=((texture[2]>>13)&8191)+1)
                        throw std::runtime_error("Display packet differs from texture descriptor");
                    present_(fetch(2),texture);break;
                }
                default:{char error[96];std::snprintf(error,sizeof(error),"Unsupported PM4 opcode=%02x",opcode);throw std::runtime_error(error);}
            }
        }
        ++packets_;return payload+1;
    }
};
}
