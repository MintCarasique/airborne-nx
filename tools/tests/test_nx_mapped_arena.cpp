#include "mapped_arena.h"
#include <cstdio>
#include <stdexcept>
#include <string>
struct View {uint64_t source,size;unsigned type,permission;};
static std::map<uint64_t,View> views;
static bool fail_map=false,fail_permission=false,fail_unmap=false,fail_alias=false;
static unsigned reservations=0,checks=0;
static void check(bool value){++checks;if(!value)throw std::runtime_error("Check "+std::to_string(checks));}
bool envIsSyscallHinted(unsigned){return true;}
Handle envGetOwnProcessHandle(){return 7;}
void virtmemLock(){} void virtmemUnlock(){}
void* virtmemFindAslr(size_t,size_t){return reinterpret_cast<void*>(0x1000000000ull);}
void* virtmemFindCodeMemory(size_t,size_t){return reinterpret_cast<void*>(0x2000000000ull);}
VirtmemReservation* virtmemAddReservation(void* at,size_t bytes){++reservations;return new VirtmemReservation{at,bytes};}
void virtmemRemoveReservation(VirtmemReservation* r){--reservations;delete r;}
static bool overlap(uint64_t at,uint64_t bytes) {
    for(auto& pair:views)if(at<pair.first+pair.second.size && pair.first<at+bytes)return true;
    return false;
}
Result svcMapProcessCodeMemory(Handle handle,uint64_t to,uint64_t from,uint64_t bytes) {
    if(fail_map)return 0xd401;
    if(handle!=7 || overlap(to,bytes))throw std::runtime_error("Invalid code map");
    views.emplace(to,View{from,bytes,8,0});return 0;
}
Result svcSetProcessMemoryPermission(Handle,uint64_t at,uint64_t bytes,uint32_t permission) {
    if(fail_permission)return 0xd401;
    auto& v=views.at(at);check(v.type==8 && v.size==bytes && permission==3);v.type=9;v.permission=3;return 0;
}
Result svcSetMemoryPermission(void* at,uint64_t bytes,uint32_t permission) {
    auto it=views.upper_bound(uintptr_t(at));if(it==views.begin())return 0xd401;--it;
    auto& v=it->second;
    if(v.type!=9 || uintptr_t(at)+bytes>it->first+v.size)return 0xd401;
    if(fail_permission)return 0xd401;v.permission=permission;return 0;
}
Result svcMapProcessMemory(void* to,Handle,uint64_t from,uint64_t bytes) {
    if(fail_alias)return 0xd401;
    auto it=views.upper_bound(from);if(it==views.begin())return 0xd401;--it;
    if(it->second.type!=9 || from+bytes>it->first+it->second.size || overlap(uintptr_t(to),bytes))return 0xd401;
    views.emplace(uintptr_t(to),View{from,bytes,15,3});return 0;
}
Result svcUnmapProcessMemory(void* to,Handle,uint64_t from,uint64_t bytes) {
    if(fail_unmap)return 0xd401;
    auto& v=views.at(uintptr_t(to));check(v.type==15 && v.source==from && v.size==bytes);views.erase(uintptr_t(to));return 0;
}
Result svcUnmapProcessCodeMemory(Handle,uint64_t to,uint64_t from,uint64_t bytes) {
    if(fail_unmap)return 0xd401;
    auto& v=views.at(to);check((v.type==8 || v.type==9) && v.source==from && v.size==bytes);
    for(auto& p:views)if(p.second.type==15 && p.second.source>=to && p.second.source<to+bytes)throw std::runtime_error("Live physical alias");
    views.erase(to);return 0;
}
Result svcQueryMemory(MemoryInfo* m,uint32_t*,uint64_t at) {
    auto it=views.upper_bound(at);
    if(it!=views.begin()) {--it;if(at<it->first+it->second.size){*m={it->first,it->second.size,it->second.type,0,it->second.permission,0,0,0};return 0;}}
    *m={at,4096,0,0,0,0,0,0};return 0;
}
int main() {
    {
        NxMappedArena memory(nullptr,65536);check(memory.open()==0);
        check(!memory.commit(0x10001,4096));check(!memory.commit(0xfffff000,8192));check(!memory.commit(0xa0000000,4096));
        check(memory.commit(0x10000,65536));check(!memory.commit(0x18000,4096));
        check(!memory.protect(0x20000,4096,1));check(!memory.decommit(0x11000,4096));check(views.size()==1);
        check(memory.protect(0x10000,65536,1));check(memory.protect(0x10000,65536,0));check(memory.protect(0x10000,65536,3));
        check(memory.decommit(0x10000,65536));check(memory.decommit(0x10000,65536));check(views.empty());
        check(memory.commit(0x10000,4096));check(memory.commit(0x12000,4096));
        check(!memory.protect(0x10000,12288,1));check(memory.decommit(0x10000,12288));
        for(uint64_t at:{0xa0001000ull,0xc0001000ull,0xe0001000ull})check(memory.physical_map(at,4096));
        check(!memory.physical_map(0xa0001000,4096));check(!memory.physical_map(0xa0010000,4096));
        check(!memory.protect(0xa0001000,4096,1));check(!memory.physical_unmap(0xa0001000,8192));
        check(memory.physical_unmap(0xa0001000,4096));check(memory.physical_map(0xa0001000,4096));
        check(memory.close()==0);check(views.empty() && reservations==0);
    }
    for(unsigned failure=0;failure<2;++failure) {
        NxMappedArena memory(nullptr);check(memory.open()==0);fail_map=failure==0;fail_permission=failure==1;
        check(!memory.commit(0x20000,4096));fail_map=false;fail_permission=false;
        check(views.empty());check(memory.close()==0);check(reservations==0);
    }
    {
        NxMappedArena memory(nullptr,65536);check(memory.open()==0);
        check(memory.physical_map(0xa0001000,4096));fail_alias=true;
        check(!memory.physical_map(0xc0001000,4096));fail_alias=false;
        check(memory.close()==0);check(views.empty() && reservations==0);
    }
    {
        NxMappedArena memory(nullptr);check(memory.open()==0);check(memory.commit(0x30000,4096));
        const auto source=views.begin()->second.source;fail_unmap=true;
        check(!memory.decommit(0x30000,4096));check(views.size()==1);check(!memory.commit(0x40000,4096));check(memory.close()!=0);
        // Model kernel process teardown; do not claim the poisoned arena recovered.
        fail_unmap=false;views.clear();std::free(reinterpret_cast<void*>(source));
    }
    std::printf("%u Switch runtime memory lifecycle checks passed (mock kernel only)\n",checks);
}
