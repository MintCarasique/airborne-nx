#include "../../runtime/gpu/command_ring.h"
#include "../../runtime/gpu/guest_mmio.h"
#include "../../runtime/gpu/masked_point.h"
#include <cstring>
#include <iostream>
#include <memory>
struct PPCContext{};
static unsigned mmio_bytes=0;static uint32_t mmio_address=0;static uint64_t mmio_value=0;
extern "C" uint64_t airborne_gpu_mmio_read(PPCContext&,uint32_t address,unsigned bytes){mmio_address=address;mmio_bytes=bytes;return 0x11223344;}
extern "C" void airborne_gpu_mmio_write(PPCContext&,uint32_t address,uint64_t value,unsigned bytes){mmio_address=address;mmio_value=value;mmio_bytes=bytes;}

int main() {
    size_t checks=0;
    auto check=[&](bool condition){++checks;if(!condition)throw std::runtime_error("Ring contract check "+std::to_string(checks));};
    std::vector<uint8_t> memory(0x4000);
    auto read=[&](uint32_t at){if(at%4 || at>memory.size()-4)throw std::runtime_error("Physical read bounds");uint32_t value;std::memcpy(&value,memory.data()+at,4);return value;};
    auto write=[&](uint32_t at,uint32_t value){if(at%4 || at>memory.size()-4)throw std::runtime_error("Physical write bounds");std::memcpy(memory.data()+at,&value,4);};
    auto put=[&](uint32_t at,uint32_t value){for(unsigned i=0;i<4;++i)memory[at+i]=uint8_t(value>>(24-i*8));};
    auto be=[&](uint32_t at){return uint32_t(memory[at])<<24|uint32_t(memory[at+1])<<16|uint32_t(memory[at+2])<<8|memory[at+3];};
    auto quiet=[](const char*){};
    airborne::CommandRing ring(read,write,quiet);ring.initialize(0x100,2);ring.enable_writeback(0x80,6);
    check(ring.byte_size()==32 && be(0x80)==0);
    for(uint32_t i=0;i<5;++i)put(0x100+i*4,0x80000000);
    ring.submit(5);check(ring.read_index()==5 && be(0x80)==5);
    // One register packet straddles the physical end of the primary ring.
    put(0x114,0x00022000);put(0x118,11);put(0x11c,22);put(0x100,33);
    ring.submit(1);check(ring.read_index()==1 && ring.read_register(0x2000)==11 && ring.read_register(0x2002)==33 && be(0x80)==1);
    put(0x104,0xc0002200);put(0x108,0);
    bool failed=false;try{ring.submit(3);}catch(const std::runtime_error&){failed=true;}
    check(failed && ring.read_index()==1 && be(0x80)==1 && ring.first_error().find("draw requires")!=std::string::npos);
    const auto first=ring.first_error();try{ring.submit(3);}catch(...){}
    check(ring.first_error()==first && ring.read_index()==1);

    // GPU memory-endian controls have independent expected byte vectors.
    const uint8_t expected[][4]={{0x44,0x33,0x22,0x11},{0x33,0x44,0x11,0x22},{0x11,0x22,0x33,0x44},{0x22,0x11,0x44,0x33}};
    for(uint32_t mode=0;mode<4;++mode) {
        airborne::CommandRing q(read,write,quiet);q.initialize(0x100,4);
        put(0x100,0xc0013d00);put(0x104,0x400|mode);put(0x108,0x11223344);q.submit(3);
        check(!std::memcmp(memory.data()+0x400,expected[mode],4));
    }
    airborne::CommandRing q(read,write,quiet);q.initialize(0x100,4);
    put(0x100,0xc0013f00);put(0x104,0x600);put(0x108,2);
    put(0x600,0x1001);put(0x604,0xaabbccdd);q.submit(3);
    check(q.read_register(0x1001)==0xaabbccdd && q.read_index()==3 && q.packets()==2);
    put(0x10c,0xc0022100);put(0x110,0x1001);put(0x114,0xffff);put(0x118,0x80000000);q.submit(7);
    check(q.read_register(0x1001)==0x8000ccdd);
    // Constant bank and scratch writeback update the values the game polls.
    put(0x11c,0xc0012d00);put(0x120,0x00010002);put(0x124,0x1234);q.submit(10);
    check(q.read_register(0x4802)==0x1234);
    q.write_register(0x1dd,0x700);q.write_register(0x1dc,1);q.write_register(0x578,0x5a12bc34);
    check(be(0x700)==0x5a12bc34);
    // A self-referencing indirect list must terminate without retiring it.
    airborne::CommandRing cycle(read,write,quiet);cycle.initialize(0x100,4);
    put(0x100,0xc0013f00);put(0x104,0x600);put(0x108,3);
    put(0x600,0xc0013f00);put(0x604,0x600);put(0x608,3);
    failed=false;try{cycle.submit(3);}catch(const std::runtime_error&){failed=true;}
    check(failed && cycle.read_index()==0 && cycle.first_error().find("depth")!=std::string::npos);
    airborne::CommandRing partial(read,write,quiet);partial.initialize(0x100,4);
    put(0x100,0xc0013d00);failed=false;try{partial.submit(2);}catch(...){failed=true;}
    check(failed && partial.read_index()==0);
    airborne::CommandRing wait(read,write,quiet);wait.initialize(0x100,4);
    put(0x100,0xc0043c00);put(0x104,3);put(0x108,0x2000);put(0x10c,1);put(0x110,0xffffffff);put(0x114,0);
    failed=false;try{wait.submit(6);}catch(...){failed=true;}
    check(failed && wait.read_index()==0 && wait.first_error().find("blocked")!=std::string::npos);
    // Synthetic ALU-only shaders: CF EXEC_END at block 1, one ALU
    // instruction. No proprietary shader words are embedded in this test.
    auto warmup=[&](uint32_t destination,bool fetch_instruction,bool visible) {
        airborne::CommandRing warm(read,write,quiet);warm.initialize(0x100,6);
        uint32_t cursor=0x100;
        auto emit=[&](uint32_t word){put(cursor,word);cursor+=4;};
        for(uint32_t stage=0;stage<2;++stage) {
            emit(0xc0072b00);emit(stage);emit(6);
            emit(0x1001|(fetch_instruction?0x10000:0));emit(0x2000);emit(0);
            emit(0x8000|destination);emit(0);emit(0);
        }
        warm.write_register(0x2208,4);warm.write_register(0x2104,visible?15:0);
        emit(0xc0003600);emit(0);
        bool rejected=false;try{warm.submit((cursor-0x100)/4);}catch(...){rejected=true;}
        check(rejected==(destination==32 || fetch_instruction || visible));
        check(warm.shader(0).words.size()==6 && warm.shader(1).words.size()==6);
    };
    warmup(0,false,false);warmup(32,false,false);warmup(0,true,false);warmup(0,false,true);
    airborne::CommandRing signal(read,write,quiet);signal.initialize(0x100,4);
    put(0x100,0xc0025800);put(0x104,7);put(0x108,0x802);put(0x10c,0x12345678);
    signal.submit(4);check(be(0x800)==0x12345678 && signal.read_register(0x21f9)==7);
    airborne::CommandRing counter(read,write,quiet);counter.initialize(0x100,4);
    put(0x104,0x80000007);put(0x10c,0xffffffff);
    failed=false;try{counter.submit(4);}catch(...){failed=true;}
    check(failed && counter.read_index()==0 && be(0x800)==0x12345678);
    // Display callback must execute after earlier memory writes and must not
    // retire the containing primary packet if native presentation fails.
    for(bool native_failure:{false,true}) {
        airborne::CommandRing display(read,write,quiet);display.initialize(0x100,6);
        unsigned calls=0;
        display.set_present([&](uint32_t address,const std::array<uint32_t,6>& texture){
            ++calls;check(address==0x1000 && texture[0]==2 && be(0x900)==0xabcdef01);
            if(native_failure)throw std::runtime_error("Native present failure");
        });
        uint32_t cursor=0x100;auto emit=[&](uint32_t word){put(cursor,word);cursor+=4;};
        emit(0xc0013d00);emit(0x902);emit(0xabcdef01);
        emit(0x00054800);for(uint32_t word:{2u,0x1006u,0x0027e03fu,0u,0u,0x200u})emit(word); // 64x320
        emit(0xc0036400);emit(0x41495258);emit(0x1000);emit(64);emit(320);
        failed=false;try{display.submit((cursor-0x100)/4);}catch(...){failed=true;}
        check(calls==1 && failed==native_failure && display.read_index()==(native_failure?10u:15u));
    }
    airborne::CommandRing predicated(read,write,quiet);predicated.initialize(0x100,5);
    put(0x100,0xc0003601);put(0x104,0); // default bin mask/select: draw is disabled
    predicated.submit(2);check(predicated.read_index()==2);
    put(0x108,0xc0006000);put(0x10c,1);put(0x110,0xc0006200);put(0x114,1);
    put(0x118,0xc0003601);put(0x11c,0);failed=false;
    try{predicated.submit(8);}catch(...){failed=true;}
    check(failed && predicated.read_index()==6 && predicated.first_error().find("draw requires")!=std::string::npos);
    unsigned effect_count=0;
    auto counted_write=[&](uint32_t at,uint32_t value){if(at==0x800)++effect_count;write(at,value);};
    airborne::CommandRing yielding(read,counted_write,quiet);yielding.initialize(0x100,4);
    put(0x100,0xc0013f00);put(0x104,0x600);put(0x108,11);
    put(0x600,0xc0013d00);put(0x604,0x802);put(0x608,0x12345678);
    put(0x60c,0xc0005400);put(0x610,4);
    put(0x614,0xc0043c00);put(0x618,0x13);put(0x61c,0x902);put(0x620,0);put(0x624,0xffffffff);put(0x628,0);
    put(0x900,1);yielding.submit(3);
    check(yielding.suspended() && yielding.read_index()==0 && yielding.first_error().empty() && effect_count==1);
    check(yielding.take_interrupts()==std::vector<uint32_t>{2});
    put(0x900,0);yielding.resume();
    check(!yielding.suspended() && yielding.read_index()==3 && effect_count==1 && yielding.take_interrupts().empty());
    // Conditional writes preserve the destination on a failed comparison.
    airborne::CommandRing conditional(read,write,quiet);conditional.initialize(0x100,4);
    put(0x900,0x11223344);put(0x100,0xc0054500);put(0x104,0x113);put(0x108,0x902);
    put(0x10c,0x11223344);put(0x110,0xffffffff);put(0x114,0xa02);put(0x118,0x55667788);
    conditional.submit(7);check(be(0xa00)==0x55667788);
    put(0x11c,0xc0054500);put(0x120,0x113);put(0x124,0x902);put(0x128,0xdeadbeef);
    put(0x12c,0xffffffff);put(0x130,0xa02);put(0x134,0);conditional.submit(14);check(be(0xa00)==0x55667788);
    conditional.write_register(0x5002,123);check(conditional.read_register(0x5002)==123);
    airborne::CommandRing display_wait(read,write,quiet);display_wait.initialize(0x100,4);display_wait.enable_display_wait(true);
    put(0x100,0xc0043c00);put(0x104,3);put(0x108,0x1951);put(0x10c,1);put(0x110,1);put(0x114,0);
    display_wait.submit(6);check(display_wait.suspended() && display_wait.waiting_display() && display_wait.read_index()==0 && display_wait.first_error().empty());
    bool busy_rebind=false;try{display_wait.initialize(0x200,4);}catch(const std::runtime_error&){busy_rebind=true;}
    check(busy_rebind && display_wait.suspended() && display_wait.read_index()==0);
    display_wait.write_register(0x1951,1);display_wait.resume();
    check(!display_wait.suspended() && !display_wait.waiting_display() && display_wait.read_index()==6);
    display_wait.initialize(0x200,4);
    check(display_wait.read_index()==0 && display_wait.read_register(0x1c5)==0 && display_wait.read_register(0x1951)==1);
    put(0x200,0x80000000);display_wait.submit(1);check(display_wait.read_index()==1);
    unsigned false_vertex_reads=0;bool texture_logged=false;
    auto inspect_read=[&](uint32_t at){if(at==0x59e4fc)++false_vertex_reads;return read(at);};
    auto inspect_log=[&](const char* line){if(std::strstr(line,"GPU_DRAW_TEXTURE_FETCH slot=0"))texture_logged=true;};
    auto texture_draw=std::make_unique<airborne::CommandRing>(inspect_read,write,inspect_log);texture_draw->initialize(0x100,4);
    texture_draw->write_register(0x2000,1280);texture_draw->write_register(0x2208,4);
    texture_draw->write_register(0x4800,0x8a000002);texture_draw->write_register(0x4801,0x1f658006);
    texture_draw->write_register(0x4802,0x0059e4ff);texture_draw->write_register(0x4803,0x1414);
    put(0x100,0xc0003600);put(0x104,0x10081);
    bool unsupported_draw=false;try{texture_draw->submit(2);}catch(const std::runtime_error& e){unsupported_draw=std::strstr(e.what(),"pipeline/resource binding")!=nullptr;}
    check(unsupported_draw && texture_logged && !false_vertex_reads && texture_draw->read_index()==0);
    auto point_state=std::make_unique<std::array<uint32_t,0x5003>>();
    const std::pair<uint32_t,uint32_t> point_profile[]={{0x2208,4},{0x2202,0x87000007},{0x2204,0x10000},{0x2205,0x10000},
        {0x2206,0x300},{0x2280,0x80008},{0x2281,0x4000010},{0x2300,0x400},{0x2302,4},{0x2000,0x14000500},
        {0x200f,0x20002000},{0x2082,0x100010},{0x2324,2}};
    for(auto pair:point_profile)(*point_state)[pair.first]=pair.second;
    check(airborne::masked_point_state(*point_state,0x10081));
    check(!airborne::masked_point_state(*point_state,0x20081)); // Cannot turn one vertex into two.
    for(auto index:{0x2104,0x2200,0x2080,0x2081,0x200e}) {
        (*point_state)[index]=1;check(!airborne::masked_point_state(*point_state,0x10081));(*point_state)[index]=0;
    }
    for(auto pair:point_profile) {
        (*point_state)[pair.first]=pair.second^1;check(!airborne::masked_point_state(*point_state,0x10081));(*point_state)[pair.first]=pair.second;
    }
    const uint32_t banks[]={0x4000,0x4800,0x4900,0x4908,0x2000};
    for(unsigned bank=0;bank<5;++bank) {
        auto constants=std::make_unique<airborne::CommandRing>(read,write,quiet);constants->initialize(0x100,4);
        put(0x300,0x11223344);put(0x304,0xffeeddcc);
        put(0x100,0xc0022f00);put(0x104,0x80000300);put(0x108,(bank<<16)|2);put(0x10c,2);
        constants->submit(4);check(constants->read_index()==4 && constants->read_register(banks[bank]+2)==0x11223344 && constants->read_register(banks[bank]+3)==0xffeeddcc);
    }
    auto bad_constants=std::make_unique<airborne::CommandRing>(read,write,quiet);bad_constants->initialize(0x100,4);
    bad_constants->write_register(0x4000,0xfacefeed);put(0x3ffc,0x12345678);
    put(0x100,0xc0022f00);put(0x104,0x3ffc);put(0x108,0);put(0x10c,2);
    bool constant_fault=false;try{bad_constants->submit(4);}catch(const std::runtime_error&){constant_fault=true;}
    check(constant_fault && bad_constants->read_index()==0 && bad_constants->read_register(0x4000)==0xfacefeed);
    std::string pointer_trace;
    auto pointer_log=[&](const char* line){pointer_trace+=line;pointer_trace+='\n';};
    auto pointer_shader=std::make_unique<airborne::CommandRing>(read,write,pointer_log);pointer_shader->initialize(0x100,4);
    put(0x300,0x11223344);put(0x304,0xffeeddcc);put(0x308,0x01020304);
    put(0x100,0xc0012700);put(0x104,0x300);put(0x108,3);pointer_shader->submit(3);
    check(pointer_shader->shader(0).words==std::vector<uint32_t>({0x11223344,0xffeeddcc,0x01020304}) &&
        pointer_trace.find("immediate=0")!=std::string::npos && pointer_trace.find("GPU_SHADER_WORDS stage=0 offset=0 11223344 ffeeddcc 01020304")!=std::string::npos);
    bool auto_draw_called=false;pointer_shader->set_clear([&](const auto&,const auto&,uint32_t initiator){auto_draw_called=initiator==0x60084;return auto_draw_called;});
    put(0x10c,0xc0012200);put(0x110,0);put(0x114,0x60084);pointer_shader->submit(6);
    check(auto_draw_called && pointer_shader->read_index()==6);
    PPCContext ctx;uint8_t* base=memory.data();
    PPC_STORE_U32(0x20,0x11223344);check(be(0x20)==0x11223344 && PPC_LOAD_U32(0x20)==0x11223344);
    check(PPC_LOAD_U32(0x7fc80710)==0x11223344 && mmio_address==0x7fc80710 && mmio_bytes==4);
    PPC_STORE_U32(0x7fc80714,0x41);check(mmio_address==0x7fc80714 && mmio_value==0x41 && mmio_bytes==4);
    check(PPC_LOAD_U32(0x7fea1800)==0x11223344 && mmio_address==0x7fea1800 && mmio_bytes==4);
    std::cout<<checks<<" PM4/MMIO contract checks passed\n";
}
