#include "guest_memory.h"
#include <cstdio>
#include <vector>
#include <stdexcept>
#include <string>
#include <map>
struct Mapping {void* destination;uint64_t source,size;};
static std::vector<Mapping> active;
static bool hints=true,find_space=true,reserve=true,fail_map=false,fail_unmap=false;
static Handle process=7;
static int locks=0;static unsigned releases=0,unmaps=0,map_calls=0;
static VirtmemReservation reservation{};
struct Staging {uint64_t source,size;bool writable;};
static std::map<uint64_t,Staging> staging;
static bool fail_code_map=false,fail_permission=false,fail_code_unmap=false;
bool envIsSyscallHinted(unsigned number){return hints && (number==0x73 || number==0x74 || number==0x75 || number==0x77 || number==0x78);}
Handle envGetOwnProcessHandle(){return process;}
void virtmemLock(){if(locks++)throw std::runtime_error("Double lock");}
void virtmemUnlock(){if(--locks)throw std::runtime_error("Unbalanced unlock");}
void* virtmemFindAslr(size_t bytes,size_t guard){
    if(locks!=1 || bytes!=0x100000000ull || guard!=0x10000)throw std::runtime_error("Wrong reservation request");
    return find_space ? reinterpret_cast<void*>(0x1000000000ull):nullptr;
}
void* virtmemFindCodeMemory(size_t bytes,size_t guard){
    if(locks!=1 || guard!=4096)throw std::runtime_error("Staging reservation request");return std::malloc(bytes);
}
VirtmemReservation* virtmemAddReservation(void* base,size_t bytes){
    if(locks!=1)throw std::runtime_error("Reservation outside lock");
    if(bytes!=0x100000000ull)return new VirtmemReservation{base,bytes};
    reservation={base,bytes};return reserve ? &reservation:nullptr;
}
void virtmemRemoveReservation(VirtmemReservation* r){
    if(locks!=1 || !active.empty())throw std::runtime_error("Premature reservation release");
    if(r==&reservation){if(!staging.empty())throw std::runtime_error("Live staging at VA release");++releases;}
    else {if(staging.count(uintptr_t(r->base)))throw std::runtime_error("Live code alias at release");std::free(r->base);delete r;}
}
Result svcMapProcessCodeMemory(Handle handle,uint64_t destination,uint64_t source,uint64_t bytes){
    if(handle!=7 || staging.count(destination))throw std::runtime_error("Bad code-map arguments");
    if(fail_code_map)return 0x1111;
    std::memcpy(reinterpret_cast<void*>(destination),reinterpret_cast<void*>(source),bytes);
    staging.emplace(destination,Staging{source,bytes,false});return 0;
}
Result svcSetProcessMemoryPermission(Handle handle,uint64_t destination,uint64_t bytes,uint32_t perm){
    if(handle!=7 || !staging.count(destination) || staging.at(destination).size!=bytes || perm!=Perm_Rw)throw std::runtime_error("Bad code permission request");
    if(fail_permission)return 0x2222;staging.at(destination).writable=true;return 0;
}
Result svcUnmapProcessCodeMemory(Handle handle,uint64_t destination,uint64_t source,uint64_t bytes){
    if(handle!=7 || !active.empty() || !staging.count(destination) || staging.at(destination).source!=source || staging.at(destination).size!=bytes)throw std::runtime_error("Code alias cleanup out of order");
    if(fail_code_unmap)return 0x3333;
    std::memcpy(reinterpret_cast<void*>(source),reinterpret_cast<void*>(destination),bytes);staging.erase(destination);return 0;
}
Result svcQueryMemory(MemoryInfo* info,uint32_t* page,uint64_t address){
    *info={address&~uint64_t(4095),4096,5,0,Perm_Rw,0,0,0};*page=0;return 0;
}
Result svcMapProcessMemory(void* destination,Handle handle,uint64_t source,uint64_t bytes) {
    ++map_calls;if(handle!=7 || !source || !bytes)throw std::runtime_error("Wrong mapping call");
    if(fail_map)return 0x1234;
    // Horizon's Normal heap state does NOT carry CanMapProcess. This check
    // reproduces the real v0.2 failure that the old mock failed to model.
    auto found=staging.find(source);
    if(found==staging.end() || !found->second.writable)return 0xd401;
    active.push_back({destination,source,bytes});return 0;
}
Result svcUnmapProcessMemory(void* destination,Handle handle,uint64_t source,uint64_t bytes) {
    ++unmaps;if(handle!=7 || active.empty())throw std::runtime_error("Invalid unmap");
    auto& last=active.back();if(last.destination!=destination || last.source!=source || last.size!=bytes)throw std::runtime_error("Unmap mismatch/order");
    if(fail_unmap)return 0x5678;active.pop_back();return 0;
}
int main() {
    size_t checks=0;
    auto check=[&](bool good){++checks;if(!good)throw std::runtime_error("NX memory contract test "+std::to_string(checks));};
    for(int failure=0;failure<4;++failure) {
        hints=failure!=0;process=failure==1 ? 0:7;find_space=failure!=2;reserve=failure!=3;
        NxGuestMemory m;check(R_FAILED(m.open()));check(m.base==nullptr);check(m.close()==0);check(locks==0);
    }
    hints=find_space=reserve=true;process=7;
    {
        auto* ordinary=std::malloc(4096);
        check(svcMapProcessMemory(reinterpret_cast<void*>(0x1020000000ull),7,uintptr_t(ordinary),4096)==0xd401);
        std::free(ordinary);
    }
    for(unsigned failure=0;failure<3;++failure) {
        NxGuestMemory m;check(m.open()==0);unsigned index;
        fail_code_map=failure==0;fail_permission=failure==1;
        if(failure<2)check(R_FAILED(m.create_backing(4096,index)));
        else check(m.create_backing(4096,index)==0);
        fail_code_map=fail_permission=false;fail_code_unmap=failure==2;
        if(failure==2){check(m.close()==0x3333);check(m.base && m.backing(0));fail_code_unmap=false;}
        check(m.close()==0 && staging.empty());
    }
    {
        NxGuestMemory m;check(m.open()==0);check(R_FAILED(m.open()));unsigned index;
        check(R_FAILED(m.create_backing(0,index)));check(R_FAILED(m.create_backing(1,index)));
        check(m.create_backing(65536,index)==0 && index==0);check(m.backing(index)!=nullptr);
        for(unsigned i=0;i<65536;++i)check(m.backing(index)[i]==0);
        check(m.map(0xa0010000,index)==0);check(m.map(0xc0010000,index)==0);check(m.map(0xe000f000,index)==0);
        check(active.size()==3 && active[0].source==active[1].source && active[1].source==active[2].source);
        check(uintptr_t(active[2].destination)==uintptr_t(m.base)+0xe0010000ull);
        auto before=map_calls;check(R_FAILED(m.map(0xa0011000,index)));check(map_calls==before);
        check(R_FAILED(m.map(0x20000001,index)));check(R_FAILED(m.map(0x20000000,8)));
        fail_map=true;check(m.map(0x20000000,index)==0x1234);fail_map=false;check(active.size()==3);
        fail_unmap=true;auto previous=releases;check(m.close()==0x5678);check(m.base && m.backing(0));check(releases==previous && active.size()==3);
        fail_unmap=false;check(m.close()==0);check(active.empty() && m.base==nullptr && m.backing(0)==nullptr);check(releases==previous+1);
        check(m.close()==0);
    }
    {
        NxGuestMemory m;check(m.open()==0);unsigned index;
        for(unsigned i=0;i<8;++i)check(m.create_backing(4096,index)==0 && index==i);
        check(R_FAILED(m.create_backing(4096,index)));
        for(unsigned i=0;i<16;++i)check(m.map(0x20000000+i*4096,0)==0);
        check(R_FAILED(m.map(0x20010000,0)));check(m.close()==0 && active.empty());
    }
    check(!NxGuestMemory::valid_range(0xfffff000,4096));
    check(!NxGuestMemory::valid_range(0xdffff000,8192));
    check(NxGuestMemory::valid_range(0xdffff000,4096));
    check(NxGuestMemory::valid_range(0xe0000000,4096));
    check(locks==0);std::printf("%zu NX guest-memory API/cleanup contract checks passed (mock kernel, not hardware)\n",checks);
}
