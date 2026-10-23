#pragma once
#include "guest_memory.h"
#include <map>
#include <algorithm>
#include <stdexcept>

// Runtime virtual pages use AliasCodeData directly: unlike SharedCode guest
// aliases, these pages have CanReprotect. Physical aliases stay RW or unmapped.
// Contracts: Atmosphere kern_k_memory_block.hpp / kern_k_page_table_base.cpp.
class NxMappedArena {
public:
    using Log=void (*)(const char*,...);
private:
    struct Block { uint8_t* original;size_t bytes;bool borrowed; };
    struct Alias { uint64_t source;size_t bytes; };
    NxGuestMemory reservation_;
    std::map<uint64_t,Block> blocks_;
    std::map<uint64_t,Alias> aliases_;
    Log log_;
    uint8_t* physical_=nullptr;
    size_t physical_size_;
    bool poisoned_=false;
    Result operation(const char* name,uint64_t destination,uint64_t source,size_t bytes,Result rc) {
        if(log_)log_("RUNTIME_MEM operation=%s destination=%016llx source=%016llx bytes=%zu rc=%08x module=%u description=%u\n",
            name,(unsigned long long)destination,(unsigned long long)source,bytes,rc,R_MODULE(rc),R_DESCRIPTION(rc));
        if(R_FAILED(rc) && log_) {
            for(auto at:{destination,source})if(at) {
                MemoryInfo m{};u32 page=0;Result q=svcQueryMemory(&m,&page,at);
                log_("RUNTIME_REGION address=%016llx query_rc=%08x region=%016llx size=%llu type=%x attr=%x perm=%x ipc=%u device=%u\n",
                    (unsigned long long)at,q,(unsigned long long)m.addr,(unsigned long long)m.size,m.type,m.attr,m.perm,m.ipc_refcount,m.device_refcount);
            }
        }
        return rc;
    }
    bool valid(uint64_t offset,size_t bytes)const {
        return base() && bytes && !(offset&4095) && !(bytes&4095) && offset<0x100000000ull && bytes<=0x100000000ull-offset;
    }
public:
    explicit NxMappedArena(Log logger,size_t physical_bytes=0x20000000):reservation_(logger),log_(logger),physical_size_(physical_bytes){}
    uint8_t* base()const{return reservation_.base;}
    Result open(){return reservation_.open();}
    bool commit(uint64_t offset,size_t bytes,unsigned permission=Perm_Rw) {
        if(poisoned_ || !valid(offset,bytes) || offset+bytes>0xa0000000 || (permission!=0 && permission!=1 && permission!=3))return false;
        auto next=blocks_.lower_bound(offset);
        if(next!=blocks_.end() && next->first<offset+bytes)return false;
        if(next!=blocks_.begin()) {auto previous=std::prev(next);if(previous->first+previous->second.bytes>offset)return false;}
        auto* original=static_cast<uint8_t*>(memalign(4096,bytes));if(!original)return false;
        std::memset(original,0,bytes);
        auto inserted=blocks_.emplace(offset,Block{original,bytes,false});
        auto& block=inserted.first->second;const auto destination=uint64_t(uintptr_t(base()+offset));
        auto rc=operation("MapProcessCodeMemory",destination,uintptr_t(original),bytes,
            svcMapProcessCodeMemory(envGetOwnProcessHandle(),destination,uintptr_t(original),bytes));
        if(R_FAILED(rc)){std::free(original);blocks_.erase(inserted.first);return false;}
        block.borrowed=true;
        rc=operation("SetProcessMemoryPermission_RW",destination,0,bytes,
            svcSetProcessMemoryPermission(envGetOwnProcessHandle(),destination,bytes,Perm_Rw));
        if(R_SUCCEEDED(rc) && permission!=Perm_Rw) {
            if(log_)log_("RUNTIME_PERMISSION address=%016llx bytes=%zu permission=%u\n",(unsigned long long)destination,bytes,permission);
            rc=operation("SetMemoryPermission",destination,0,bytes,svcSetMemoryPermission(base()+offset,bytes,permission));
        }
        if(R_FAILED(rc)) {
            const auto undo=operation("UnmapProcessCodeMemory_rollback",destination,uintptr_t(original),bytes,
                svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),destination,uintptr_t(original),bytes));
            if(R_SUCCEEDED(undo)){std::free(original);blocks_.erase(inserted.first);}else poisoned_=true;
            return false;
        }
        return true;
    }
    bool decommit(uint64_t offset,size_t bytes) {
        if(poisoned_ || !valid(offset,bytes) || offset+bytes>0xa0000000)return false;
        // Validate the entire requested range before mutating any backing.
        for(const auto& pair:blocks_)if(pair.first<offset+bytes && offset<pair.first+pair.second.bytes)
            if(pair.first<offset || pair.first+pair.second.bytes>offset+bytes)return false;
        auto it=blocks_.lower_bound(offset);
        while(it!=blocks_.end() && it->first<offset+bytes) {
            const auto destination=uintptr_t(base()+it->first);auto& b=it->second;
            const auto rc=operation("UnmapProcessCodeMemory",destination,uintptr_t(b.original),b.bytes,
                svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),destination,uintptr_t(b.original),b.bytes));
            if(R_FAILED(rc)){poisoned_=true;return false;}
            std::free(b.original);it=blocks_.erase(it);
        }
        return true;
    }
    bool protect(uint64_t offset,size_t bytes,unsigned permission) {
        if(poisoned_ || !valid(offset,bytes) || (permission!=0 && permission!=1 && permission!=3))return false;
        if(offset>=0xa0000000) {
            if(permission==Perm_Rw)return physical_map(offset,bytes);
            if(permission==Perm_None)return physical_unmap(offset,bytes);
            if(log_)log_("RUNTIME_MEMORY_REJECT reason=physical_readonly_alias_unsupported offset=%08llx bytes=%zu permission=%u\n",(unsigned long long)offset,bytes,permission);
            return false; // SharedCode cannot honestly provide read-only aliases.
        }
        // Check all pages are committed before invoking the reprotection SVC.
        uint64_t cursor=offset;
        while(cursor<offset+bytes) {
            auto it=blocks_.upper_bound(cursor);if(it==blocks_.begin())return false;--it;
            if(cursor>=it->first+it->second.bytes || !it->second.borrowed)return false;
            cursor=std::min<uint64_t>(offset+bytes,it->first+it->second.bytes);
        }
        if(log_)log_("RUNTIME_PERMISSION address=%016llx bytes=%zu permission=%u\n",(unsigned long long)uintptr_t(base()+offset),bytes,permission);
        const auto rc=operation("SetMemoryPermission",uintptr_t(base()+offset),0,bytes,svcSetMemoryPermission(base()+offset,bytes,permission));
        return R_SUCCEEDED(rc);
    }
    bool physical_map(uint64_t offset,size_t bytes) {
        if(poisoned_ || !valid(offset,bytes) || offset<0xa0000000 || bytes>0x20000000-(offset&0x1fffffff))return false;
        const uint64_t physical_offset=offset&0x1fffffff;
        if(physical_offset>physical_size_ || bytes>physical_size_-physical_offset)return false;
        if(!physical_) {
            unsigned index=0;const Result rc=reservation_.create_backing(physical_size_,index);
            if(R_FAILED(rc)){poisoned_=true;return false;}
            physical_=reservation_.backing(index);
        }
        auto next=aliases_.lower_bound(offset);
        if(next!=aliases_.end() && next->first<offset+bytes)return false;
        if(next!=aliases_.begin()) {auto previous=std::prev(next);if(previous->first+previous->second.bytes>offset)return false;}
        const uint64_t source=uintptr_t(physical_+physical_offset);
        const auto rc=operation("MapProcessMemory_physical",uintptr_t(base()+offset),source,bytes,
            svcMapProcessMemory(base()+offset,envGetOwnProcessHandle(),source,bytes));
        if(R_FAILED(rc))return false;
        aliases_.emplace(offset,Alias{source,bytes});return true;
    }
    bool physical_unmap(uint64_t offset,size_t bytes) {
        if(poisoned_ || !valid(offset,bytes))return false;
        auto it=aliases_.find(offset);
        if(it==aliases_.end() || it->second.bytes!=bytes)return false;
        const auto rc=operation("UnmapProcessMemory_physical",uintptr_t(base()+offset),it->second.source,bytes,
            svcUnmapProcessMemory(base()+offset,envGetOwnProcessHandle(),it->second.source,bytes));
        if(R_FAILED(rc)){poisoned_=true;return false;}
        aliases_.erase(it);return true;
    }
    Result close() {
        if(poisoned_)return MAKERESULT(Module_Libnx,LibnxError_BadInput);
        while(!aliases_.empty()) {const auto it=aliases_.begin();if(!physical_unmap(it->first,it->second.bytes))return MAKERESULT(Module_Libnx,LibnxError_BadInput);}
        while(!blocks_.empty()) {const auto it=blocks_.begin();if(!decommit(it->first,it->second.bytes))return MAKERESULT(Module_Libnx,LibnxError_BadInput);}
        return reservation_.close();
    }
};
