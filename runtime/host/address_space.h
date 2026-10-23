#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef __SWITCH__
#include "../switch/platform_compat.h"
#else
#include <windows.h>
#endif
#include <cstdint>
#include <stdexcept>
#include <map>
#include <algorithm>
#include <cstring>

#ifndef __SWITCH__
class GuestMemory {
    HANDLE physical_=nullptr;
    bool lower_reserved_=false;
    bool views_[3]{};
    void cleanup() {
        for(unsigned i=0;i<3;++i)if(views_[i])UnmapViewOfFile(base+0xa0000000ull+i*0x20000000ull);
        if(physical_)CloseHandle(physical_);
        if(lower_reserved_)VirtualFree(base,0,MEM_RELEASE);
    }
public:
    uint8_t* base=nullptr;
    GuestMemory() {
        // Locate a contiguous window, then reserve its virtual portion and map
        // the three physical aliases separately at that exact placement.
        base=static_cast<uint8_t*>(VirtualAlloc(nullptr,0x100000000ull,MEM_RESERVE,PAGE_NOACCESS));
        if(!base)throw std::runtime_error("Guest address window unavailable");
        VirtualFree(base,0,MEM_RELEASE);
        if(VirtualAlloc(base,0xa0000000ull,MEM_RESERVE,PAGE_NOACCESS)!=base)
            throw std::runtime_error("Guest virtual reservation placement failed");
        lower_reserved_=true;
        physical_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,0x20000000,nullptr);
        if(!physical_){cleanup();throw std::runtime_error("Physical backing allocation failed");}
        for(unsigned i=0;i<3;++i) {
            auto* target=base+0xa0000000ull+i*0x20000000ull;
            if(MapViewOfFileEx(physical_,FILE_MAP_ALL_ACCESS,0,0,0x20000000,target)!=target) {
                cleanup();throw std::runtime_error("Physical alias mapping failed");
            }
            views_[i]=true;DWORD previous;
            if(!VirtualProtect(target,0x20000000,PAGE_NOACCESS,&previous)){cleanup();throw std::runtime_error("Physical alias protection failed");}
        }
    }
    ~GuestMemory(){cleanup();}
    GuestMemory(const GuestMemory&)=delete;
    GuestMemory& operator=(const GuestMemory&)=delete;
    void commit(uint64_t address,uint64_t bytes) {
        if(!bytes || address>=0xa0000000 || bytes>0xa0000000-address ||
           !VirtualAlloc(base+address,bytes,MEM_COMMIT,PAGE_READWRITE))throw std::runtime_error("Guest commit failed");
    }
};
#else
using GuestMemory = NxRuntimeGuestMemory;
#endif

class GuestPhysicalAllocator {
    struct Allocation { uint32_t bytes,guest,protection,page; };
    uint8_t* base_;
    std::map<uint32_t,Allocation> allocations_;
    void protect_aliases(uint32_t offset,uint32_t bytes,DWORD protection) {
        for(unsigned i=0;i<3;++i) {
            DWORD previous;
            if(!VirtualProtect(base_+0xa0000000ull+i*0x20000000ull+offset,bytes,protection,&previous))
                throw std::runtime_error("Physical page protection failed");
        }
    }
public:
    explicit GuestPhysicalAllocator(uint8_t* base):base_(base){}
    uint32_t query_protection(uint32_t guest) const {
        const auto offset=physical(guest);auto it=allocations_.upper_bound(offset);
        if(it==allocations_.begin())return 0;--it;
        if(uint64_t(offset)>=uint64_t(it->first)+it->second.bytes)return 0;
        return it->second.protection&0x607;
    }
    uint64_t allocated_bytes() const {
        uint64_t total=0;for(const auto& pair:allocations_)total+=pair.second.bytes;return total;
    }
    uint32_t allocate(uint32_t bytes,uint32_t protection,uint32_t minimum,uint32_t maximum,uint32_t alignment) {
        const uint32_t page=protection&0x20000000 ? 65536 : protection&0x80000000 ? 0x1000000:4096;
        const uint32_t view=page==4096 ? 0xe0000000 : page==65536 ? 0xa0000000 : 0xc0000000;
        if((protection&~0xa0000606u) || ((protection&6)!=2 && (protection&6)!=4) ||
           (protection&0xa0000000u)==0xa0000000u || !bytes)return 0;
        const uint64_t size=(uint64_t(bytes)+page-1)/page*page;
        const uint64_t align=std::max<uint64_t>(page,(uint64_t(alignment)+page-1)/page*page);
        const uint64_t end=std::min<uint64_t>(uint64_t(maximum)+1,page==4096 ? 0x1fd01000 : 0x20000000);
        const uint64_t low=std::max<uint64_t>(minimum,page==4096 ? 0x1000:0);
        if(end<size || end-size<low)return 0;
        uint64_t start=(end-size)/align*align;
        for(auto it=allocations_.rbegin();it!=allocations_.rend();++it) {
            if(uint64_t(it->first)>=start+size || uint64_t(it->first)+it->second.bytes<=start)continue;
            if(it->first<size)return 0;
            start=(uint64_t(it->first)-size)/align*align;
        }
        if(start<low || start+size>end)return 0;
        const uint32_t guest=view+uint32_t(start)-(page==4096 ? 0x1000:0);
        protect_aliases(uint32_t(start),uint32_t(size),PAGE_READWRITE);
        std::memset(base_+0xa0000000ull+start,0,size);
        if((protection&6)==2)protect_aliases(uint32_t(start),uint32_t(size),PAGE_READONLY);
        allocations_.emplace(uint32_t(start),Allocation{uint32_t(size),guest,protection,page});return guest;
    }
    bool free(uint32_t guest) {
        for(auto it=allocations_.begin();it!=allocations_.end();++it)if(it->second.guest==guest) {
            protect_aliases(it->first,it->second.bytes,PAGE_NOACCESS);allocations_.erase(it);return true;
        }
        return false;
    }
    static uint32_t physical(uint32_t guest) {
        if(guest<0xa0000000)return UINT32_MAX;
        const uint32_t offset=guest&0x1fffffff;
        return offset+(guest>=0xe0000000 ? 0x1000:0);
    }
    bool contains(uint32_t physical,uint32_t bytes,bool write=false) const {
        if(!bytes || physical>=0x20000000 || bytes>0x20000000-physical)return false;
        auto allocation=allocations_.upper_bound(physical);
        if(allocation==allocations_.begin())return false;
        --allocation;
        return uint64_t(physical)+bytes<=uint64_t(allocation->first)+allocation->second.bytes &&
            (allocation->second.protection&255)!=1 && (!write || (allocation->second.protection&255)==4);
    }
    bool protect(uint32_t guest,uint32_t bytes,uint32_t protection) {
        const uint32_t access=protection&255;
        if(access!=1 && access!=2 && access!=4)return false;
        const uint32_t physical_address=physical(guest);
        if(!bytes || physical_address>=0x20000000 || bytes>0x20000000-physical_address)return false;
        auto allocation=allocations_.find(physical_address);
        if(allocation==allocations_.end())return false;
        const uint64_t page=allocation->second.page;
        if((uint64_t(bytes)+page-1)/page*page!=allocation->second.bytes)return false;
        const uint32_t old_access=allocation->second.protection&255;
        // Cache hints are guest policy metadata. GPU uploads use the native
        // backend's cache/coherency contract rather than alias cache attributes.
        if(access!=old_access) {
            DWORD previous[3]{};unsigned changed=0;
            const DWORD native=access==1?PAGE_NOACCESS:access==2?PAGE_READONLY:PAGE_READWRITE;
            for(;changed<3;++changed) {
                if(!VirtualProtect(base_+0xa0000000ull+changed*0x20000000ull+physical_address,
                                   allocation->second.bytes,native,&previous[changed]))break;
            }
            if(changed!=3) {
                bool restored=true;
                for(unsigned i=0;i<changed;++i){DWORD unused;restored&=VirtualProtect(base_+0xa0000000ull+i*0x20000000ull+physical_address,
                    allocation->second.bytes,previous[i],&unused);}
                if(!restored)throw std::runtime_error("Physical protection rollback failed");
                return false;
            }
        }
        allocation->second.protection=protection;return true;
    }
};
