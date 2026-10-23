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
#include <map>
#include <vector>

// Guest reservations are tracked separately from the host's 4 GiB reservation.
// Only the diagnostic user-virtual arenas are supported here; physical aliases,
// reset and 16 MiB pages need separate implementations.
class GuestVirtualAllocator {
    struct Region { uint32_t size,page; std::vector<uint32_t> protections; };
    uint8_t* base_;
    std::map<uint32_t,Region> regions_;
    static uint64_t align_up(uint64_t v,uint32_t page){return (v+page-1)&~uint64_t(page-1);}
    static DWORD host_protection(uint32_t p) {
        switch(p) {case 1:return PAGE_NOACCESS;case 2:return PAGE_READONLY;case 4:return PAGE_READWRITE;default:return 0;}
    }
    auto containing(uint32_t address) {
        auto it=regions_.upper_bound(address);
        if(it==regions_.begin())return regions_.end();
        --it;return uint64_t(address)<uint64_t(it->first)+it->second.size ? it : regions_.end();
    }
public:
    explicit GuestVirtualAllocator(uint8_t* base):base_(base){}
    uint32_t query_protection(uint32_t address) const {
        auto it=regions_.upper_bound(address);if(it==regions_.begin())return 0;--it;
        if(uint64_t(address)>=uint64_t(it->first)+it->second.size)return 0;
        return it->second.protections[(address-it->first)/it->second.page];
    }
    uint64_t reserved_bytes() const {
        uint64_t total=0;for(const auto& pair:regions_)total+=pair.second.size;return total;
    }
    uint64_t committed_bytes() const {
        uint64_t total=0;for(const auto& pair:regions_)for(const auto protection:pair.second.protections)if(protection)total+=pair.second.page;return total;
    }
    uint32_t allocate(uint32_t& address,uint32_t& bytes,uint32_t flags,uint32_t protection) {
        constexpr uint32_t supported=0x1000|0x2000|0x100000|0x800000|0x20000000|0x40000000;
        const bool reserve=flags&0x2000,commit=flags&0x1000;
        if(!bytes || !(reserve||commit) || (flags&~supported) || !host_protection(protection))return 0xc000000d;
        auto existing=containing(address);
        const uint32_t page=address && existing!=regions_.end() ? existing->second.page : (flags&0x20000000 ? 65536:4096);
        const uint64_t magnitude=(bytes&0x80000000u) ? uint64_t(0x100000000ull-bytes) : bytes;
        const uint64_t size=align_up(magnitude,page);
        const uint32_t low=page==65536 ? 0x40000000 : 0x20000000;
        const uint64_t high=page==65536 ? 0x60000000 : 0x40000000;
        uint64_t start=address & ~uint64_t(page-1);
        if(size>high-low)return 0xc0000017;
        if(!address) {
            // Null-base COMMIT also establishes a reservation.
            start=flags&0x100000 ? high-size : low;
            if(flags&0x100000) {
                for(auto it=regions_.rbegin();it!=regions_.rend();++it) {
                    if(uint64_t(it->first)>=start+size || uint64_t(it->first)+it->second.size<=start)continue;
                    if(it->first<low+size)return 0xc0000017;
                    start=(uint64_t(it->first)-size)&~uint64_t(page-1);
                }
            } else {
                for(const auto& pair:regions_) {
                    if(uint64_t(pair.first)+pair.second.size<=start)continue;
                    if(pair.first>=start+size)break;
                    start=align_up(uint64_t(pair.first)+pair.second.size,page);
                }
            }
        }
        if(start<low || start+size>high)return 0xc0000018;
        existing=containing(uint32_t(start));
        bool created=false;
        if(existing==regions_.end()) {
            if(address && !reserve)return 0xc0000018;
            auto next=regions_.lower_bound(uint32_t(start));
            if(next!=regions_.end() && next->first<start+size)return 0xc0000018;
            existing=regions_.emplace(uint32_t(start),Region{uint32_t(size),page,std::vector<uint32_t>(size/page,0)}).first;
            created=true;
        } else if(reserve) return 0xc0000018;
        if(start+size>uint64_t(existing->first)+existing->second.size)return 0xc0000018;
        if(commit) {
            auto& region=existing->second;
            const uint32_t first=(uint32_t(start)-existing->first)/page;
            // Recommit preserves bytes and protections of already committed pages.
            // Fresh pages are zero-filled by VirtualAlloc, including NOZERO requests
            // (which permit, but do not require, skipping zero initialization).
            for(uint32_t i=0;i<size/page;++i) {
                if(region.protections[first+i])continue;
                if(!VirtualAlloc(base_+start+uint64_t(i)*page,page,MEM_COMMIT,host_protection(protection))) {
                    if(created) {
                        VirtualFree(base_+start,size,MEM_DECOMMIT);regions_.erase(existing);
                    }
                    return 0xc0000017;
                }
                region.protections[first+i]=protection;
            }
        }
        address=uint32_t(start);bytes=uint32_t(size);return 0;
    }
    uint32_t free(uint32_t& address,uint32_t& bytes,uint32_t flags) {
        auto it=containing(address);
        if(it==regions_.end())return 0xc00000a0;
        auto& region=it->second;
        if(flags==0x8000) {
            if(address!=it->first)return 0xc000000d;
            if(!VirtualFree(base_+address,region.size,MEM_DECOMMIT))return 0xc0000001;
            bytes=region.size;regions_.erase(it);return 0;
        }
        if(flags!=0x4000 || !bytes)return 0xc000000d;
        const uint32_t start=address&~(region.page-1);
        const uint64_t size=align_up(bytes,region.page);
        if(uint64_t(start)+size>uint64_t(it->first)+region.size)return 0xc000000d;
        if(!VirtualFree(base_+start,size,MEM_DECOMMIT))return 0xc0000001;
        for(uint32_t i=(start-it->first)/region.page;i<(start-it->first+size)/region.page;++i)region.protections[i]=0;
        address=start;bytes=uint32_t(size);return 0;
    }
    uint32_t protect(uint32_t& address,uint32_t& bytes,uint32_t protection,uint32_t& old) {
        if(!bytes || !host_protection(protection))return 0xc000000d;
        auto it=containing(address);if(it==regions_.end())return 0xc0000018;
        auto& r=it->second;
        const uint32_t start=address&~(r.page-1);
        const uint64_t size=align_up(bytes,r.page);
        if(uint64_t(start)+size>uint64_t(it->first)+r.size)return 0xc0000018;
        unsigned first=(start-it->first)/r.page;
        for(unsigned i=0;i<size/r.page;++i)if(!r.protections[first+i])return 0xc0000018;
        DWORD previous;
        if(!VirtualProtect(base_+start,size,host_protection(protection),&previous))return 0xc0000001;
        old=r.protections[first];
        for(unsigned i=0;i<size/r.page;++i)r.protections[first+i]=protection;
        address=start;bytes=uint32_t(size);return 0;
    }
};
