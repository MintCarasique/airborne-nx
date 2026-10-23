// Shared diagnostic startup services. Platform APIs are supplied by the caller.
#include "../gpu/vulkan_device.h"
#include "../gpu/command_ring.h"
#include "../gpu/front_buffer.h"
#include "../host/guest_fp.h"
#include "startup_shader_profile.h"
#include "post_config_graphics.h"
#include "../gpu/masked_point.h"
#include "../gpu/video_draw.h"
#include "video_graphics.h"
#include <chrono>
#include <cstdarg>
#include <ctime>
#include <atomic>
#include "../audio/pcm_output.h"
#include "../io/game_files.h"
#include "../net/socket_runtime.h"
#ifndef STARTUP_PRINT
#define STARTUP_PRINT(...) std::printf(__VA_ARGS__)
#define STARTUP_ERROR(...) std::fprintf(stderr,__VA_ARGS__)
#define STARTUP_FLUSH() std::fflush(stdout)
#endif
namespace {
constexpr uint32_t stack_bottom=0x10010000, stack_size=0x40000;
constexpr uint32_t module_variable=0x70000000, module=0x70001000, header=0x70010000;
constexpr uint64_t table_address=PPC_IMAGE_BASE+PPC_IMAGE_SIZE;
thread_local PPCContext* active_context=nullptr;
uint8_t* active_base=nullptr;
GuestVirtualAllocator* allocator=nullptr;
GuestPhysicalAllocator* physical_allocator=nullptr;
std::unique_ptr<airborne::VulkanDevice> graphics_device;
std::unique_ptr<airborne::CommandRing> graphics_ring;
uint32_t graphics_interrupt_callback=0,graphics_interrupt_argument=0;
uint32_t graphics_system_identifier_address=0;
uint32_t graphics_system_generation=0;
struct KnownZeroDepth {bool valid=false;uint32_t tile=0,width=0,height=0,samples=0;} graphics_zero_depth;
struct MovieColor {uint32_t tile=0;std::vector<uint8_t> rgba;} graphics_movie_color;
uint32_t graphics_front_buffer=UINT32_MAX;
uint32_t xma_context_table=0;
std::bitset<320> xma_contexts;
std::map<uint32_t,uint32_t> title_terminate_notifications;
std::atomic<bool> imports_stopping{false};
std::atomic<uint64_t> tls_get_calls{0},tls_set_calls{0};
// Native recursive locks back guest critical sections; native events and worker
// threads implement the bounded startup profile on each supported platform.
std::map<uint32_t,CRITICAL_SECTION*> critical_sections;
// Counts acquired recursion levels and native acquisitions in flight. Guest
// memory may be reused, so its owner bytes alone cannot prove a mutex is idle.
std::map<uint32_t,uint32_t> critical_section_users;
constexpr uint32_t initial_thread=0x70020000;
constexpr uint32_t initial_pcr=0x70021000,initial_tls=0x70022000;
int initial_thread_priority=0;
#ifdef __SWITCH__
Handle initial_native_thread=INVALID_HANDLE;
#else
HANDLE initial_native_thread=nullptr;
#endif
void register_initial_native_thread() {
#ifdef __SWITCH__
    initial_native_thread=threadGetSelf()->handle;
#else
    if(!DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&initial_native_thread,THREAD_SET_INFORMATION|THREAD_QUERY_INFORMATION,FALSE,0))
        throw DiagnosticStop("Cannot retain initial native thread handle");
#endif
}
thread_local uint32_t current_thread_address=initial_thread,current_tls_address=initial_tls;
std::bitset<64> allocated_tls;
struct HostEvent { bool manual,signalled;std::condition_variable_any changed; };
std::map<uint32_t,std::shared_ptr<HostEvent>> events;
std::map<uint32_t,std::shared_ptr<HostEvent>> event_objects;
struct HostSemaphore { uint32_t count,limit;std::condition_variable_any changed; };
std::map<uint32_t,std::shared_ptr<HostSemaphore>> semaphore_objects;
std::map<uint32_t,std::shared_ptr<HostSemaphore>> semaphore_handles;
struct HostThread { HANDLE handle=nullptr;uint32_t id,object,pcr,tls,stack_low,stack_high,startup,entry,argument,references=0;int priority=0;bool closed=false,audio=false;std::string name; };
std::map<uint32_t,std::shared_ptr<HostThread>> threads;
std::mutex service_mutex;
std::condition_variable_any dispatcher_changed;
std::recursive_mutex gpu_interrupt_mutex;
thread_local unsigned gpu_interrupt_depth=0;
uint32_t next_handle=0x10000;
uint32_t next_thread_id=2;
uint32_t initial_thread_references=0;
struct ThreadExit { uint32_t status; };
void audio_log(const char* format,...) {
    char text[1024];va_list args;va_start(args,format);std::vsnprintf(text,sizeof(text),format,args);va_end(args);
    STARTUP_PRINT("%s",text);
}
struct AudioClient {
    airborne::PcmOutput output{audio_log};std::condition_variable_any changed;
    uint32_t credits=4;bool active=true;uint64_t frames=0;
};
std::shared_ptr<AudioClient> audio_client;
airborne::SocketRuntime socket_runtime(audio_log);
const auto guest_clock_epoch=std::chrono::steady_clock::now();
#ifndef __SWITCH__
std::chrono::steady_clock::time_point host_entry_deadline{};
#endif
uint64_t critical_import_calls[3]{};
uint64_t single_wait_calls=0;
uint64_t service_lock_samples=0,service_lock_wait_ns=0,service_lock_max_ns=0,main_service_lock_wait_ns=0;
void log_critical_import_totals() {
    STARTUP_PRINT("IMPORT_TOTAL KeWaitForSingleObject=%llu detailed_policy=FIRST_8_THEN_EVERY_4096\n",(unsigned long long)single_wait_calls);
    STARTUP_PRINT("IMPORT_TOTAL RtlEnterCriticalSection=%llu RtlLeaveCriticalSection=%llu RtlTryEnterCriticalSection=%llu detailed_policy=FIRST_8_THEN_EVERY_65536\n",
        (unsigned long long)critical_import_calls[0],(unsigned long long)critical_import_calls[1],(unsigned long long)critical_import_calls[2]);
    STARTUP_PRINT("IMPORT_TOTAL KeTlsGetValue=%llu KeTlsSetValue=%llu service_lock=BYPASSED_PER_THREAD_STORAGE\n",(unsigned long long)tls_get_calls.load(),(unsigned long long)tls_set_calls.load());
    STARTUP_PRINT("SERVICE_LOCK_TIMING samples=%llu total_wait_ns=%llu max_wait_ns=%llu main_wait_ns=%llu\n",(unsigned long long)service_lock_samples,(unsigned long long)service_lock_wait_ns,(unsigned long long)service_lock_max_ns,(unsigned long long)main_service_lock_wait_ns);
}
std::string game_files_root="extracted/base";
struct NativeFile {FILE* stream=nullptr;std::string path;~NativeFile(){if(stream)std::fclose(stream);}};
std::map<uint32_t,std::shared_ptr<NativeFile>> files;
bool close_game_files() {
    bool good=true;
    for(auto& pair:files) {
        const int rc=std::fclose(pair.second->stream);pair.second->stream=nullptr;
        STARTUP_PRINT("FILE_CLEANUP handle=%08x native=%s native_rc=%d\n",pair.first,pair.second->path.c_str(),rc);good&=rc==0;
    }
    files.clear();return good;
}
struct XNetProfile {std::array<uint8_t,13> parameters{};uint32_t references=0;} xnet_profile;
uint32_t read32(uint8_t* base,uint32_t at) {
    uint32_t v; std::memcpy(&v,ppc_guest_ptr(base,at),4); return __builtin_bswap32(v);
}
void write32(uint8_t* base,uint32_t at,uint32_t v) {
    v=__builtin_bswap32(v);std::memcpy(ppc_guest_ptr(base,at),&v,4);
}
void write64(uint8_t* base,uint32_t at,uint64_t value) {
    value=__builtin_bswap64(value);std::memcpy(ppc_guest_ptr(base,at),&value,8);
}
std::string object_path(uint8_t* base,uint32_t attributes) {
    if(!attributes || attributes%4)throw DiagnosticStop("Invalid file object attributes");
    const auto root=read32(base,attributes),name=read32(base,attributes+4);
    if(root && root!=0xfffffffd)throw DiagnosticStop("Relative file object root not implemented");
    if(!name || name%4)throw DiagnosticStop("Invalid file object name");
    const auto lengths=read32(base,name),length=lengths>>16,source=read32(base,name+4);
    if(length>(lengths&65535) || length>4096 || (!source && length))throw DiagnosticStop("Invalid file ANSI_STRING extent");
    return std::string(reinterpret_cast<char*>(ppc_guest_ptr(base,source)),length);
}
uint32_t file_network_information(uint8_t* base,uint32_t output,const std::string& native) {
        struct stat info{};
        if(stat(native.c_str(),&info)){return 0xc000000f;}
        const bool directory=(info.st_mode&S_IFMT)==S_IFDIR;
        std::error_code error;const uint64_t size=directory?0:std::filesystem::file_size(native,error);
        if(error){return 0xc0000001;}
        const uint64_t epoch=116444736000000000ull;
        write64(base,output,epoch+uint64_t(info.st_ctime)*10000000);
        write64(base,output+8,epoch+uint64_t(info.st_atime)*10000000);
        write64(base,output+16,epoch+uint64_t(info.st_mtime)*10000000);
        write64(base,output+24,epoch+uint64_t(info.st_mtime)*10000000);
        write64(base,output+32,(size+4095)&~4095ull);write64(base,output+40,size);
        write32(base,output+48,directory?0x10:0x1);write32(base,output+52,0);return 0;
}
void xma_ensure_table() {
    if(xma_context_table)return;
    if(!physical_allocator)throw DiagnosticStop("XMA context table requires physical allocator");
    const auto guest=physical_allocator->allocate(320*64,0x20000004,0,0xffffffff,256);
    if(!guest)throw DiagnosticStop("XMA context table allocation failed");
    xma_context_table=guest;
    STARTUP_PRINT("XMA_CONTEXT_TABLE guest=%08x physical=%08x contexts=320 stride=64 bytes=20480 decoding=NOT_IMPLEMENTED\n",guest,GuestPhysicalAllocator::physical(guest));
}
uint32_t gpu_read_raw(uint32_t physical) {
    if(!physical_allocator || physical%4 || !physical_allocator->contains(physical,4)) {
        STARTUP_ERROR("GPU_MEMORY_FAIL operation=read physical=%08x bytes=4 guest_alias=%08x\n",physical,0xa0000000u+physical);
#ifdef __SWITCH__
        MemoryInfo info{};u32 page=0;const auto address=uintptr_t(active_base)+0xa0000000ull+physical;
        const Result rc=svcQueryMemory(&info,&page,address);
        STARTUP_ERROR("GPU_MEMORY_QUERY address=%016llx rc=%08x base=%016llx size=%016llx state=%08x attributes=%08x permissions=%08x page=%08x\n",(unsigned long long)address,rc,(unsigned long long)info.addr,(unsigned long long)info.size,info.type,info.attr,info.perm,page);
#endif
        throw DiagnosticStop("GPU read outside live physical allocation");
    }
    uint32_t value;std::memcpy(&value,active_base+0xa0000000ull+physical,4);return value;
}
void gpu_require_live(uint32_t physical,uint32_t bytes,bool write,const char* operation) {
    if(physical_allocator && physical_allocator->contains(physical,bytes,write))return;
    STARTUP_ERROR("GPU_MEMORY_FAIL operation=%s physical=%08x guest=%08x bytes=%u requested_write=%u tracked_protection=%08x\n",operation,physical,0xa0000000u+physical,bytes,write,
        physical_allocator?physical_allocator->query_protection(0xa0000000u+physical):0);
#ifdef __SWITCH__
    for(unsigned alias=0;alias<3;++alias){
        const auto address=uintptr_t(active_base)+0xa0000000ull+uint64_t(alias)*0x20000000+physical;
        MemoryInfo info{};u32 page=0;const auto result=svcQueryMemory(&info,&page,address);
        STARTUP_ERROR("GPU_MEMORY_QUERY operation=%s alias=%u address=%016llx bytes=%u rc=%08x base=%016llx size=%016llx state=%08x attributes=%08x permissions=%08x page=%08x\n",operation,alias,(unsigned long long)address,bytes,result,(unsigned long long)info.addr,(unsigned long long)info.size,info.type,info.attr,info.perm,page);
    }
#endif
    throw DiagnosticStop(std::string(operation)+" outside live physical allocation");
}
void gpu_write_raw(uint32_t physical,uint32_t value) {
    if(!physical_allocator || physical%4 || !physical_allocator->contains(physical,4,true)) {
        STARTUP_ERROR("GPU_MEMORY_FAIL operation=write physical=%08x bytes=4 guest_alias=%08x value=%08x\n",physical,0xa0000000u+physical,value);
#ifdef __SWITCH__
        MemoryInfo info{};u32 page=0;const auto address=uintptr_t(active_base)+0xa0000000ull+physical;
        const Result rc=svcQueryMemory(&info,&page,address);
        STARTUP_ERROR("GPU_MEMORY_QUERY address=%016llx rc=%08x base=%016llx size=%016llx state=%08x attributes=%08x permissions=%08x page=%08x\n",(unsigned long long)address,rc,(unsigned long long)info.addr,(unsigned long long)info.size,info.type,info.attr,info.perm,page);
#endif
        throw DiagnosticStop("GPU write outside writable physical allocation");
    }
    STARTUP_PRINT("GPU_MEMORY_WRITE physical=%08x bytes=4 raw=%08x big_endian=%08x\n",physical,value,__builtin_bswap32(value));
    std::memcpy(active_base+0xa0000000ull+physical,&value,4);
}
struct GpuStageTimer {
    const char* stage;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();
    explicit GpuStageTimer(const char* name):stage(name){}
    ~GpuStageTimer(){STARTUP_PRINT("GPU_STAGE_TIME stage=%s elapsed_us=%llu\n",stage,(unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count());}
};
void gpu_present(uint32_t physical,const std::array<uint32_t,6>& fetch) {
    GpuStageTimer timer("front_buffer_decode_and_present");
    if((fetch[0]&3)!=2 || (fetch[1]&63)!=6 || ((fetch[5]>>9)&3)!=1)
        throw DiagnosticStop("Unsupported front-buffer texture type/format/dimension");
    airborne::FrontBuffer layout{(fetch[2]&8191)+1,((fetch[2]>>13)&8191)+1,((fetch[0]>>22)&511)*32,(fetch[1]>>6)&3,bool(fetch[0]>>31)};
    if((fetch[3]&1) || (fetch[3]&0x7e000))throw DiagnosticStop("Unsupported front-buffer integer format/exponent adjustment");
    layout.swizzle=(fetch[3]>>1)&4095;
    const auto bytes=layout.required_bytes();
    STARTUP_PRINT("GPU_FRONT_BUFFER physical=%08x extent=%u width=%u height=%u pitch=%u tiled=%u endian=%u\n",physical,bytes,layout.width,layout.height,layout.pitch,layout.tiled,layout.endian);
    gpu_require_live(physical,bytes,false,"front-buffer-read");
    const auto pixels=layout.decode(active_base+0xa0000000ull+physical,bytes);
    graphics_device->display_rgba(pixels,layout.width,layout.height);
    graphics_ring->enable_display_wait(true);
}
bool gpu_startup_resolve(const std::array<uint32_t,0x5003>& regs,const std::array<airborne::CommandRing::ShaderCode,2>& shaders,uint32_t initiator) {
    const bool movie=!graphics_movie_color.rgba.empty();
    const auto* pixel=movie?video_code_1:startup_shader_1;
    const auto pixel_words=movie?std::size(video_code_1):std::size(startup_shader_1);
    if(initiator!=0x30088 || regs[0x2208]!=6 || (!graphics_zero_depth.valid && !movie) || regs[0x2204]!=0x10000 ||
        regs[0x2205]!=0x10000 || regs[0x2206]!=0x300 || regs[0x2302]!=4 || regs[0x2080] || regs[0x2081] ||
        regs[0x200e] || regs[0x200f]!=0x20002000 || regs[0x2293] || regs[0x2318]!=0x100000 ||
        regs[0x231b]!=0x01000300 || (regs[0x4800]&3)!=3 || regs[0x4801]!=0x1000001a ||
        shaders[0].words.size()!=std::size(startup_resolve_vertex) || shaders[1].words.size()!=pixel_words ||
        !std::equal(shaders[0].words.begin(),shaders[0].words.end(),std::begin(startup_resolve_vertex)) ||
        !std::equal(shaders[1].words.begin(),shaders[1].words.end(),pixel))return false;
    const uint32_t width=regs[0x231a]&0x3fff,height=(regs[0x231a]>>16)&0x3fff,destination=regs[0x2319],source=regs[0x4800]&~3u;
    if(width!=1280 || height!=720 || destination!=graphics_front_buffer || (regs[0x2000]&0x3ffff)!=width ||
        regs[0x2001]!=(movie?graphics_movie_color.tile:graphics_zero_depth.tile) || regs[0x2082]!=((height<<16)|width) ||
        (!movie && uint64_t(width)*height!=uint64_t(graphics_zero_depth.width)*graphics_zero_depth.height*graphics_zero_depth.samples) ||
        !physical_allocator->contains(source,24))return false;
    float vertices[6];
    for(unsigned i=0;i<6;++i){auto bits=__builtin_bswap32(gpu_read_raw(source+4*i));std::memcpy(&vertices[i],&bits,4);}
    if(vertices[0]!=-0.5f || vertices[1]!=-0.5f || vertices[2]!=float(width)-0.5f || vertices[3]!=-0.5f ||
        vertices[4]!=vertices[2] || vertices[5]!=float(height)-0.5f)return false;
    airborne::FrontBuffer layout{width,height,width,0,true};const auto bytes=layout.required_bytes();
    gpu_require_live(destination,bytes,true,"edram-resolve-write");
    GpuStageTimer timer("edram_resolve");
    graphics_device->synchronize();
    // The preceding completed D24S8 clear establishes every bit of this EDRAM
    // range as zero. Reinterpreting that same full range as RGBA8, converting
    // unsigned zero and swapping red/blue still yields zero. No unknown EDRAM
    // contents or arbitrary title frame can take this optimized resolve path.
    const uint32_t zero=0;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
        auto* output=active_base+0xa0000000ull+destination+layout.offset(x,y);
        if(movie) {
            const auto* rgba=graphics_movie_color.rgba.data()+(size_t(y)*width+x)*4;
            // RB_COPY_DEST_INFO=01000300: RGBA8 UNORM, endian none,
            // red/blue swap enabled. Do not apply the display fetch swizzle here.
            output[0]=rgba[2];output[1]=rgba[1];output[2]=rgba[0];output[3]=rgba[3];
        }else std::memcpy(output,&zero,4);
    }
    STARTUP_PRINT("GPU_EDRAM_RESOLVE_COMPLETE mode=%s tile=%u destination=%08x extent=%u width=%u height=%u touched_bytes=%u\n",movie?"NATIVE_MOVIE_RGBA":"KNOWN_ZERO_DEPTH_ALIAS",movie?graphics_movie_color.tile:graphics_zero_depth.tile,destination,bytes,width,height,width*height*4);
    return true;
}
bool gpu_startup_clear(const std::array<uint32_t,0x5003>& regs,const std::array<airborne::CommandRing::ShaderCode,2>& shaders,uint32_t initiator) {
#ifndef __SWITCH__
    // Private source-data capture for the first exercised YUV video draw. No
    // draw is acknowledged here; execution still stops at the unsupported path.
    static bool video_captured=false;
    if(!video_captured && initiator==0x60084 && (regs[0x4801]&63)==2 && (regs[0x4807]&63)==2 && (regs[0x480d]&63)==2) {
        const auto folder=std::filesystem::path("generated/captures/video");std::filesystem::create_directories(folder);
        std::ofstream metadata(folder/"state.json");metadata<<"{\"initiator\":"<<initiator<<",\"registers\":[";
        for(unsigned i=0;i<regs.size();++i){if(i)metadata<<',';metadata<<regs[i];}metadata<<"]}";
        if(!metadata)throw DiagnosticStop("Video state capture failed");
        for(unsigned plane=0;plane<3;++plane) {
            const auto first=0x4800+plane*6,physical=regs[first+1]&0xfffff000u;
            const auto pitch=((regs[first]>>22)&511)*32,height=((regs[first+2]>>13)&8191)+1;
            const auto bytes=pitch*height;
            if(regs[first]>>31 || ((regs[first+1]>>6)&3) || !physical_allocator->contains(physical,bytes))throw DiagnosticStop("Unsupported YUV capture layout");
            std::ofstream stream(folder/("plane"+std::to_string(plane)+".bin"),std::ios::binary);
            stream.write(reinterpret_cast<char*>(active_base+0xa0000000ull+physical),bytes);
            if(!stream)throw DiagnosticStop("Video plane capture failed");
            STARTUP_PRINT("GPU_VIDEO_CAPTURE plane=%u physical=%08x pitch=%u height=%u bytes=%u\n",plane,physical,pitch,height,bytes);
        }
        for(unsigned stage=0;stage<2;++stage) {
            std::ofstream stream(folder/("stage"+std::to_string(stage)+".bin"),std::ios::binary);
            for(auto word:shaders[stage].words){const auto raw=__builtin_bswap32(word);stream.write(reinterpret_cast<const char*>(&raw),4);}
            if(!stream)throw DiagnosticStop("Video shader capture failed");
        }
        video_captured=true;
    }
#endif
    if(airborne::video_draw_state(regs,initiator) &&
       shaders[0].words.size()==std::size(video_code_0) && shaders[1].words.size()==std::size(video_code_1) &&
       std::equal(shaders[0].words.begin(),shaders[0].words.end(),std::begin(video_code_0)) &&
       std::equal(shaders[1].words.begin(),shaders[1].words.end(),std::begin(video_code_1))) {
        GpuStageTimer timer("movie_upload_draw_readback");
        static auto previous_movie=std::chrono::steady_clock::time_point{};
        static uint64_t movie_frame=0;const auto now=std::chrono::steady_clock::now();
        STARTUP_PRINT("MOVIE_FRAME sequence=%llu uptime_ms=%llu interval_us=%llu\n",(unsigned long long)++movie_frame,(unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(now-guest_clock_epoch).count(),(unsigned long long)(previous_movie.time_since_epoch().count()?std::chrono::duration_cast<std::chrono::microseconds>(now-previous_movie).count():0));previous_movie=now;
        const uint32_t source=regs[0x48be]&~3u;
        gpu_require_live(source,120,false,"movie-vertex-read");
        const float vertices[30]={0,0,0,0,0,1280,0,0,1,0,0,720,0,0,1,0,720,0,0,1,1280,0,0,1,0,1280,720,0,1,1};
        for(unsigned i=0;i<30;++i){const auto bits=__builtin_bswap32(gpu_read_raw(source+4*i));uint32_t expected;std::memcpy(&expected,&vertices[i],4);if(bits!=expected)return false;}
        std::vector<uint8_t> planes[3];
        for(unsigned i=0;i<3;++i) {
            const auto at=0x4800+6*i,physical=regs[at+1]&0xfffff000u,pitch=((regs[at]>>22)&511)*32;
            const uint32_t width=i?640:1280,height=i?360:720,bytes=pitch*height;
            STARTUP_PRINT("GPU_MOVIE_PLANE plane=%u physical=%08x guest=%08x width=%u height=%u pitch=%u bytes=%u\n",i,physical,0xa0000000u+physical,width,height,pitch,bytes);
            gpu_require_live(physical,bytes,false,"movie-plane-read");
            planes[i].resize(size_t(width)*height);
            for(uint32_t y=0;y<height;++y)std::memcpy(planes[i].data()+size_t(y)*width,active_base+0xa0000000ull+physical+size_t(y)*pitch,width);
        }
        auto pixels=graphics_device->draw_video(video_spirv_0,std::size(video_spirv_0),video_spirv_1,std::size(video_spirv_1),planes);
        graphics_zero_depth.valid=false;graphics_movie_color.tile=regs[0x2001];graphics_movie_color.rgba=std::move(pixels);
        STARTUP_PRINT("GPU_MOVIE_EDRAM_OWNER tile=%u bytes=%zu source=COMPLETED_NATIVE_ATTACHMENT\n",graphics_movie_color.tile,graphics_movie_color.rgba.size());return true;
    }
    if(airborne::masked_point_state(regs,initiator) &&
        shaders[0].words.size()==std::size(point_code_0) && shaders[1].words.size()==std::size(point_code_1) &&
        std::equal(shaders[0].words.begin(),shaders[0].words.end(),std::begin(point_code_0)) &&
        std::equal(shaders[1].words.begin(),shaders[1].words.end(),std::begin(point_code_1))) {
        graphics_device->draw_masked_point(point_spirv_0,std::size(point_spirv_0),point_spirv_1,std::size(point_spirv_1));return true;
    }
    if(regs[0x2208]==6)return gpu_startup_resolve(regs,shaders,initiator);
    // This exact XDK program exports its fetched XYZ position with W=1 and
    // color only (no discard, depth export or memory side effects). Its first
    // rectangle is a full-surface depth/stencil clear. Keep this proof narrow;
    // all other draws go to the unsupported draw diagnostic.
    if(initiator!=0x30088 || regs[0x2208]!=5 || regs[0x2200]!=0x8777 || regs[0x2204]!=0x10000 ||
        regs[0x2205]!=0x10000 || regs[0x2206]!=0x300 || regs[0x2302]!=4 || regs[0x210c]!=0xffff00 ||
        regs[0x200e] || regs[0x200f]!=0x20002000 || regs[0x2080] || regs[0x2081] || regs[0x2082]!=0x20002000 || regs[0x2293] ||
        regs[0x4801]!=0x10000056 || (regs[0x4800]&3)!=3 || (regs[0x2002]&~0x7ffu) ||
        shaders[0].words.size()!=std::size(startup_shader_0) || shaders[1].words.size()!=std::size(startup_shader_1) ||
        !std::equal(shaders[0].words.begin(),shaders[0].words.end(),std::begin(startup_shader_0)) ||
        !std::equal(shaders[1].words.begin(),shaders[1].words.end(),std::begin(startup_shader_1)))return false;
    const uint32_t source=regs[0x4800]&~3u,width=regs[0x2000]&0x3fff,samples=(regs[0x2000]>>16)&3;
    if(width!=640 || samples!=2 || !physical_allocator->contains(source,84))return false;
    float position[3][3];uint32_t z[3];
    for(unsigned vertex=0;vertex<3;++vertex)for(unsigned component=0;component<3;++component) {
        const auto bits=__builtin_bswap32(gpu_read_raw(source+(vertex*7+component)*4));
        std::memcpy(&position[vertex][component],&bits,4);if(component==2)z[vertex]=bits;
    }
    const float height_value=position[2][1]+0.5f;
    if(!(height_value>=1 && height_value<=4096))return false;
    const uint32_t height=uint32_t(height_value),tile=regs[0x2002]&0x7ff;
    // PA_SU_VTX_CNTL has D3D integer centers: adding 0.5 makes exact
    // rectangle edges [0,width] x [0,height], including every MSAA sample.
    // Clip-disabled constant negative Z clamps to zero in D24 UNORM.
    if(height!=360 || float(height)!=height_value || position[0][0]!=-0.5f || position[0][1]!=-0.5f ||
        position[1][0]!=float(width)-0.5f || position[1][1]!=-0.5f || position[2][0]!=position[1][0] ||
        z[0]!=0xb1800001 || z[1]!=z[0] || z[2]!=z[0] || uint64_t(tile)*5120+uint64_t(width)*height*16>10*1024*1024)return false;
    STARTUP_PRINT("GPU_STARTUP_CLEAR source=%08x bytes=84 tile=%u width=%u height=%u samples=4 proof=EXACT_SHADERS_FULL_RECT_DEPTH_ALWAYS_STENCIL_REPLACE\n",source,tile,width,height);
    graphics_device->clear_depth(tile,width,height,4,0.0f,0);
    graphics_movie_color.rgba.clear();
    graphics_zero_depth={true,tile,width,height,4};
    return true;
}
void gpu_dispatch_interrupts(PPCContext& interrupted,const std::vector<uint32_t>& cpus,uint32_t callback,uint32_t argument,uint32_t source=1) {
    if(cpus.empty())return;
    if(!callback)throw DiagnosticStop("GPU interrupt has no registered guest callback");
    std::lock_guard<std::recursive_mutex> serialize(gpu_interrupt_mutex);
    if(gpu_interrupt_depth>=8)throw DiagnosticStop("GPU interrupt nesting limit");
    const uint32_t pcr=interrupted.r13.u32,low=read32(active_base,pcr+0x74);
    if(interrupted.r1.u32<low+0x400)throw DiagnosticStop("Insufficient guest stack for GPU interrupt");
    for(auto cpu:cpus) {
        GuestFpScope restore_native_fp(interrupted);
        const uint32_t tls=read32(active_base,pcr);
        const uint8_t irql=*ppc_guest_ptr(active_base,pcr+0x18),old_cpu=*ppc_guest_ptr(active_base,pcr+0x10c);
        if(irql>2)throw DiagnosticStop("GPU interrupt delivery above supported IRQL");
        PPCContext context=interrupted;context.r1.u64=interrupted.r1.u32-0x100;context.r3.u64=source;context.r4.u64=argument;
        context.lr=0;context.ctr.u64=0;initialize_guest_fp(context);
        auto* previous=active_context;active_context=&context;++gpu_interrupt_depth;
        write32(active_base,pcr,0);*ppc_guest_ptr(active_base,pcr+0x18)=2;*ppc_guest_ptr(active_base,pcr+0x10c)=uint8_t(cpu);
        auto restore=[&](){write32(active_base,pcr,tls);*ppc_guest_ptr(active_base,pcr+0x18)=irql;*ppc_guest_ptr(active_base,pcr+0x10c)=old_cpu;active_context=previous;--gpu_interrupt_depth;};
        STARTUP_PRINT("GPU_INTERRUPT_DISPATCH callback=%08x argument=%08x cpu=%u source=%u irql=2\n",callback,argument,cpu,source);
        try{(PPC_LOOKUP_FUNC(active_base,callback))(context,active_base);restore();}
        catch(...){restore();throw;}
    }
}
std::shared_ptr<HostEvent> kernel_event(uint8_t* base,uint32_t object) {
    if(object%4)throw DiagnosticStop("Unaligned kernel event object");
    const uint8_t type=*ppc_guest_ptr(base,object);
    if(type>1)throw DiagnosticStop("Unsupported kernel dispatcher object type "+std::to_string(type));
    auto& event=event_objects[object];
    if(!event) {
        event=std::make_shared<HostEvent>();event->manual=type==0;
        event->signalled=read32(base,object+4)!=0;
        STARTUP_PRINT("KERNEL_EVENT_BIND object=%08x type=%u initial=%u\n",object,type,event->signalled);
    }
    return event;
}
uint64_t wait_milliseconds(uint8_t* base,uint32_t pointer) {
    if(!pointer)return INFINITE;
    uint64_t raw;std::memcpy(&raw,ppc_guest_ptr(base,pointer),8);raw=__builtin_bswap64(raw);
    if(raw>>63)return ((~raw+1)+9999)/10000;
    FILETIME now;GetSystemTimeAsFileTime(&now);
    const auto ticks=(uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime;
    return raw>ticks ? (raw-ticks+9999)/10000:0;
}
std::vector<uint8_t> read_file(const char* path,size_t limit) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f || f.tellg()<0 || uint64_t(f.tellg())>limit)throw DiagnosticStop("Invalid input file size");
    std::vector<uint8_t> b(size_t(f.tellg())); f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(b.data()),b.size()))throw DiagnosticStop("Input read failed");
    return b;
}
PPC_FUNC(unmapped_target) { throw DiagnosticStop("Unmapped indirect guest target"); }
void setup_thread_memory(uint8_t* base,uint32_t object,uint32_t pcr,uint32_t tls,uint32_t low,uint32_t high,uint32_t id,uint32_t entry) {
    base[object]=6;
    write32(base,object+0x5c,high);write32(base,object+0x60,low);write32(base,object+0x68,tls);
    write32(base,object+0x14c,id);write32(base,object+0x150,entry);
    write32(base,pcr,tls);write32(base,pcr+0x30,pcr);write32(base,pcr+0x70,high);
    write32(base,pcr+0x74,low);write32(base,pcr+0x100,object);
}
DWORD WINAPI guest_thread_start(void* argument) {
    auto* thread=static_cast<HostThread*>(argument);
    current_thread_address=thread->object;current_tls_address=thread->tls;
    PPCContext ctx{};initialize_guest_fp(ctx);ctx.r13.u64=thread->pcr;ctx.r1.u64=thread->stack_high-0x100;
    uint32_t target=thread->startup ? thread->startup : thread->entry;
    if(thread->startup){ctx.r3.u64=thread->entry;ctx.r4.u64=thread->argument;}
    else ctx.r3.u64=thread->argument;
    active_context=&ctx;
    auto finish=[&](uint32_t status) {
        std::lock_guard<std::mutex> guard(service_mutex);
        write32(active_base,thread->object+0x140,status);write32(active_base,thread->object+4,1);
        active_context=nullptr;return status;
    };
    try {
        if(target<PPC_CODE_BASE || target>=PPC_CODE_BASE+PPC_CODE_SIZE || target%4)throw DiagnosticStop("Invalid guest thread entry");
        if(thread->audio) {
            auto client=audio_client;
            for(;;) {
                std::unique_lock<std::mutex> lock(service_mutex);
                client->changed.wait(lock,[&]{return client->credits || !client->active || imports_stopping
#ifdef __SWITCH__
                    || nx_runtime_stopping()
#endif
                    ;});
                if(!client->active || imports_stopping
#ifdef __SWITCH__
                    || nx_runtime_stopping()
#endif
                    )break;
                --client->credits;lock.unlock();ctx.r3.u64=thread->argument;
                (PPC_LOOKUP_FUNC(active_base,target))(ctx,active_base);
            }
        }else (PPC_LOOKUP_FUNC(active_base,target))(ctx,active_base);
        return finish(ctx.r3.u32);
    } catch(const ThreadExit& exit){return finish(exit.status);}
    catch(const std::exception& e) {
#ifdef __SWITCH__
        if(nx_runtime_stopping())return finish(0);
#endif
        STARTUP_ERROR("THREAD_STOP id=%u %s LR=0x%08x SP=0x%08x CTR=0x%08x\n",thread->id,e.what(),uint32_t(ctx.lr),ctx.r1.u32,ctx.ctr.u32);
        STARTUP_FLUSH();ExitProcess(10);
    }
}
void bind_imports(uint8_t* base) {
    // This exact XDK GetTickCount leaf reads KeTimeStampBundle+16. Replace the
    // leaf with a monotonic platform clock, rather than exposing an unchanging
    // bundle or racing a periodic native writer against guest memory loads.
    constexpr uint32_t clock_code[]={0x3d608200,0x816b0990,0x806b0010,0x4e800020};
    for(unsigned i=0;i<4;++i)if(read32(base,0x82c5b7d8+i*4)!=clock_code[i])throw DiagnosticStop("XDK tick leaf does not match clock hook");
    if(PPC_LOOKUP_FUNC(base,0x82c5b7d8)!=&sub_82C5B7D8)throw DiagnosticStop("XDK clock hook missing from lookup table");
    STARTUP_PRINT("XDK_CLOCK_HOOK guest=82c5b7d8 source=STEADY_UPTIME_MILLISECONDS width=32 bytes_verified=16\n");
    write32(base,module_variable,module);
    write32(base,module+0x1c,PPC_IMAGE_BASE);
    write32(base,module+0x20,PPC_IMAGE_SIZE);
    write32(base,module+0x38,PPC_IMAGE_SIZE);
    write32(base,module+0x3c,0x82c5c310);
    write32(base,module+0x58,header);
    std::memcpy(base+module_variable+0x100,"\"default.xex\"",14);
    write32(base,module_variable+0x200,0); // Guest D3D device pointer, written by the game.
    write32(base,module_variable+0x204,500); // Declared virtual Xenos clock, not native GPU MHz.
    PPCContext calibration{};calibration.r3.u64=module_variable+0x240;calibration.r4.u64=10000;
    host_import("RtlInitializeCriticalSectionAndSpinCount",calibration,base);
    unsigned variable_index=0;
    for(const auto& binding:import_bindings) {
        if(binding.thunk) {write32(base,binding.slot,binding.thunk);continue;}
        // Unsupported variables point to distinct uncommitted pages. Do not
        // leave ordinal numbers or fabricated zero-filled structures behind.
        uint32_t value=0x71000000+0x10000*variable_index++;
        if(std::strcmp(binding.name,"XexExecutableModuleHandle")==0)value=module_variable;
        if(std::strcmp(binding.name,"KeDebugMonitorData")==0)value=module_variable+4;
        if(std::strcmp(binding.name,"KeCertMonitorData")==0)value=module_variable+8;
        if(std::strcmp(binding.name,"ExLoadedCommandLine")==0)value=module_variable+0x100;
        if(std::strcmp(binding.name,"VdGlobalDevice")==0)value=module_variable+0x200;
        if(std::strcmp(binding.name,"VdGpuClockInMHz")==0)value=module_variable+0x204;
        if(std::strcmp(binding.name,"VdHSIOCalibrationLock")==0)value=module_variable+0x240;
        write32(base,binding.slot,value);
    }
}
}

// Strong implementation overrides only the generated weak alias. Both direct
// guest calls and the generated function table resolve to this same clock.
PPC_FUNC(sub_82C5B7D8) {
#ifdef __SWITCH__
    if(nx_runtime_stopping())throw ThreadExit{0};
#endif
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-guest_clock_epoch).count();
    ctx.r11.u64=read32(base,0x82000990);ctx.r3.u64=uint32_t(elapsed);
}

void host_import(const char* name,PPCContext& ctx,uint8_t* base) {
    GuestFpScope floating_point(ctx);
    // These APIs touch only the calling thread's permanent TLS block, exactly
    // as the kernel's direct per-thread read/write. Never queue them behind
    // GPU waits, audio output, shared allocators or file/log I/O.
    if(!std::strcmp(name,"KeTlsGetValue") || !std::strcmp(name,"KeTlsSetValue")) {
        if(imports_stopping.load(std::memory_order_relaxed))throw ThreadExit{0};
#ifdef __SWITCH__
        if(nx_runtime_stopping())throw ThreadExit{0};
#endif
        const bool get=!std::strcmp(name,"KeTlsGetValue");const auto slot=ctx.r3.u32;
        (get?tls_get_calls:tls_set_calls).fetch_add(1,std::memory_order_relaxed);
        if(get)ctx.r3.u64=slot<64?read32(base,current_tls_address+slot*4):0;
        else if(slot<64){write32(base,current_tls_address+slot*4,ctx.r4.u32);ctx.r3.u64=1;}
        else ctx.r3.u64=0;
        return;
    }
    const auto service_wait_started=std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> service_lock(service_mutex);
    const auto waited=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-service_wait_started).count());
    ++service_lock_samples;service_lock_wait_ns+=waited;service_lock_max_ns=std::max(service_lock_max_ns,waited);
    if(current_thread_address==initial_thread)main_service_lock_wait_ns+=waited;
    if(imports_stopping)throw ThreadExit{0};
#ifdef __SWITCH__
    if(nx_runtime_stopping())throw ThreadExit{0};
#else
    if(host_entry_deadline.time_since_epoch().count() && std::chrono::steady_clock::now()>host_entry_deadline)
        throw DiagnosticStop("HOST_ENTRY_DEADLINE reached; startup remains incomplete");
#endif
    const bool single_wait=!std::strcmp(name,"KeWaitForSingleObject");
    const auto wait_count=single_wait?++single_wait_calls:0;
    unsigned critical=3;
    if(!std::strcmp(name,"RtlEnterCriticalSection"))critical=0;
    else if(!std::strcmp(name,"RtlLeaveCriticalSection"))critical=1;
    else if(!std::strcmp(name,"RtlTryEnterCriticalSection"))critical=2;
    const uint64_t count=critical<3?++critical_import_calls[critical]:0;
    if((critical==3 || count<=8 || count%65536==0) && (!single_wait || wait_count<=8 || wait_count%4096==0)) {
      if(single_wait)STARTUP_PRINT("IMPORT_SAMPLED name=%s total_calls=%llu\n",name,(unsigned long long)wait_count);
      if(critical<3)STARTUP_PRINT("IMPORT_SAMPLED name=%s total_calls=%llu\n",name,(unsigned long long)count);
      STARTUP_PRINT("IMPORT %s LR=0x%08x r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x r8=0x%08x r9=0x%08x SP=0x%08x R13=0x%08x CTR=0x%08x\n",
        name,uint32_t(ctx.lr),ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,ctx.r8.u32,ctx.r9.u32,ctx.r1.u32,ctx.r13.u32,ctx.ctr.u32);
      STARTUP_FLUSH();
    }
    if(std::strcmp(name,"MmQueryAddressProtect")==0) {
        const auto address=ctx.r3.u32;
        if(address>=0xa0000000 && physical_allocator){ctx.r3.u64=physical_allocator->query_protection(address);return;}
        if(address>=0x20000000 && address<0x60000000 && allocator){ctx.r3.u64=allocator->query_protection(address);return;}
        throw DiagnosticStop("Protection query outside tracked allocator arenas");
    }
    if(std::strcmp(name,"RtlTimeToTimeFields")==0) {
        if(!ctx.r3.u32 || ctx.r3.u32%8 || !ctx.r4.u32 || ctx.r4.u32%2)throw DiagnosticStop("Invalid time-fields pointers");
        uint64_t ticks;std::memcpy(&ticks,ppc_guest_ptr(base,ctx.r3.u32),8);ticks=__builtin_bswap64(ticks);
        uint16_t fields[8]{};
#ifdef __SWITCH__
        const time_t seconds=time_t(int64_t(ticks/10000000)-11644473600ll);struct tm calendar{};
        if(!gmtime_r(&seconds,&calendar) || calendar.tm_year+1900>32767)throw DiagnosticStop("Unsupported time-fields range");
        fields[0]=uint16_t(calendar.tm_year+1900);fields[1]=uint16_t(calendar.tm_mon+1);fields[2]=uint16_t(calendar.tm_mday);
        fields[3]=uint16_t(calendar.tm_hour);fields[4]=uint16_t(calendar.tm_min);fields[5]=uint16_t(calendar.tm_sec);
        fields[6]=uint16_t((ticks/10000)%1000);fields[7]=uint16_t(calendar.tm_wday);
#else
        FILETIME input{uint32_t(ticks),uint32_t(ticks>>32)};SYSTEMTIME calendar{};
        if(!FileTimeToSystemTime(&input,&calendar))throw DiagnosticStop("Native FILETIME conversion failed");
        fields[0]=calendar.wYear;fields[1]=calendar.wMonth;fields[2]=calendar.wDay;fields[3]=calendar.wHour;
        fields[4]=calendar.wMinute;fields[5]=calendar.wSecond;fields[6]=calendar.wMilliseconds;fields[7]=calendar.wDayOfWeek;
#endif
        for(auto& field:fields)field=__builtin_bswap16(field);std::memcpy(ppc_guest_ptr(base,ctx.r4.u32),fields,16);return;
    }
    if(std::strcmp(name,"NtSetInformationFile")==0) {
        const auto io=ctx.r4.u32,source=ctx.r5.u32,size=ctx.r6.u32,kind=ctx.r7.u32;
        if(!io || io%4 || !source || source%8){ctx.r3.u64=0xc000000d;return;}
        auto finish=[&](uint32_t status){write32(base,io,status);write32(base,io+4,0);ctx.r3.u64=status;};
        auto found=files.find(ctx.r3.u32);if(found==files.end()){finish(0xc0000008);return;}
        if(kind!=14)throw DiagnosticStop("Unsupported set-file information class "+std::to_string(kind));
        if(size<8){finish(0xc0000023);return;}
        uint64_t offset;std::memcpy(&offset,ppc_guest_ptr(base,source),8);offset=__builtin_bswap64(offset);
        if(offset>INT64_MAX){finish(0xc000000d);return;}
#ifdef __SWITCH__
        const int rc=fseeko(found->second->stream,off_t(offset),SEEK_SET);
#else
        const int rc=_fseeki64(found->second->stream,int64_t(offset),SEEK_SET);
#endif
        STARTUP_PRINT("FILE_SEEK handle=%08x native=%s source=%08x source_bytes=8 offset=%016llx native_rc=%d\n",ctx.r3.u32,found->second->path.c_str(),source,(unsigned long long)offset,rc);
        finish(rc?0xc000000d:0);return;
    }
    if(std::strcmp(name,"RtlRaiseException")==0) {
        const auto record=ctx.r3.u32;
        if(!record || record%4)throw DiagnosticStop("Invalid guest exception record");
        const auto code=read32(base,record),flags=read32(base,record+4),address=read32(base,record+12),count=read32(base,record+16);
        STARTUP_PRINT("GUEST_EXCEPTION record=%08x code=%08x flags=%08x address=%08x parameters=%u\n",record,code,flags,address,count);
        for(unsigned i=0;i<std::min(count,15u);++i)STARTUP_PRINT("GUEST_EXCEPTION_PARAMETER index=%u value=%08x\n",i,read32(base,record+20+i*4));
        if(code==0x406d1388 && !flags && count==4 && read32(base,record+20)==0x1000 && !read32(base,record+32)) {
            const auto pointer=read32(base,record+24),id=read32(base,record+28);
            if(!pointer)throw DiagnosticStop("Missing debugger thread name");
            std::string text;for(unsigned i=0;i<256;++i){const auto c=*ppc_guest_ptr(base,pointer+i);if(!c)break;text+=char(c);}
            if(text.size()==256)throw DiagnosticStop("Unterminated debugger thread name");
            bool found=(id==1 || (id==0xffffffff && current_thread_address==initial_thread));
            for(auto& pair:threads)if(pair.second->id==id || (id==0xffffffff && pair.second->object==current_thread_address)){pair.second->name=text;found=true;break;}
            if(!found)throw DiagnosticStop("Unknown debugger thread-name target "+std::to_string(id));
            STARTUP_PRINT("THREAD_DEBUG_NAME id=%u name=%s delivery=DEBUGGER_NOTIFICATION\n",id,text.c_str());return;
        }
        throw DiagnosticStop("Guest raised exception code="+std::to_string(code));
    }
    if(std::strcmp(name,"NetDll_WSAStartup")==0) {
        if(ctx.r3.u32!=1 || !ctx.r5.u32 || ctx.r5.u32%4){ctx.r3.u64=10022;return;}
        airborne::SocketRuntime::Data data;const auto status=socket_runtime.start(ctx.r4.u32,data);
        if(status){ctx.r3.u64=status;return;}
        const auto output=ctx.r5.u32;
        write32(base,output,(uint32_t(data.version)<<16)|data.highest);
        std::memcpy(ppc_guest_ptr(base,output+4),data.description.data(),257);
        std::memcpy(ppc_guest_ptr(base,output+261),data.status.data(),129);
        write32(base,output+390,(uint32_t(data.max_sockets)<<16)|data.max_udp);
        // Preserve padding and the guest's vendor-info pointer at +396.
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NetDll_WSACleanup")==0) {
        ctx.r3.u64=ctx.r3.u32==1?socket_runtime.cleanup():10022;return;
    }
    if(std::strcmp(name,"MmQueryStatistics")==0) {
        const auto output=ctx.r3.u32;
        if(!output || output%4){ctx.r3.u64=0xc000000d;return;}
        if(read32(base,output)!=104){ctx.r3.u64=0xc0000023;return;}
        if(!allocator || !physical_allocator)throw DiagnosticStop("Memory statistics require live allocators");
        const auto physical_pages=uint32_t(physical_allocator->allocated_bytes()/4096);
        const auto virtual_pages=uint32_t(allocator->committed_bytes()/4096);
        const auto image_pages=uint32_t((PPC_IMAGE_SIZE+4095)/4096);
        uint32_t stack_pages=stack_size/4096;
        for(const auto& pair:threads)stack_pages+=(pair.second->stack_high-pair.second->stack_low)/4096;
        const auto kernel_pages=uint32_t((PPC_CODE_SIZE*2+4095)/4096)+9+uint32_t(threads.size())*3;
        const uint64_t used=uint64_t(physical_pages)+virtual_pages+image_pages+stack_pages+kernel_pages;
        const uint32_t total=0x20000,available=used<total?total-uint32_t(used):0;
        std::memset(ppc_guest_ptr(base,output),0,104);
        write32(base,output,104);write32(base,output+4,total);write32(base,output+8,kernel_pages);
        write32(base,output+12,available);write32(base,output+16,0x40000000);
        write32(base,output+20,uint32_t(allocator->reserved_bytes()));write32(base,output+24,physical_pages);
        write32(base,output+32,stack_pages);write32(base,output+36,image_pages);write32(base,output+44,virtual_pages);
        write32(base,output+100,total-1);
        STARTUP_PRINT("MEMORY_STATISTICS total_pages=%u available_pages=%u physical_pages=%u virtual_pages=%u image_pages=%u stack_pages=%u kernel_pages=%u budget=VIRTUAL_XBOX_512_MIB\n",
            total,available,physical_pages,virtual_pages,image_pages,stack_pages,kernel_pages);
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NtQueryInformationFile")==0) {
        const auto io=ctx.r4.u32,output=ctx.r5.u32,size=ctx.r6.u32,kind=ctx.r7.u32;
        if(!io || io%4 || !output || output%8){ctx.r3.u64=0xc000000d;return;}
        auto found=files.find(ctx.r3.u32);uint32_t status=0,count=0;
        if(found==files.end())status=0xc0000008;
        else if(kind==34) {
            if(size<56)status=0xc0000023;
            else {status=file_network_information(base,output,found->second->path);if(!status)count=56;}
        }else throw DiagnosticStop("Unsupported file information class "+std::to_string(kind));
        write32(base,io,status);write32(base,io+4,count);ctx.r3.u64=status;return;
    }
    if(std::strcmp(name,"NtCreateFile")==0 || std::strcmp(name,"NtOpenFile")==0) {
        const auto output=ctx.r3.u32,access=ctx.r4.u32,io=ctx.r6.u32;
        const bool create=std::strcmp(name,"NtCreateFile")==0;
        const auto disposition=create?ctx.r10.u32:1,options=create?read32(base,ctx.r1.u32+0x54):ctx.r7.u32;
        if(!output || output%4 || !io || io%4){ctx.r3.u64=0xc000000d;return;}
        auto finish=[&](uint32_t status,uint32_t handle=0){write32(base,output,handle);write32(base,io,status);write32(base,io+4,status?0:1);ctx.r3.u64=status;};
        const auto guest=object_path(base,ctx.r5.u32);const auto resolved=airborne::game_path(game_files_root,guest);
        STARTUP_PRINT("FILE_OPEN guest=%s native=%s access=%08x disposition=%u options=%08x rc=%08x\n",guest.c_str(),resolved.native.string().c_str(),access,disposition,options,resolved.status);
        if(resolved.status){finish(resolved.status);return;}
        // Game content is mounted read-only. Writable save/cache devices need
        // their own mount and must never modify extracted source assets.
        if(disposition!=1 || (access&0x500d0116u)){finish(0xc0000022);return;}
        if(options&1){finish(0xc00000ba);return;} // Directory handles not implemented.
        auto file=std::make_shared<NativeFile>();file->path=resolved.native.string();file->stream=std::fopen(file->path.c_str(),"rb");
        if(!file->stream){finish(0xc0000022);return;}
        const auto handle=next_handle;next_handle+=4;files.emplace(handle,file);
        STARTUP_PRINT("FILE_OPEN_COMPLETE handle=%08x native=%s\n",handle,file->path.c_str());finish(0,handle);return;
    }
    if(std::strcmp(name,"NtReadFile")==0) {
        auto found=files.find(ctx.r3.u32);const auto io=ctx.r7.u32,destination=ctx.r8.u32,bytes=ctx.r9.u32;
        if(!io || io%4 || (!destination && bytes) || uint64_t(destination)+bytes>0x100000000ull){ctx.r3.u64=0xc000000d;return;}
        auto finish=[&](uint32_t status,uint32_t count){write32(base,io,status);write32(base,io+4,count);ctx.r3.u64=status;};
        if(found==files.end()){finish(0xc0000008,0);return;}
        if(ctx.r4.u32 || ctx.r5.u32 || ctx.r6.u32)throw DiagnosticStop("Asynchronous file event/APC completion not implemented");
        auto& file=*found->second;uint64_t offset=UINT64_MAX-1;
        if(ctx.r10.u32){std::memcpy(&offset,ppc_guest_ptr(base,ctx.r10.u32),8);offset=__builtin_bswap64(offset);}
        int seek=0;
        if(offset!=UINT64_MAX-1) {
            if(offset>INT64_MAX){finish(0xc000000d,0);return;}
#ifdef __SWITCH__
            seek=fseeko(file.stream,off_t(offset),SEEK_SET);
#else
            seek=_fseeki64(file.stream,int64_t(offset),SEEK_SET);
#endif
        }
        if(seek){finish(0xc000000d,0);return;}
        const auto count=std::fread(ppc_guest_ptr(base,destination),1,bytes,file.stream);
        const auto status=std::ferror(file.stream)?0xc0000185u:(!count && bytes?0xc0000011u:0u);
        STARTUP_PRINT("FILE_READ handle=%08x native=%s offset=%016llx destination=%08x requested=%u transferred=%zu rc=%08x\n",ctx.r3.u32,file.path.c_str(),(unsigned long long)offset,destination,bytes,count,status);
        finish(status,uint32_t(count));return;
    }
    if(std::strcmp(name,"XexGetModuleSection")==0) {
        if(ctx.r3.u32!=module){ctx.r3.u64=0xc0000008;return;}
        if(!ctx.r4.u32 || !ctx.r5.u32 || !ctx.r6.u32 || ctx.r5.u32%4 || ctx.r6.u32%4){ctx.r3.u64=0xc000000d;return;}
        std::string name;for(unsigned i=0;i<9;++i){const auto c=*ppc_guest_ptr(base,ctx.r4.u32+i);if(!c)break;name+=char(c);}
        const auto resources=xex_header_field(base+header,0x4000,header,0x2ff);
        if(resources && name.size()<=8) {
            const auto bytes=read32(base,resources);
            if(bytes<4 || (bytes-4)%16)throw DiagnosticStop("Invalid XEX resource table size");
            for(uint32_t offset=4;offset<bytes;offset+=16) {
                const auto at=resources+offset;char resource_name[9]{};std::memcpy(resource_name,base+at,8);
                if(name!=resource_name)continue;
                const auto address=read32(base,at+8),size=read32(base,at+12);
                if(address<PPC_IMAGE_BASE || uint64_t(address)+size>PPC_IMAGE_BASE+PPC_IMAGE_SIZE)throw DiagnosticStop("XEX resource outside loaded image");
                write32(base,ctx.r5.u32,address);write32(base,ctx.r6.u32,size);
                STARTUP_PRINT("MODULE_SECTION name=%s address=%08x bytes=%u\n",name.c_str(),address,size);ctx.r3.u64=0;return;
            }
        }
        ctx.r3.u64=0xc0000225;return;
    }
    if(std::strcmp(name,"XexGetModuleHandle")==0) {
        if(!ctx.r4.u32 || ctx.r4.u32%4){ctx.r3.u64=0xc000000d;return;}
        bool executable=!ctx.r3.u32;
        if(ctx.r3.u32) {
            std::string name;
            for(unsigned i=0;i<256;++i){const auto c=*ppc_guest_ptr(base,ctx.r3.u32+i);if(!c)break;name+=char(c);}
            if(name.size()==256)throw DiagnosticStop("Unterminated module name");
            executable=airborne::ascii_lower(name)=="default.xex";
            STARTUP_PRINT("MODULE_LOOKUP name=%s present=%u\n",name.c_str(),executable);
        }
        write32(base,ctx.r4.u32,executable?module:0);ctx.r3.u64=executable?0:1168;return;
    }
    if(std::strcmp(name,"KeEnableFpuExceptions")==0) {
        if(ctx.r3.u32)throw DiagnosticStop("Guest FP exception delivery not implemented");
        // Every guest thread already masks native FP traps. The requested
        // disabled state does not require synthesizing guest exceptions.
        STARTUP_PRINT("GUEST_FP_EXCEPTIONS enabled=0 native_traps=MASKED\n");return;
    }
    if(std::strcmp(name,"RtlNtStatusToDosError")==0) {
        const uint32_t status=ctx.r3.u32;
        constexpr std::pair<uint32_t,uint32_t> errors[]={{0,0},{0xc000000f,2},{0xc0000034,2},{0xc000003a,3},
            {0xc0000022,5},{0xc0000008,6},{0xc0000017,8},{0xc000000d,87},{0xc0000033,123},
            {0xc0000225,1168},{0xc0000023,122},{0xc0000011,38},{0xc0000043,32},{0x80000006,18},{0xc0000002,120}};
        for(const auto& error:errors)if(error.first==status){ctx.r3.u64=error.second;return;}
        throw DiagnosticStop("Unmapped NTSTATUS to DOS error: "+std::to_string(status));
    }
    if(std::strcmp(name,"NtQueryFullAttributesFile")==0) {
        if(!ctx.r4.u32 || ctx.r4.u32%8){ctx.r3.u64=0xc000000d;return;}
        const auto guest=object_path(base,ctx.r3.u32);const auto resolved=airborne::game_path(game_files_root,guest);
        STARTUP_PRINT("FILE_QUERY_FULL guest=%s native=%s rc=%08x output=%08x bytes=56\n",guest.c_str(),resolved.native.string().c_str(),resolved.status,ctx.r4.u32);
        if(resolved.status){ctx.r3.u64=resolved.status;return;}
        ctx.r3.u64=file_network_information(base,ctx.r4.u32,resolved.native.string());return;
    }
    if(std::strcmp(name,"RtlInitAnsiString")==0) {
        const auto output=ctx.r3.u32,source=ctx.r4.u32;
        if(!output || output%4)throw DiagnosticStop("Invalid ANSI_STRING output");
        uint32_t length=0;
        if(source) {
            while(length<65535 && *ppc_guest_ptr(base,source+length))++length;
            if(length>=65535)throw DiagnosticStop("ANSI_STRING source exceeds representable length");
        }
        write32(base,output,(length<<16)|(source?length+1:0));write32(base,output+4,source);return;
    }
    if(std::strcmp(name,"NetDll_XNetStartup")==0) {
        // XNet configuration registration is distinct from opening sockets or
        // establishing a secure Xbox connection. Those operations have their
        // own imports and must not report success until actually implemented.
        if(ctx.r3.u32!=1 || !ctx.r4.u32 || *ppc_guest_ptr(base,ctx.r4.u32)!=13){ctx.r3.u64=10022;return;}
        std::array<uint8_t,13> parameters;std::memcpy(parameters.data(),ppc_guest_ptr(base,ctx.r4.u32),parameters.size());
        if(xnet_profile.references && parameters!=xnet_profile.parameters){ctx.r3.u64=10037;return;}
        xnet_profile.parameters=parameters;++xnet_profile.references;
        STARTUP_PRINT("XNET_PROFILE_REGISTER caller=1 source=%08x bytes=13 flags=%02x datagram_limit=%u stream_limit=%u references=%u secure_transport=NOT_IMPLEMENTED\n",
            ctx.r4.u32,parameters[1],parameters[2],parameters[3],xnet_profile.references);
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NetDll_XNetCleanup")==0) {
        if(ctx.r3.u32!=1 || !xnet_profile.references){ctx.r3.u64=10093;return;}
        if(!--xnet_profile.references)xnet_profile.parameters.fill(0);
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NetDll_XNetGetOpt")==0) {
        if(!xnet_profile.references){ctx.r3.u64=10093;return;}
        if(ctx.r3.u32!=1 || ctx.r4.u32!=1 || !ctx.r5.u32 || !ctx.r6.u32 || ctx.r6.u32%4){ctx.r3.u64=10022;return;}
        const auto size=read32(base,ctx.r6.u32);write32(base,ctx.r6.u32,13);
        if(size<13){ctx.r3.u64=10040;return;}
        std::memcpy(ppc_guest_ptr(base,ctx.r5.u32),xnet_profile.parameters.data(),13);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"XAudioGetVoiceCategoryVolumeChangeMask")==0) {
        if(ctx.r3.u32!=0x41550000 || !audio_client || !audio_client->active || !ctx.r4.u32 || ctx.r4.u32%4)
            {ctx.r3.u64=0x80070057;return;}
        // No dashboard category-volume controls exist in this runtime.
        write32(base,ctx.r4.u32,0);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"XAudioGetVoiceCategoryVolume")==0) {
        if(!ctx.r4.u32 || ctx.r4.u32%4){ctx.r3.u64=0x80070057;return;}
        write32(base,ctx.r4.u32,0x3f800000);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"XAudioGetSpeakerConfig")==0) {
        if(!ctx.r3.u32 || ctx.r3.u32%4){ctx.r3.u64=0x80070057;return;}
        write32(base,ctx.r3.u32,0x00010001);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"XAudioRegisterRenderDriverClient")==0) {
        if(!ctx.r3.u32 || ctx.r3.u32%4 || !ctx.r4.u32 || ctx.r4.u32%4){ctx.r3.u64=0x80070057;return;}
        const auto callback=read32(base,ctx.r3.u32),argument=read32(base,ctx.r3.u32+4);
        if(callback<PPC_CODE_BASE || callback>=PPC_CODE_BASE+PPC_CODE_SIZE || callback%4){ctx.r3.u64=0x80070057;return;}
        if(audio_client)throw DiagnosticStop("Multiple or re-registered audio clients unsupported");
        auto client=std::make_shared<AudioClient>();audio_client=client;client->output.open();
        auto thread=std::make_shared<HostThread>();thread->id=next_thread_id++;
        if(thread->id>=100)throw DiagnosticStop("Audio worker guest thread limit");
        thread->object=0x70030000+(thread->id-2)*0x10000;thread->pcr=thread->object+0x1000;thread->tls=thread->object+0x2000;
        thread->stack_low=0x60010000+(thread->id-2)*0x200000;thread->stack_high=thread->stack_low+0x40000;
        thread->entry=callback;thread->startup=0;thread->argument=thread->object+0x2800;thread->audio=true;
        if(!VirtualAlloc(base+thread->object,0x3000,MEM_COMMIT,PAGE_READWRITE) ||
           !VirtualAlloc(base+thread->stack_low,0x40000,MEM_COMMIT,PAGE_READWRITE))throw DiagnosticStop("Audio worker guest memory allocation failed");
        setup_thread_memory(base,thread->object,thread->pcr,thread->tls,thread->stack_low,thread->stack_high,thread->id,callback);
        write32(base,thread->argument,argument);
        thread->handle=CreateThread(nullptr,0,guest_thread_start,thread.get(),CREATE_SUSPENDED,nullptr);
        if(!thread->handle)throw DiagnosticStop("Audio worker native thread creation failed");
        const auto handle=next_handle;next_handle+=4;threads.emplace(handle,thread);audio_client=client;
        write32(base,ctx.r4.u32,0x41550000);
        STARTUP_PRINT("AUDIO_CLIENT_REGISTER driver=41550000 callback=%08x argument=%08x wrapper=%08x worker=%u frames_available=4\n",callback,argument,thread->argument,thread->id);
        if(ResumeThread(thread->handle)==DWORD(-1))throw DiagnosticStop("Audio worker resume failed");
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"XAudioSubmitRenderDriverFrame")==0) {
        if(ctx.r3.u32!=0x41550000 || !audio_client || !audio_client->active || !ctx.r4.u32 || ctx.r4.u32%4)
            throw DiagnosticStop("Invalid XAudio render frame submission");
        auto client=audio_client;const auto source=ctx.r4.u32;
        const auto pcm=airborne::stereo_pcm(ppc_guest_ptr(base,source));
        STARTUP_PRINT("AUDIO_FRAME_SUBMIT driver=%08x source=%08x guest_bytes=6144 frame=%llu\n",ctx.r3.u32,source,(unsigned long long)(client->frames+1));
        service_lock.unlock();client->output.play(pcm);service_lock.lock();
        ++client->frames;if(client->credits>=4)throw DiagnosticStop("Audio frame submitted without callback credit");
        ++client->credits;client->changed.notify_one();ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"XAudioUnregisterRenderDriverClient")==0) {
        if(ctx.r3.u32!=0x41550000 || !audio_client || !audio_client->active){ctx.r3.u64=0x80070057;return;}
        audio_client->active=false;audio_client->changed.notify_all();ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"XMACreateContext")==0) {
        if(!ctx.r3.u32 || ctx.r3.u32%4){ctx.r3.u64=0xc000000d;return;}
        xma_ensure_table();
        for(unsigned i=0;i<320;++i)if(!xma_contexts[i]) {
            xma_contexts.set(i);const auto context=xma_context_table+i*64;
            std::memset(ppc_guest_ptr(base,context),0,64);write32(base,ctx.r3.u32,context);ctx.r3.u64=0;return;
        }
        write32(base,ctx.r3.u32,0);ctx.r3.u64=0xc0000017;return;
    }
    if(std::strcmp(name,"XMAReleaseContext")==0) {
        const auto context=ctx.r3.u32;
        if(!xma_context_table || context<xma_context_table || context-xma_context_table>=320*64 || (context-xma_context_table)%64 ||
            !xma_contexts[(context-xma_context_table)/64]){ctx.r3.u64=0xc000000d;return;}
        xma_contexts.reset((context-xma_context_table)/64);std::memset(ppc_guest_ptr(base,context),0,64);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"KeAcquireSpinLockAtRaisedIrql")==0 || std::strcmp(name,"KeReleaseSpinLockFromRaisedIrql")==0 ||
        std::strcmp(name,"KfAcquireSpinLock")==0 || std::strcmp(name,"KfReleaseSpinLock")==0) {
        const bool kf=std::strncmp(name,"Kf",2)==0,release=std::strstr(name,"Release")!=nullptr;
        auto* irql=ppc_guest_ptr(base,ctx.r13.u32+0x18);const uint8_t old_irql=*irql;
        if(ctx.r3.u32%4 || !ctx.r13.u32 || old_irql>2 || (!kf && old_irql<2) || (release && kf && ctx.r4.u32>old_irql))
            throw DiagnosticStop("Invalid spinlock address/IRQL");
        auto* pointer=reinterpret_cast<uint32_t*>(ppc_guest_ptr(base,ctx.r3.u32));
        const uint32_t self=__builtin_bswap32(ctx.r13.u32);
        if(release) {
            if(__atomic_load_n(pointer,__ATOMIC_ACQUIRE)!=self)throw DiagnosticStop("Releasing guest spinlock owned by another PCR");
            __atomic_store_n(pointer,0u,__ATOMIC_RELEASE);if(kf)*irql=ctx.r4.u8;return;
        }
        if(__atomic_load_n(pointer,__ATOMIC_ACQUIRE)==self)throw DiagnosticStop("Guest spinlock self-deadlock");
        if(kf)*irql=2;
        service_lock.unlock();
        const auto started=std::chrono::steady_clock::now();
        try {
        for(;;) {
            uint32_t expected=0;
            if(__atomic_compare_exchange_n(pointer,&expected,self,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))break;
#ifdef __SWITCH__
            if(nx_runtime_stopping())throw ThreadExit{0};
#endif
            if(std::chrono::steady_clock::now()-started>std::chrono::seconds(3))throw DiagnosticStop("Guest spinlock wait exceeded diagnostic deadline");
            Sleep(0);
        }
        }catch(...){if(kf)*irql=old_irql;throw;}
        if(kf)ctx.r3.u64=old_irql;
        return;
    }
    if(std::strcmp(name,"RtlFillMemoryUlong")==0) {
        if(ctx.r3.u32%4 || ctx.r4.u32%4 || uint64_t(ctx.r3.u32)+ctx.r4.u32>0x100000000ull)
            throw DiagnosticStop("Invalid RtlFillMemoryUlong alignment/length");
        for(uint32_t offset=0;offset<ctx.r4.u32;offset+=4)write32(base,ctx.r3.u32+offset,ctx.r5.u32);
        return;
    }
    if(std::strcmp(name,"VdInitializeEngines")==0) {
        if(graphics_device && graphics_device->ready())throw DiagnosticStop("Graphics engines already initialized");
        graphics_zero_depth.valid=false;graphics_movie_color.rgba.clear();graphics_front_buffer=UINT32_MAX;
        graphics_device.reset(new airborne::VulkanDevice([](const char* line){STARTUP_PRINT("%s\n",line);STARTUP_FLUSH();}));
        try{graphics_device->initialize(1280,720);}
        catch(const std::exception& error){throw DiagnosticStop(std::string("Native Vulkan initialization: ")+error.what());}
        graphics_ring.reset(new airborne::CommandRing(gpu_read_raw,gpu_write_raw,[](const char* line){STARTUP_PRINT("%s\n",line);STARTUP_FLUSH();}));
        graphics_ring->set_present(gpu_present);
        graphics_ring->set_clear(gpu_startup_clear);
        STARTUP_PRINT("GPU_ENGINES flags=%08x callback=%08x argument=%08x pfp=%08x me=%08x command_processor=PM4_PARTIAL\n",
            ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32);
        ctx.r3.u64=1;return;
    }
    if(std::strcmp(name,"VdInitializeScalerCommandBuffer")==0) {
        const auto destination=read32(base,ctx.r1.u32+0x64),count=read32(base,ctx.r1.u32+0x6c);
        // Native presentation currently performs an exact 1280x720 copy. The
        // observed identity scaler requires no Xenos hardware configuration.
        // Reject scaling/cropping rather than silently discarding that work.
        if(ctx.r3.u32 || ctx.r5.u32 || ctx.r4.u32!=0x050002d0 || ctx.r6.u32!=ctx.r4.u32 || ctx.r7.u32!=ctx.r4.u32 ||
            !destination || destination%4 || !count || count>4096 || uint64_t(destination)+count*4>0x100000000ull)
            throw DiagnosticStop("Unsupported Vd scaler geometry/command space");
        for(uint32_t i=0;i<count;++i)write32(base,destination+4*i,0x80000000);
        STARTUP_PRINT("GPU_SCALER mode=NATIVE_IDENTITY width=1280 height=720 reserved_nops=%u\n",count);
        ctx.r3.u64=count;return;
    }
    if(std::strcmp(name,"VdPersistDisplay")==0) {
        // Persisting a VI image across title/application teardown is unsupported.
        // The XDK caller explicitly handles FALSE without freeing an output.
        if(ctx.r4.u32)write32(base,ctx.r4.u32,0);
        STARTUP_PRINT("GPU_PERSIST_DISPLAY result=FALSE reason=CROSS_TITLE_RETENTION_UNSUPPORTED\n");
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"VdShutdownEngines")==0) {
        graphics_ring.reset();
        if(graphics_device)graphics_device->shutdown();
        graphics_interrupt_callback=0;graphics_interrupt_argument=0;return;
    }
    if(std::strcmp(name,"VdSetSystemCommandBufferGpuIdentifierAddress")==0) {
        // The ring tracks completion; the system command buffer itself is not
        // returned until its submission/retirement protocol is implemented.
        const uint32_t pointer=ctx.r3.u32;
        if(pointer && (!physical_allocator || !physical_allocator->contains(GuestPhysicalAllocator::physical(pointer),4,true)))
            throw DiagnosticStop("Invalid system GPU identifier address");
        graphics_system_identifier_address=pointer;return;
    }
    if(std::strcmp(name,"VdGetSystemCommandBuffer")==0) {
        if(!graphics_ring || !graphics_device || !graphics_device->ready())
            throw DiagnosticStop("System command buffer requires initialized graphics");
        // HLE-owned descriptor and monotonically increasing lease. The title
        // passes these values back to VdSwap; neither value is a GPU address.
        if(!ctx.r3.u32 || !ctx.r4.u32 || ctx.r3.u32%4 || ctx.r4.u32%4)
            throw DiagnosticStop("Invalid system command buffer outputs");
        if(++graphics_system_generation==0)throw DiagnosticStop("System command buffer lease exhausted");
        std::memset(ppc_guest_ptr(base,ctx.r3.u32),0,0x94);
        write32(base,module_variable+0x300,graphics_system_generation);
        write32(base,ctx.r3.u32,module_variable+0x300);
        write32(base,ctx.r4.u32,graphics_system_generation);
        STARTUP_PRINT("GPU_SYSTEM_BUFFER descriptor=%08x lease=%u output=%08x\n",module_variable+0x300,graphics_system_generation,ctx.r3.u32);
        return;
    }
    if(std::strcmp(name,"VdSwap")==0) {
        if(read32(base,ctx.r6.u32)!=module_variable+0x300 || !graphics_system_generation || ctx.r7.u32!=graphics_system_generation)
            throw DiagnosticStop("Invalid or stale system command buffer lease");
        const uint32_t width_pointer=read32(base,ctx.r1.u32+0x54),height_pointer=read32(base,ctx.r1.u32+0x5c);
        uint32_t fetch[6];for(unsigned i=0;i<6;++i)fetch[i]=read32(base,ctx.r4.u32+4*i);
        STARTUP_PRINT("GPU_SWAP_REQUEST command_buffer=%08x fetch=%08x identifier=%08x frontbuffer=%08x format=%u colorspace=%u width=%u height=%u pitch=%u tiled=%u endian=%u fetch_words=%08x,%08x,%08x,%08x,%08x,%08x\n",
            ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,read32(base,ctx.r8.u32),read32(base,ctx.r9.u32),read32(base,ctx.r10.u32),
            read32(base,width_pointer),read32(base,height_pointer),((fetch[0]>>22)&511)*32,fetch[0]>>31,(fetch[1]>>6)&3,
            fetch[0],fetch[1],fetch[2],fetch[3],fetch[4],fetch[5]);
        const auto width=read32(base,width_pointer),height=read32(base,height_pointer),guest=fetch[1]&0xfffff000;
        if(read32(base,ctx.r8.u32)!=guest || read32(base,ctx.r9.u32)!=6 || read32(base,ctx.r10.u32)!=0 ||
            width!=(fetch[2]&8191)+1 || height!=((fetch[2]>>13)&8191)+1)
            throw DiagnosticStop("Unsupported or inconsistent VdSwap geometry/format/colorspace");
        const auto physical=GuestPhysicalAllocator::physical(guest);
        // A/C/E aliases differ; E pointers have an additional 4 KiB bias.
        // That bias is representable in the fetch's 4 KiB base-address field.
        if(physical==UINT32_MAX || physical%4096)throw DiagnosticStop("Invalid VdSwap physical address");
        graphics_front_buffer=physical;
        fetch[1]=physical|(fetch[1]&4095);
        const auto commands=GuestPhysicalAllocator::physical(ctx.r3.u32);
        if(!physical_allocator->contains(commands,256,true))throw DiagnosticStop("VdSwap command space outside writable physical allocation");
        // Do not display at import time: preserve ordering with preceding PM4
        // work by emitting a checked HLE packet into the title's reserved space.
        write32(base,ctx.r3.u32,0x00054800);
        for(unsigned i=0;i<6;++i)write32(base,ctx.r3.u32+4*(i+1),fetch[i]);
        const uint32_t display[]={0xc0036400,0x41495258,physical,width,height};
        for(unsigned i=0;i<5;++i)write32(base,ctx.r3.u32+28+4*i,display[i]);
        for(unsigned i=12;i<64;++i)write32(base,ctx.r3.u32+4*i,0x80000000);
        STARTUP_PRINT("GPU_SWAP_EMIT physical=%08x bytes=256 frontbuffer=%08x completion=AT_PACKET_EXECUTION\n",commands,physical);
        return;
    }
    if(std::strcmp(name,"VdInitializeRingBuffer")==0) {
        if(!graphics_ring || ctx.r4.u32>23)throw DiagnosticStop("Graphics ring unavailable/invalid size");
        const uint32_t bytes=1u<<(ctx.r4.u32+3);
        if(!physical_allocator->contains(ctx.r3.u32,bytes))throw DiagnosticStop("Graphics ring outside live physical allocation");
        graphics_ring->initialize(ctx.r3.u32,ctx.r4.u32);return;
    }
    if(std::strcmp(name,"VdEnableRingBufferRPtrWriteBack")==0) {
        if(!graphics_ring)throw DiagnosticStop("Graphics ring unavailable");
        graphics_ring->enable_writeback(ctx.r3.u32,ctx.r4.u32);return;
    }
    if(std::strcmp(name,"VdRetrainEDRAMWorker")==0 || std::strcmp(name,"VdRetrainEDRAM")==0) {
        if(!graphics_device || !graphics_device->ready())throw DiagnosticStop("Native graphics memory unavailable");
        if(ctx.r3.u32>1)throw DiagnosticStop("Unknown EDRAM retraining mode");
        try{graphics_device->synchronize();}catch(const std::exception& error){throw DiagnosticStop(error.what());}
        // NVK owns Nvidia memory/link training; there is no physical Xenos
        // EDRAM to retrain and no guest command list to emit. Zero means the
        // caller's command-generation retry loop is finished, not draw success.
        STARTUP_PRINT("GPU_MEMORY_RETRAIN backend=native-driver xenos_commands=0\n");
        if(std::strcmp(name,"VdRetrainEDRAM")==0 && ctx.r6.u32)write32(base,ctx.r6.u32,0);
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"KiApcNormalRoutineNop")==0) {
        // This import slot is invoked by this XDK's D3D with its 19-word
        // ME_INIT command, not as an APC. Verify the observed contract before
        // accepting this compatibility alias.
        if(ctx.r4.u32!=19 || read32(base,ctx.r3.u32)!=0xc0114800)
            throw DiagnosticStop("Unrecognized KiApcNormalRoutineNop / ME_INIT verification contract");
        STARTUP_PRINT("GPU_ME_INIT_VERIFY words=19 header=c0114800\n");return;
    }
    if(std::strcmp(name,"VdSetGraphicsInterruptCallback")==0) {
        const auto callback=ctx.r3.u32;
        if(callback && (callback<PPC_CODE_BASE || callback>=PPC_CODE_BASE+PPC_CODE_SIZE || callback%4))
            throw DiagnosticStop("Invalid graphics interrupt callback");
        graphics_interrupt_callback=callback;graphics_interrupt_argument=ctx.r4.u32;return;
    }
    if(std::strcmp(name,"ExRegisterTitleTerminateNotification")==0) {
        const auto routine=read32(base,ctx.r3.u32),priority=read32(base,ctx.r3.u32+4);
        if(ctx.r4.u32) {
            if(routine<PPC_CODE_BASE || routine>=PPC_CODE_BASE+PPC_CODE_SIZE || routine%4)
                throw DiagnosticStop("Invalid title termination callback");
            title_terminate_notifications[routine]=priority;
        }else title_terminate_notifications.erase(routine);
        // Registration only. A diagnostic stop is not normal title termination.
        return;
    }
    if(std::strcmp(name,"ExGetXConfigSetting")==0) {
        // Declared English/US, UTC without DST, 720p VGA profile, independent of
        // the native OS locale. Country is a byte; language/video are dwords.
        if(ctx.r3.u32!=3 || (ctx.r4.u32!=10 && ctx.r4.u32!=9 && ctx.r4.u32!=14 && ctx.r4.u32!=12 && (ctx.r4.u32<1 || ctx.r4.u32>7)))throw DiagnosticStop("Unsupported XConfig category/setting");
        const uint16_t bytes=ctx.r4.u32==14?1:4;
        const uint32_t value=ctx.r4.u32==12?0x40:ctx.r4.u32<=7?0:ctx.r4.u32==14?103:ctx.r4.u32==9?1:0x40000;
        if(ctx.r7.u32){const auto size=__builtin_bswap16(bytes);std::memcpy(ppc_guest_ptr(base,ctx.r7.u32),&size,2);}
        if(!ctx.r5.u32){ctx.r3.u64=ctx.r6.u32?0xc00000f1:0;return;}
        if(ctx.r6.u32<bytes){ctx.r3.u64=0xc0000023;return;}
        if(bytes==1)*ppc_guest_ptr(base,ctx.r5.u32)=uint8_t(value);else write32(base,ctx.r5.u32,value);
        STARTUP_PRINT("XCONFIG_PROFILE setting=%u value=%08x bytes=%u\n",ctx.r4.u32,value,bytes);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"RtlImageXexHeaderField")==0) {
        if(ctx.r3.u32!=header)throw DiagnosticStop("Unknown XEX header pointer");
        ctx.r3.u64=xex_header_field(base+header,0x4000,header,ctx.r4.u32);
        return;
    }
    if(std::strcmp(name,"KeGetCurrentProcessType")==0) {ctx.r3.u64=1;return;}
    if(std::strcmp(name,"KeRaiseIrqlToDpcLevel")==0) {
        const auto at=ctx.r13.u32+0x18;const uint8_t old=*ppc_guest_ptr(base,at);
        if(old>2)throw DiagnosticStop("IRQL raise below current level");
        *ppc_guest_ptr(base,at)=2;ctx.r3.u64=old;return;
    }
    if(std::strcmp(name,"KfLowerIrql")==0) {
        const auto at=ctx.r13.u32+0x18;
        if(ctx.r3.u32>*ppc_guest_ptr(base,at))throw DiagnosticStop("IRQL lower above current level");
        *ppc_guest_ptr(base,at)=uint8_t(ctx.r3.u32);return;
    }
    if(std::strcmp(name,"KeEnterCriticalRegion")==0 || std::strcmp(name,"KeLeaveCriticalRegion")==0) {
        const uint32_t at=current_thread_address+0xb0,value=read32(base,at);
        if(std::strcmp(name,"KeEnterCriticalRegion")==0) {
            if(value==0x80000000)throw DiagnosticStop("Kernel APC disable nesting overflow");
            write32(base,at,value-1);
        }else {
            if(value==0)throw DiagnosticStop("Unbalanced KeLeaveCriticalRegion");
            write32(base,at,value+1);
        }
        return; // Normal APC delivery is disabled; alertable waits still reject.
    }
    if(std::strcmp(name,"MmAllocatePhysicalMemoryEx")==0 || std::strcmp(name,"MmAllocatePhysicalMemory")==0) {
        if(!physical_allocator)throw DiagnosticStop("Physical allocator unavailable");
        if(ctx.r3.u32>2)throw DiagnosticStop("Unknown physical allocation class");
        const bool extended=std::strcmp(name,"MmAllocatePhysicalMemoryEx")==0;
        ctx.r3.u64=physical_allocator->allocate(ctx.r4.u32,ctx.r5.u32,extended ? ctx.r6.u32:0,extended ? ctx.r7.u32:0xffffffff,extended ? ctx.r8.u32:0);
        STARTUP_PRINT("PHYSICAL_RESULT address=0x%08x\n",ctx.r3.u32);return;
    }
    if(std::strcmp(name,"MmFreePhysicalMemory")==0) {
        if(!physical_allocator->free(ctx.r4.u32))throw DiagnosticStop("Invalid physical free");return;
    }
    if(std::strcmp(name,"MmGetPhysicalAddress")==0) {
        ctx.r3.u64=GuestPhysicalAllocator::physical(ctx.r3.u32);
        if(ctx.r3.u32==UINT32_MAX)throw DiagnosticStop("Physical address requested for virtual pointer");return;
    }
    if(std::strcmp(name,"MmSetAddressProtect")==0) {
        if(ctx.r3.u32<0xa0000000) {
            uint32_t address=ctx.r3.u32,bytes=ctx.r4.u32,old=0;
            if(allocator->protect(address,bytes,ctx.r5.u32,old))throw DiagnosticStop("Virtual MmSetAddressProtect rejected");
        }else if(!physical_allocator->protect(ctx.r3.u32,ctx.r4.u32,ctx.r5.u32))
            throw DiagnosticStop("Physical MmSetAddressProtect rejected");
        STARTUP_PRINT("ADDRESS_PROTECT guest=%08x bytes=%08x protection=%08x cache_policy=host-managed\n",ctx.r3.u32,ctx.r4.u32,ctx.r5.u32);return;
    }
    if(std::strcmp(name,"KeQueryPerformanceFrequency")==0) {ctx.r3.u64=PPC_TIMEBASE_FREQUENCY;return;}
    if(std::strcmp(name,"XamGetSystemVersion")==0) {
        // Declared virtual XAM ABI, separate from Horizon's version. This
        // baseline exposes no later dynamically resolved XAM extensions.
        STARTUP_PRINT("XAM_ABI_VERSION value=20076000 profile=LEGACY_2_0_1888_0\n");
        ctx.r3.u64=0x20076000;return;
    }
    if(std::strcmp(name,"XamLoaderGetLaunchDataSize")==0) {
        if(!ctx.r3.u32){ctx.r3.u64=87;return;}
        write32(base,ctx.r3.u32,0);ctx.r3.u64=1168;return; // Direct launch: no launch payload.
    }
    if(std::strcmp(name,"XamLoaderGetLaunchData")==0) {ctx.r3.u64=1168;return;}
    if(std::strcmp(name,"FscSetCacheElementCount")==0) {
        // Advisory cache tuning only. Actual file reads use the host's cache;
        // Xbox cache capacity/eviction behavior is not emulated.
        if(ctx.r3.u32!=0)throw DiagnosticStop("Unknown filesystem-cache mode");
        STARTUP_PRINT("HOST_CACHE_HINT elements=%u (host-managed cache)\n",ctx.r4.u32);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NtCreateEvent")==0) {
        if(ctx.r4.u32)throw DiagnosticStop("Named event attributes unsupported");
        if(!ctx.r3.u32 || ctx.r5.u32>1){ctx.r3.u64=0xc000000d;return;}
        const bool manual=ctx.r5.u32==0,signalled=ctx.r6.u32!=0;
        auto event=std::make_shared<HostEvent>();event->manual=manual;event->signalled=signalled;
        const auto handle=next_handle;next_handle+=4;events.emplace(handle,event);
        write32(base,ctx.r3.u32,handle);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NtCreateSemaphore")==0) {
        const auto output=ctx.r3.u32,count=ctx.r5.u32,limit=ctx.r6.u32;
        if(ctx.r4.u32)throw DiagnosticStop("Named semaphore attributes unsupported");
        if(!output || output%4 || !limit || limit>0x7fffffff || count>limit){ctx.r3.u64=0xc000000d;return;}
        auto semaphore=std::make_shared<HostSemaphore>();semaphore->count=count;semaphore->limit=limit;
        const auto handle=next_handle;next_handle+=4;semaphore_handles.emplace(handle,semaphore);
        write32(base,output,handle);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NtReleaseSemaphore")==0) {
        const auto found=semaphore_handles.find(ctx.r3.u32);const auto adjustment=ctx.r4.s32;
        if(found==semaphore_handles.end()){ctx.r3.u64=0xc0000008;return;}
        if(adjustment<=0 || (ctx.r5.u32 && ctx.r5.u32%4)){ctx.r3.u64=0xc000000d;return;}
        auto semaphore=found->second;
        if(uint32_t(adjustment)>semaphore->limit-semaphore->count){ctx.r3.u64=0xc0000047;return;}
        if(ctx.r5.u32)write32(base,ctx.r5.u32,semaphore->count);
        semaphore->count+=adjustment;semaphore->changed.notify_all();dispatcher_changed.notify_all();ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"KeInitializeSemaphore")==0) {
        const uint32_t object=ctx.r3.u32,count=ctx.r4.u32,limit=ctx.r5.u32;
        if(!object || object%4 || !limit || limit>0x7fffffff || count>limit)
            throw DiagnosticStop("Invalid kernel semaphore initialization");
        if(semaphore_objects.count(object))throw DiagnosticStop("Reinitializing live kernel semaphore unsupported");
        auto semaphore=std::make_shared<HostSemaphore>();semaphore->count=count;semaphore->limit=limit;
        std::memset(ppc_guest_ptr(base,object),0,20);*ppc_guest_ptr(base,object)=5;
        write32(base,object+4,count);write32(base,object+8,object+8);write32(base,object+12,object+8);write32(base,object+16,limit);
        semaphore_objects.emplace(object,semaphore);return;
    }
    if(std::strcmp(name,"KeReleaseSemaphore")==0) {
        auto found=semaphore_objects.find(ctx.r3.u32);
        if(found==semaphore_objects.end())throw DiagnosticStop("Unknown kernel semaphore release");
        if(ctx.r6.u32)throw DiagnosticStop("Semaphore wait-next IRQL handoff unsupported");
        auto semaphore=found->second;const uint32_t adjustment=ctx.r5.u32;
        if(!adjustment || adjustment>semaphore->limit-semaphore->count)
            throw DiagnosticStop("Kernel semaphore limit exceeded or invalid adjustment");
        const auto previous=semaphore->count;semaphore->count+=adjustment;
        write32(base,ctx.r3.u32+4,semaphore->count);semaphore->changed.notify_all();dispatcher_changed.notify_all();ctx.r3.u64=previous;return;
    }
    if(std::strcmp(name,"KeSetEvent")==0 || std::strcmp(name,"KeResetEvent")==0) {
        const uint32_t object=ctx.r3.u32;
        const bool set=std::strcmp(name,"KeSetEvent")==0;
        if(set && ctx.r5.u32)throw DiagnosticStop("KeSetEvent wait-next IRQL handoff unsupported");
        auto event=kernel_event(base,object);
        const bool previous=event->signalled;event->signalled=set;write32(base,object+4,set?1:0);
        if(set){if(event->manual)event->changed.notify_all();else event->changed.notify_one();dispatcher_changed.notify_all();}
        ctx.r3.u64=previous;return;
    }
    if(std::strcmp(name,"KeWaitForMultipleObjects")==0) {
        const auto count=ctx.r3.u32,list=ctx.r4.u32,type=ctx.r5.u32;
        if(!count || count>64 || !list || list%4 || type>1 || ctx.r8.u32)
            throw DiagnosticStop("Unsupported multiple dispatcher wait parameters");
        const auto millis=wait_milliseconds(base,ctx.r9.u32);
        std::vector<uint32_t> objects;std::vector<std::shared_ptr<HostEvent>> held_events;
        std::vector<std::shared_ptr<HostSemaphore>> held_semaphores;
        for(unsigned i=0;i<count;++i) {
            const auto object=read32(base,list+i*4);
            if(std::find(objects.begin(),objects.end(),object)!=objects.end())throw DiagnosticStop("Duplicate multiple-wait dispatcher object");
            objects.push_back(object);auto sem=semaphore_objects.find(object);
            held_semaphores.push_back(sem==semaphore_objects.end()?nullptr:sem->second);
            held_events.push_back(sem==semaphore_objects.end()?kernel_event(base,object):nullptr);
            STARTUP_PRINT("DISPATCHER_WAIT_OBJECT index=%u object=%08x type=%u\n",i,object,*ppc_guest_ptr(base,object));
        }
        auto available=[&](unsigned i){return held_semaphores[i]?held_semaphores[i]->count!=0:held_events[i]->signalled;};
        auto ready=[&]{
#ifdef __SWITCH__
            if(nx_runtime_stopping())return true;
#endif
            if(type==0){for(unsigned i=0;i<count;++i)if(!available(i))return false;return true;}
            for(unsigned i=0;i<count;++i)if(available(i))return true;return false;
        };
        bool acquired;
        if(millis==INFINITE){dispatcher_changed.wait(service_lock,ready);acquired=true;}
        else acquired=dispatcher_changed.wait_for(service_lock,std::chrono::milliseconds(millis),ready);
#ifdef __SWITCH__
        if(nx_runtime_stopping())throw ThreadExit{0};
#endif
        if(!acquired){ctx.r3.u64=0x102;return;}
        unsigned selected=0;
        if(type==1)while(selected<count && !available(selected))++selected;
        for(unsigned i=0;i<count;++i)if(type==0 || i==selected) {
            if(held_semaphores[i])write32(base,objects[i]+4,--held_semaphores[i]->count);
            else if(!held_events[i]->manual){held_events[i]->signalled=false;write32(base,objects[i]+4,0);}
        }
        ctx.r3.u64=selected;return;
    }
    if(std::strcmp(name,"KeWaitForSingleObject")==0) {
        if(ctx.r6.u32)throw DiagnosticStop("Alertable kernel dispatcher wait unsupported");
        const uint32_t object=ctx.r3.u32;
        const uint64_t millis=wait_milliseconds(base,ctx.r7.u32);
        for(const auto& pair:threads)if(pair.second->object==object) {
            auto held=pair.second;service_lock.unlock();
            const auto status=WaitForSingleObject(held->handle,DWORD(std::min<uint64_t>(millis,INFINITE)));
            ctx.r3.u64=status==WAIT_OBJECT_0?0:status==WAIT_TIMEOUT?0x102:0xc0000001;return;
        }
        auto found_semaphore=semaphore_objects.find(object);
        if(found_semaphore!=semaphore_objects.end()) {
            auto semaphore=found_semaphore->second;
            auto ready=[&]{return semaphore->count!=0
#ifdef __SWITCH__
                || nx_runtime_stopping()
#endif
                ;};
            bool acquired;
            if(millis==INFINITE){semaphore->changed.wait(service_lock,ready);acquired=true;}
            else acquired=semaphore->changed.wait_for(service_lock,std::chrono::milliseconds(millis),ready);
#ifdef __SWITCH__
            if(nx_runtime_stopping())throw ThreadExit{0};
#endif
            if(acquired){--semaphore->count;write32(base,object+4,semaphore->count);}
            ctx.r3.u64=acquired?0:0x102;return;
        }
        auto event=kernel_event(base,object);
        auto ready=[&]{return event->signalled
#ifdef __SWITCH__
            || nx_runtime_stopping()
#endif
            ;};
        bool signalled;
        if(millis==INFINITE){event->changed.wait(service_lock,ready);signalled=true;}
        else signalled=event->changed.wait_for(service_lock,std::chrono::milliseconds(millis),ready);
#ifdef __SWITCH__
        if(nx_runtime_stopping())throw ThreadExit{0};
#endif
        if(signalled && !event->manual){event->signalled=false;write32(base,object+4,0);}
        ctx.r3.u64=signalled?0:0x102;return;
    }
    if(std::strcmp(name,"NtSetEvent")==0 || std::strcmp(name,"NtResetEvent")==0 || std::strcmp(name,"NtClearEvent")==0) {
        auto found=events.find(ctx.r3.u32);
        if(found==events.end()){ctx.r3.u64=0xc0000008;return;}
        auto& event=*found->second;
        if(ctx.r4.u32 && std::strcmp(name,"NtClearEvent"))write32(base,ctx.r4.u32,event.signalled);
        const bool set=std::strcmp(name,"NtSetEvent")==0;
        event.signalled=set;
        if(set) {if(event.manual)event.changed.notify_all();else event.changed.notify_one();dispatcher_changed.notify_all();}
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NtWaitForSingleObjectEx")==0) {
        auto found=events.find(ctx.r3.u32);
        auto found_semaphore=semaphore_handles.find(ctx.r3.u32);
        auto thread=threads.find(ctx.r3.u32);
        if(thread!=threads.end() && thread->second->closed)thread=threads.end();
        if(found==events.end() && thread==threads.end() && found_semaphore==semaphore_handles.end()){ctx.r3.u64=0xc0000008;return;}
        if(ctx.r5.u32)throw DiagnosticStop("Alertable wait/APC unsupported");
        uint64_t millis=INFINITE;
        if(ctx.r6.u32) {
            uint64_t raw;std::memcpy(&raw,ppc_guest_ptr(base,ctx.r6.u32),8);raw=__builtin_bswap64(raw);
            if(raw>>63)millis=((~raw+1)+9999)/10000;
            else {
                FILETIME now;GetSystemTimeAsFileTime(&now);
                const auto ticks=(uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime;
                millis=raw>ticks ? (raw-ticks+9999)/10000 : 0;
            }
        }
        if(thread!=threads.end()) {
            auto held=thread->second;service_lock.unlock();
            const auto result=WaitForSingleObject(held->handle,DWORD(std::min<uint64_t>(millis,INFINITE)));
            ctx.r3.u64=result==WAIT_OBJECT_0 ? 0 : result==WAIT_TIMEOUT ? 0x102 : 0xc0000001;return;
        }
        if(found_semaphore!=semaphore_handles.end()) {
            auto semaphore=found_semaphore->second;
            auto ready=[&]{return semaphore->count!=0 || imports_stopping
#ifdef __SWITCH__
                || nx_runtime_stopping()
#endif
                ;};
            const bool acquired=millis==INFINITE?(semaphore->changed.wait(service_lock,ready),true):semaphore->changed.wait_for(service_lock,std::chrono::milliseconds(millis),ready);
            if(imports_stopping)throw ThreadExit{0};
#ifdef __SWITCH__
            if(nx_runtime_stopping())throw ThreadExit{0};
#endif
            if(acquired)--semaphore->count;
            ctx.r3.u64=acquired?0:0x102;return;
        }
        auto event=found->second;bool signalled;
        if(millis==INFINITE) {event->changed.wait(service_lock,[&]{return event->signalled
#ifdef __SWITCH__
            || nx_runtime_stopping()
#endif
            ;});signalled=true;}
        else signalled=event->changed.wait_for(service_lock,std::chrono::milliseconds(millis),[&]{return event->signalled
#ifdef __SWITCH__
            || nx_runtime_stopping()
#endif
            ;});
#ifdef __SWITCH__
        if(nx_runtime_stopping())throw ThreadExit{0};
#endif
        if(signalled && !event->manual)event->signalled=false;
        ctx.r3.u64=signalled ? 0:0x102;return;
    }
    if(std::strcmp(name,"NtClose")==0) {
        if(semaphore_handles.erase(ctx.r3.u32)){ctx.r3.u64=0;return;}
        auto file=files.find(ctx.r3.u32);
        if(file!=files.end()) {
            const int rc=std::fclose(file->second->stream);file->second->stream=nullptr;
            STARTUP_PRINT("FILE_CLOSE handle=%08x native_rc=%d\n",ctx.r3.u32,rc);files.erase(file);ctx.r3.u64=rc?0xc0000185:0;return;
        }
        auto found=events.find(ctx.r3.u32);
        if(found!=events.end()){events.erase(found);ctx.r3.u64=0;return;}
        // Thread handles remain retained until process exit so a closed handle
        // cannot invalidate the backing state while the guest thread is running.
        auto thread=threads.find(ctx.r3.u32);
        if(thread!=threads.end() && !thread->second->closed){thread->second->closed=true;ctx.r3.u64=0;return;}
        ctx.r3.u64=0xc0000008;return;
    }
    if(std::strcmp(name,"ExCreateThread")==0) {
        if(!ctx.r3.u32){ctx.r3.u64=0xc000000d;return;}
        const uint64_t stack=uint64_t(ctx.r4.u32 ? ctx.r4.u32 : stack_size)+0xfff;
        if(stack>0x1e0000 || next_thread_id>=100) {ctx.r3.u64=0xc0000017;return;}
        auto thread=std::make_shared<HostThread>();thread->id=next_thread_id++;
        thread->object=0x70030000+(thread->id-2)*0x10000;
        thread->pcr=thread->object+0x1000;thread->tls=thread->object+0x2000;
        thread->stack_low=0x60010000+(thread->id-2)*0x200000;
        thread->stack_high=thread->stack_low+uint32_t(stack&~0xfffull);
        thread->startup=ctx.r6.u32;thread->entry=ctx.r7.u32;thread->argument=ctx.r8.u32;
        if(!VirtualAlloc(base+thread->object,0x3000,MEM_COMMIT,PAGE_READWRITE) ||
           !VirtualAlloc(base+thread->stack_low,thread->stack_high-thread->stack_low,MEM_COMMIT,PAGE_READWRITE)) {
            ctx.r3.u64=0xc0000017;return;
        }
        setup_thread_memory(base,thread->object,thread->pcr,thread->tls,thread->stack_low,thread->stack_high,thread->id,thread->entry);
        base[thread->pcr+0x10c]=uint8_t((ctx.r9.u32>>24)%6);
        thread->handle=CreateThread(nullptr,0,guest_thread_start,thread.get(),CREATE_SUSPENDED,nullptr);
        if(!thread->handle){ctx.r3.u64=0xc0000017;return;}
        const uint32_t handle=next_handle;next_handle+=4;threads.emplace(handle,thread);
        write32(base,ctx.r3.u32,handle);if(ctx.r5.u32)write32(base,ctx.r5.u32,thread->id);
        STARTUP_PRINT("THREAD_CREATED id=%u handle=0x%08x entry=0x%08x startup=0x%08x suspended=%u\n",thread->id,handle,thread->entry,thread->startup,ctx.r9.u32&1);
        if(!(ctx.r9.u32&1) && ResumeThread(thread->handle)==DWORD(-1))throw DiagnosticStop("Native thread start failed");
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"KeResumeThread")==0) {
        for(const auto& pair:threads)if(pair.second->object==ctx.r3.u32) {
            const auto previous=ResumeThread(pair.second->handle);
            if(previous==DWORD(-1))throw DiagnosticStop("Native resume of guest thread object failed");
            ctx.r3.u64=previous;return;
        }
        throw DiagnosticStop("Unknown kernel thread resume object");
    }
    if(std::strcmp(name,"NtResumeThread")==0) {
        auto found=threads.find(ctx.r3.u32);
        if(found==threads.end() || found->second->closed){ctx.r3.u64=0xc0000008;return;}
        const auto previous=ResumeThread(found->second->handle);
        if(ctx.r4.u32)write32(base,ctx.r4.u32,previous);
        ctx.r3.u64=previous==DWORD(-1) ? 0xc0000001:0;return;
    }
    if(std::strcmp(name,"ExTerminateThread")==0) {
        if(current_thread_address==initial_thread)throw DiagnosticStop("Initial thread terminated with status "+std::to_string(ctx.r3.u32));
        throw ThreadExit{ctx.r3.u32};
    }
    if(std::strcmp(name,"ObReferenceObjectByHandle")==0) {
        const bool current=ctx.r3.u32==0xfffffffe;
        auto found=threads.find(ctx.r3.u32);
        if(current)for(auto it=threads.begin();it!=threads.end();++it)
            if(it->second->object==current_thread_address){found=it;break;}
        if(!current && (found==threads.end() || found->second->closed)){ctx.r3.u64=0xc0000008;return;}
        const uint32_t thread_type=read32(base,0x82000824);
        if(ctx.r4.u32 && ctx.r4.u32!=thread_type){ctx.r3.u64=0xc0000024;return;}
        if(!ctx.r5.u32){ctx.r3.u64=0xc000000d;return;}
        if(current && current_thread_address==initial_thread){++initial_thread_references;write32(base,ctx.r5.u32,initial_thread);}
        else {
            if(found==threads.end()){ctx.r3.u64=0xc0000008;return;}
            ++found->second->references;write32(base,ctx.r5.u32,found->second->object);
        }
        ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"ObDereferenceObject")==0) {
        if(ctx.r3.u32==initial_thread) {
            if(!initial_thread_references)throw DiagnosticStop("Unbalanced initial thread dereference");
            --initial_thread_references;return;
        }
        for(auto& pair:threads)if(pair.second->object==ctx.r3.u32) {
            if(!pair.second->references)throw DiagnosticStop("Unbalanced guest object dereference");
            --pair.second->references;return;
        }
        throw DiagnosticStop("Unknown object dereference");
    }
    if(std::strcmp(name,"KeSetBasePriorityThread")==0) {
        if(ctx.r3.u32==initial_thread) {
            const auto priority=ctx.r4.s32;
            const int mapped=priority>0?THREAD_PRIORITY_ABOVE_NORMAL:priority<0?THREAD_PRIORITY_BELOW_NORMAL:THREAD_PRIORITY_NORMAL;
#ifdef __SWITCH__
            const auto result=nx_configure_guest_thread(initial_native_thread,priority);
            STARTUP_PRINT("NATIVE_INITIAL_PRIORITY handle=%08x guest=%d native=%d rc=%08x\n",initial_native_thread,priority,nx_guest_priority,result);
            if(R_FAILED(result))throw DiagnosticStop("Initial native thread priority failed");
#else
            if(!initial_native_thread || !SetThreadPriority(initial_native_thread,mapped))throw DiagnosticStop("Initial native thread priority failed");
#endif
            ctx.r3.s64=initial_thread_priority;initial_thread_priority=priority;return;
        }
        for(auto& pair:threads)if(pair.second->object==ctx.r3.u32) {
            const auto old=pair.second->priority;pair.second->priority=ctx.r4.s32;
            const int host_priority=ctx.r4.s32>0 ? THREAD_PRIORITY_ABOVE_NORMAL : ctx.r4.s32<0 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
            if(!SetThreadPriority(pair.second->handle,host_priority))throw DiagnosticStop("Host thread priority failed");
            ctx.r3.s64=old;return;
        }
        throw DiagnosticStop("Unknown thread priority object");
    }
    if(std::strcmp(name,"KeSetAffinityThread")==0) {
        const auto mask=ctx.r4.u32;
        if(!mask || (mask&~0x3fu)){ctx.r3.u64=0xc000000d;return;}
        for(auto& pair:threads)if(pair.second->object==ctx.r3.u32) {
            auto& t=*pair.second;
            if(ctx.r5.u32)write32(base,ctx.r5.u32,1u<<base[t.pcr+0x10c]);
            const auto cpu=uint8_t(__builtin_ctz(mask));base[t.pcr+0x10c]=cpu;base[t.object+0xbf]=cpu;
            // Virtual Xenon CPUs are scheduled by the host; do not interpret
            // a guest CPU index as a Windows processor-affinity bit.
            ctx.r3.u64=0;return;
        }
        ctx.r3.u64=0xc0000008;return;
    }
    if(std::strcmp(name,"KeDelayExecutionThread")==0) {
        if(ctx.r4.u32)throw DiagnosticStop("Alertable delay/APC unsupported");
        if(!ctx.r5.u32){ctx.r3.u64=0xc000000d;return;}
        uint64_t raw;std::memcpy(&raw,ppc_guest_ptr(base,ctx.r5.u32),8);raw=__builtin_bswap64(raw);
        uint64_t ticks;
        if(raw>>63)ticks=~raw+1;
        else {
            FILETIME now;GetSystemTimeAsFileTime(&now);
            const auto current=(uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime;
            ticks=raw>current ? raw-current:0;
        }
        service_lock.unlock();Sleep(DWORD(std::min<uint64_t>((ticks+9999)/10000,INFINITE-1)));ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"NtYieldExecution")==0) {service_lock.unlock();Sleep(0);ctx.r3.u64=0;return;}
    if(std::strcmp(name,"RtlMultiByteToUnicodeN")==0) {
        const uint32_t count=std::min(ctx.r4.u32/2,ctx.r7.u32);
        for(uint32_t i=0;i<count;++i) {
            ppc_guest_ptr(base,ctx.r3.u32)[i*2]=0;ppc_guest_ptr(base,ctx.r3.u32)[i*2+1]=ppc_guest_ptr(base,ctx.r6.u32)[i];
        }
        if(ctx.r5.u32)write32(base,ctx.r5.u32,count*2);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"RtlUnicodeToMultiByteN")==0) {
        const uint32_t count=std::min(ctx.r4.u32,ctx.r7.u32/2);
        for(uint32_t i=0;i<count;++i)ppc_guest_ptr(base,ctx.r3.u32)[i]=ppc_guest_ptr(base,ctx.r6.u32)[i*2] ? '?' : ppc_guest_ptr(base,ctx.r6.u32)[i*2+1];
        if(ctx.r5.u32)write32(base,ctx.r5.u32,count);ctx.r3.u64=0;return;
    }
    // Explicit diagnostic machine profile: VGA output (same profile used by
    // Xenia's XGetAVPack), independent of the user's real monitor/Switch.
    if(std::strcmp(name,"XGetAVPack")==0) {ctx.r3.u64=6;return;}
    if(std::strcmp(name,"XGetGameRegion")==0) {
        STARTUP_PRINT("GUEST_REGION value=0000ffff profile=UNRESTRICTED_RUNTIME\n");ctx.r3.u64=0xffff;return;
    }
    if(std::strcmp(name,"VdQueryVideoFlags")==0){ctx.r3.u64=3;return;}
    if(std::strcmp(name,"VdCallGraphicsNotificationRoutines")==0) {
        if(ctx.r3.u32!=1)throw DiagnosticStop("Unknown graphics notification kind");
        // This title imports no VdRegisterGraphicsNotification variant, so the
        // graphics notification list is empty (distinct from GPU interrupts).
        STARTUP_PRINT("GPU_GRAPHICS_NOTIFICATION registered=0\n");ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"VdIsHSIOTrainingSucceeded")==0) {
        // Native Vulkan replaces the Xenos hardware link. Report readiness of
        // the actual native device, not unconditional success.
        ctx.r3.u64=graphics_device && graphics_device->ready();return;
    }
    if(std::strcmp(name,"VdSetDisplayMode")==0) {
        if(ctx.r3.u32!=0 && ctx.r3.u32!=0x40000000)
            throw DiagnosticStop("Unsupported display format/color-space flags");
        if(!graphics_device || !graphics_device->ready())throw DiagnosticStop("Display device not initialized");
        ctx.r3.u64=0;return; // The native 1280x720 RGBA8 resource already exists.
    }
    if(std::strcmp(name,"VdGetCurrentDisplayGamma")==0) {
        write32(base,ctx.r3.u32,0);write32(base,ctx.r4.u32,0x3f800000);return; // Declared linear output.
    }
    if(std::strcmp(name,"VdGetCurrentDisplayInformation")==0) {
        const uint32_t at=ctx.r3.u32;std::memset(ppc_guest_ptr(base,at),0,0x58);
        auto half=[&](uint32_t offset,uint16_t value){value=__builtin_bswap16(value);std::memcpy(ppc_guest_ptr(base,at+offset),&value,2);};
        half(0,1280);half(2,720);
        write32(base,at+0x10,1280);write32(base,at+0x14,720);
        write32(base,at+0x18,1280);write32(base,at+0x1c,720);
        write32(base,at+0x20,1);write32(base,at+0x30,1);
        half(0x40,320);half(0x42,180);half(0x44,320);half(0x46,180);
        half(0x48,1280);half(0x4a,720);write32(base,at+0x4c,0x42700000);half(0x56,1280);return;
    }
    if(std::strcmp(name,"XGetVideoMode")==0 || std::strcmp(name,"VdQueryVideoMode")==0) {
        const auto at=ctx.r3.u32;std::memset(ppc_guest_ptr(base,at),0,48);
        write32(base,at,1280);write32(base,at+4,720);write32(base,at+12,1);write32(base,at+16,1);
        write32(base,at+20,0x42700000);write32(base,at+24,1);write32(base,at+28,0x4a);write32(base,at+32,1);
        return; // Declared diagnostic mode only; no display/rendering is created.
    }
    if(std::strcmp(name,"KeQuerySystemTime")==0) {
        FILETIME time;GetSystemTimeAsFileTime(&time);
        const uint64_t ticks=(uint64_t(time.dwHighDateTime)<<32)|time.dwLowDateTime;
        if(ctx.r3.u32) {uint64_t be=__builtin_bswap64(ticks);std::memcpy(ppc_guest_ptr(base,ctx.r3.u32),&be,8);}return;
    }
    if(std::strcmp(name,"KeTlsAlloc")==0) {
        for(unsigned i=0;i<64;++i)if(!allocated_tls[i]) {
            allocated_tls[i]=true;write32(base,current_tls_address+i*4,0);ctx.r3.u64=i;return;
        }
        ctx.r3.u64=0xffffffff;return;
    }
    if(std::strcmp(name,"KeTlsFree")==0) {
        auto slot=ctx.r3.u32;
        if(slot>=64 || !allocated_tls[slot]){ctx.r3.u64=0;return;}
        allocated_tls[slot]=false;write32(base,current_tls_address+slot*4,0);ctx.r3.u64=1;return;
    }
    if(std::strcmp(name,"XexCheckExecutablePrivilege")==0) {
        const auto privilege=ctx.r3.u32;
        const auto flags=xex_header_field(base+header,0x4000,header,0x30000);
        ctx.r3.u64=privilege<32 && (flags&(1u<<privilege)) ? 1:0;return;
    }
    if(std::strcmp(name,"RtlInitializeCriticalSection")==0 || std::strcmp(name,"RtlInitializeCriticalSectionAndSpinCount")==0) {
        const auto address=ctx.r3.u32;
        if(critical_sections.count(address)) {
            STARTUP_PRINT("CRITICAL_SECTION_REINITIALIZE address=%08x native_users=%u guest_depth=%u guest_owner=%08x\n",address,critical_section_users[address],read32(base,address+20),read32(base,address+24));
            if(critical_section_users[address])throw DiagnosticStop("Critical section reinitialization with owner or waiters");
            // Reuse the existing, provably idle native mutex; initialize the
            // guest object exactly as the kernel does for a new lifetime.
        }else {auto* lock=new CRITICAL_SECTION;InitializeCriticalSection(lock);critical_sections.emplace(address,lock);}
        std::memset(ppc_guest_ptr(base,address),0,28);*ppc_guest_ptr(base,address)=1;
        if(std::strcmp(name,"RtlInitializeCriticalSectionAndSpinCount")==0)
            ppc_guest_ptr(base,address)[1]=uint8_t(std::min<uint64_t>((uint64_t(ctx.r4.u32)+255)/256,255));
        write32(base,address+16,0xffffffff);ctx.r3.u64=0;return;
    }
    if(std::strcmp(name,"RtlEnterCriticalSection")==0 || std::strcmp(name,"RtlTryEnterCriticalSection")==0 ||
       std::strcmp(name,"RtlLeaveCriticalSection")==0) {
        const uint32_t address=ctx.r3.u32;
        auto found=critical_sections.find(address);
        if(found==critical_sections.end()) {
            // Statically initialized critical sections are legal; validate the
            // unlocked representation before creating the corresponding host lock.
            if(*ppc_guest_ptr(base,address)!=1 || read32(base,address+16)!=0xffffffff || read32(base,address+20) || read32(base,address+24))
                throw DiagnosticStop("Unknown/uninitialized guest critical section");
            auto* lock=new CRITICAL_SECTION;InitializeCriticalSection(lock);
            found=critical_sections.emplace(address,lock).first;
        }
        if(std::strcmp(name,"RtlLeaveCriticalSection")==0) {
            auto depth=read32(base,address+20);
            if(!depth || read32(base,address+24)!=current_thread_address || !critical_section_users[address])throw DiagnosticStop("Invalid critical section release");
            write32(base,address+20,depth-1);if(depth==1)write32(base,address+24,0);
            write32(base,address+16,read32(base,address+16)-1);LeaveCriticalSection(found->second);--critical_section_users[address];return;
        }
        if(std::strcmp(name,"RtlTryEnterCriticalSection")==0 && !TryEnterCriticalSection(found->second)) {ctx.r3.u64=0;return;}
        ++critical_section_users[address];
        if(std::strcmp(name,"RtlEnterCriticalSection")==0) {
            auto* lock=found->second;service_lock.unlock();EnterCriticalSection(lock);service_lock.lock();
        }
        write32(base,address+16,read32(base,address+16)+1);
        write32(base,address+20,read32(base,address+20)+1);write32(base,address+24,current_thread_address);
        if(std::strcmp(name,"RtlTryEnterCriticalSection")==0)ctx.r3.u64=1;
        return;
    }
    if(std::strcmp(name,"NtAllocateVirtualMemory")==0 || std::strcmp(name,"NtFreeVirtualMemory")==0 ||
       std::strcmp(name,"NtProtectVirtualMemory")==0) {
        if(!allocator)throw DiagnosticStop("Guest allocator unavailable");
        const uint32_t address_ptr=ctx.r3.u32,size_ptr=ctx.r4.u32;
        if(!address_ptr || !size_ptr) {ctx.r3.u64=0xc000000d;return;}
        uint32_t address=read32(base,address_ptr),size=read32(base,size_ptr),status,old=0;
        if(std::strcmp(name,"NtAllocateVirtualMemory")==0) {
            if(ctx.r7.u32)throw DiagnosticStop("Devkit allocation unsupported");
            status=allocator->allocate(address,size,ctx.r5.u32,ctx.r6.u32);
        } else if(std::strcmp(name,"NtFreeVirtualMemory")==0) {
            if(ctx.r6.u32)throw DiagnosticStop("Devkit free unsupported");
            status=allocator->free(address,size,ctx.r5.u32);
        } else {
            if(ctx.r7.u32)throw DiagnosticStop("Devkit protection unsupported");
            status=allocator->protect(address,size,ctx.r5.u32,old);
            if(!status && ctx.r6.u32)write32(base,ctx.r6.u32,old);
        }
        if(!status){write32(base,address_ptr,address);write32(base,size_ptr,size);}
        ctx.r3.u64=status;
        STARTUP_PRINT("VM_RESULT status=0x%08x base=0x%08x size=0x%08x\n",status,address,size);return;
    }
    // Other imports fail visibly. No success values are fabricated.
    throw DiagnosticStop(std::string("Unimplemented import: ")+name);
}

extern "C" uint64_t airborne_gpu_mmio_read(PPCContext& ctx,uint32_t address,unsigned bytes) {
    GuestFpScope floating_point(ctx);
    std::lock_guard<std::mutex> lock(service_mutex);
    if(imports_stopping)throw ThreadExit{0};
#ifdef __SWITCH__
    if(nx_runtime_stopping())throw ThreadExit{0};
#endif
    STARTUP_PRINT("GPU_MMIO_READ address=%08x bytes=%u LR=%08x\n",address,bytes,uint32_t(ctx.lr));
    if((address&0xffff0000u)==0x7fea0000u) {
        if(bytes!=4 || address%4)throw DiagnosticStop("Unsupported XMA MMIO read width/alignment");
        if(address==0x7fea1800){xma_ensure_table();return __builtin_bswap32(GuestPhysicalAllocator::physical(xma_context_table));}
        throw DiagnosticStop("XMA register read requires decoder implementation: "+std::to_string(address));
    }
    if(bytes!=4 || address%4 || !graphics_ring)throw DiagnosticStop("Invalid GPU MMIO read width/alignment/state");
    const uint32_t index=(address&0xffff)/4;
    // Declared virtual Xenos render-buffer configuration. No GPU completion or
    // vblank interrupt status is fabricated by these configuration registers.
    if(index==0xf00)return 0x08100748;
    if(index==0xf01)return 0x0000200e;
    if(index==0x1961)return (1280u<<16)|720;
    if(index==0x1951)return graphics_ring->read_register(index);
    if(index==0x194c) {
        if(graphics_ring->read_register(0x1951)&1)return 720; // Only the delivered vblank interval is modeled.
        throw DiagnosticStop("Active scanline position outside delivered vblank is not modeled");
    }
    try{return graphics_ring->read_register(index);}
    catch(const std::exception& error){throw DiagnosticStop(error.what());}
}

extern "C" void airborne_gpu_mmio_write(PPCContext& ctx,uint32_t address,uint64_t value,unsigned bytes) {
    GuestFpScope floating_point(ctx);
    std::unique_lock<std::mutex> lock(service_mutex);
    if(imports_stopping)throw ThreadExit{0};
#ifdef __SWITCH__
    if(nx_runtime_stopping())throw ThreadExit{0};
#endif
    STARTUP_PRINT("GPU_MMIO_WRITE address=%08x bytes=%u value=%016llx LR=%08x\n",address,bytes,(unsigned long long)value,uint32_t(ctx.lr));
    if((address&0xffff0000u)==0x7fea0000u)throw DiagnosticStop("XMA register write requires decoder implementation: "+std::to_string(address));
    if(bytes!=4 || address%4 || !graphics_ring)throw DiagnosticStop("Invalid GPU MMIO write width/alignment/state");
    try{graphics_ring->write_register((address&0xffff)/4,uint32_t(value));}
    catch(const std::exception& error){throw DiagnosticStop(error.what());}
    if((address&0xffff)/4!=0x1c5)return;
    unsigned display_wakes=0;
    for(unsigned batch=0;batch<1024;++batch) {
        const auto pending=graphics_ring->take_interrupts();
        const auto callback=graphics_interrupt_callback,argument=graphics_interrupt_argument;
        const bool display=graphics_ring->waiting_display();
        if(display) {
            if(++display_wakes>120)throw DiagnosticStop("GPU wait still blocked after 120 display events; see last WAIT operands");
            try{graphics_device->wait_vblank();}catch(const std::exception& error){throw DiagnosticStop(error.what());}
            graphics_ring->write_register(0x1951,1);
        }
        lock.unlock();
        try{gpu_dispatch_interrupts(ctx,pending,callback,argument);if(display)gpu_dispatch_interrupts(ctx,{2},callback,argument,0);}
        catch(...){lock.lock();if(display && graphics_ring)graphics_ring->write_register(0x1951,0);throw;}
        lock.lock();
        if(!graphics_ring)throw DiagnosticStop("Graphics engine removed during interrupt delivery");
        if(display)graphics_ring->write_register(0x1951,0);
        if(!graphics_ring->suspended())return;
        try{graphics_ring->resume();}catch(const std::exception& error){throw DiagnosticStop(error.what());}
    }
    throw DiagnosticStop("GPU interrupt/resumption batch limit");
}
