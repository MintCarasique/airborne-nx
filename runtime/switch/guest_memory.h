#pragma once
#include <switch.h>
#include "ppc_context.h"
#include <malloc.h>
#include <array>
#include <cstring>

// Small proof of the address model, not a complete guest heap manager.
// Normal heap pages cannot be MapProcessMemory sources. First borrow them into
// an AliasCodeData staging view, then create guest aliases from that view.
class NxGuestMemory {
    struct Backing {
        uint8_t* original=nullptr;uint8_t* bytes=nullptr;size_t size=0;
        VirtmemReservation* reservation=nullptr;bool borrowed=false;
    };
    struct Mapping { uint8_t* destination=nullptr;unsigned backing=0; };
    std::array<Backing,8> backing_{};
    std::array<Mapping,16> mappings_{};
    unsigned backing_count_=0,mapping_count_=0;
    VirtmemReservation* reservation_=nullptr;
    Handle process_=INVALID_HANDLE;
    using Log=void (*)(const char*,...);
    Log log_=nullptr;
    Result fail(const char* stage,Result result){
        failure_stage=stage;failure_result=result;
        if(log_)log_("MEM_RESULT stage=%s rc=%08x module=%u description=%u\n",stage,result,R_MODULE(result),R_DESCRIPTION(result));
        return result;
    }
    void describe(const char* label,const void* pointer,size_t bytes) {
        if(!log_)return;
        const uint64_t end=uint64_t(uintptr_t(pointer))+bytes;
        uint64_t cursor=uintptr_t(pointer);
        for(unsigned segments=0;cursor<end && segments<128;++segments) {
            MemoryInfo info{};uint32_t page=0;Result rc=svcQueryMemory(&info,&page,cursor);
            log_("MEM_REGION label=%s address=%016llx remaining=%llu query_rc=%08x region=%016llx size=%llu type=%08x attr=%08x perm=%08x ipc_refs=%u device_refs=%u\n",
                label,(unsigned long long)cursor,(unsigned long long)(end-cursor),rc,(unsigned long long)info.addr,
                (unsigned long long)info.size,info.type,info.attr,info.perm,info.ipc_refcount,info.device_refcount);
            if(R_FAILED(rc) || !info.size || info.addr+info.size<=cursor)break;
            cursor=info.addr+info.size;
        }
        if(cursor<end)log_("MEM_REGION_SCAN incomplete label=%s next=%016llx end=%016llx\n",label,(unsigned long long)cursor,(unsigned long long)end);
    }
public:
    explicit NxGuestMemory(Log logger=nullptr):log_(logger){}
    uint8_t* base=nullptr;
    const char* failure_stage="none";
    Result failure_result=0;
    static constexpr size_t span=0x100000000ull;
    static bool valid_range(uint32_t guest,size_t size) {
        if(!size || (guest&4095) || (size&4095))return false;
        if(guest<0xe0000000 && size>uint64_t(0xe0000000)-guest)return false;
        const uint64_t translated=uint64_t(guest)+(guest>=0xe0000000 ? 0x1000:0);
        return translated<span && size<=span-translated;
    }
    Result open() {
        if(base)return fail("already_open",MAKERESULT(Module_Libnx,LibnxError_BadInput));
        for(unsigned number:{0x73u,0x74u,0x75u,0x77u,0x78u})if(!envIsSyscallHinted(number))
            return fail("required_mapping_SVC_not_hinted",MAKERESULT(Module_Libnx,LibnxError_IncompatSysVer));
        process_=envGetOwnProcessHandle();
        if(process_==INVALID_HANDLE)return fail("own_process_handle_missing",MAKERESULT(Module_Libnx,LibnxError_BadInput));
        virtmemLock();
        base=static_cast<uint8_t*>(virtmemFindAslr(span,0x10000));
        if(base)reservation_=virtmemAddReservation(base,span);
        virtmemUnlock();
        if(!reservation_){base=nullptr;return fail("reserve_4GiB",MAKERESULT(Module_Libnx,LibnxError_OutOfMemory));}
        return 0;
    }
    Result create_backing(size_t size,unsigned& index) {
        if(!base || !size || (size&4095) || backing_count_==backing_.size())
            return fail("backing_arguments",MAKERESULT(Module_Libnx,LibnxError_BadInput));
        auto* original=static_cast<uint8_t*>(memalign(0x1000,size));
        if(!original)return fail("backing_allocation",MAKERESULT(Module_Libnx,LibnxError_OutOfMemory));
        std::memset(original,0,size);index=backing_count_++;
        auto& b=backing_[index];b.original=original;b.size=size;
        virtmemLock();b.bytes=static_cast<uint8_t*>(virtmemFindCodeMemory(size,0x1000));
        if(b.bytes)b.reservation=virtmemAddReservation(b.bytes,size);
        virtmemUnlock();
        if(!b.reservation)return fail("reserve_staging_view",MAKERESULT(Module_Libnx,LibnxError_OutOfMemory));
        describe("heap_before_borrow",original,size);describe("staging_before_borrow",b.bytes,size);
        Result rc=svcMapProcessCodeMemory(process_,uintptr_t(b.bytes),uintptr_t(original),size);
        if(log_)log_("MEM_OP operation=MapProcessCodeMemory handle=%08x source=%016llx destination=%016llx size=%llu rc=%08x\n",process_,(unsigned long long)uintptr_t(original),(unsigned long long)uintptr_t(b.bytes),(unsigned long long)size,rc);
        if(R_FAILED(rc))return fail("svcMapProcessCodeMemory",rc);
        b.borrowed=true;
        rc=svcSetProcessMemoryPermission(process_,uintptr_t(b.bytes),size,Perm_Rw);
        if(log_)log_("MEM_OP operation=SetProcessMemoryPermission address=%016llx size=%llu permission=RW rc=%08x\n",(unsigned long long)uintptr_t(b.bytes),(unsigned long long)size,rc);
        describe("staging_after_permission",b.bytes,size);
        if(R_FAILED(rc))return fail("svcSetProcessMemoryPermission",rc);
        return 0;
    }
    uint8_t* backing(unsigned index){return index<backing_count_ ? backing_[index].bytes:nullptr;}
    Result map(uint32_t guest,unsigned index) {
        if(!base || index>=backing_count_ || mapping_count_==mappings_.size() || !valid_range(guest,backing_[index].size))
            return fail("mapping_arguments",MAKERESULT(Module_Libnx,LibnxError_BadInput));
        auto* destination=ppc_guest_ptr(base,guest);auto& source=backing_[index];
        const auto begin=uintptr_t(destination),end=begin+source.size;
        for(unsigned i=0;i<mapping_count_;++i) {
            const auto other=uintptr_t(mappings_[i].destination),other_end=other+backing_[mappings_[i].backing].size;
            if(begin<other_end && other<end)return fail("mapping_overlap",MAKERESULT(Module_Libnx,LibnxError_BadInput));
        }
        describe("guest_mapping_source",source.bytes,source.size);describe("guest_mapping_destination",destination,source.size);
        const Result rc=svcMapProcessMemory(destination,process_,uintptr_t(source.bytes),source.size);
        if(log_)log_("MEM_OP operation=MapProcessMemory handle=%08x guest=%08x source=%016llx destination=%016llx size=%llu rc=%08x\n",process_,guest,(unsigned long long)uintptr_t(source.bytes),(unsigned long long)uintptr_t(destination),(unsigned long long)source.size,rc);
        if(R_FAILED(rc))return fail("svcMapProcessMemory",rc);
        describe("guest_mapping_after",destination,source.size);
        mappings_[mapping_count_++]={destination,index};return 0;
    }
    Result close() {
        // On an unmap failure keep backing/reservation alive; the caller must
        // terminate this process rather than returning a live alias to hbloader.
        while(mapping_count_) {
            const auto& mapping=mappings_[mapping_count_-1];const auto& source=backing_[mapping.backing];
            const Result rc=svcUnmapProcessMemory(mapping.destination,process_,uintptr_t(source.bytes),source.size);
            if(log_)log_("MEM_OP operation=UnmapProcessMemory handle=%08x source=%016llx destination=%016llx size=%llu rc=%08x\n",process_,(unsigned long long)uintptr_t(source.bytes),(unsigned long long)uintptr_t(mapping.destination),(unsigned long long)source.size,rc);
            if(R_FAILED(rc))return fail("svcUnmapProcessMemory",rc);
            --mapping_count_;
        }
        while(backing_count_) {
            auto& b=backing_[backing_count_-1];
            if(b.borrowed) {
                const Result rc=svcUnmapProcessCodeMemory(process_,uintptr_t(b.bytes),uintptr_t(b.original),b.size);
                if(log_)log_("MEM_OP operation=UnmapProcessCodeMemory destination=%016llx source=%016llx size=%llu rc=%08x\n",(unsigned long long)uintptr_t(b.bytes),(unsigned long long)uintptr_t(b.original),(unsigned long long)b.size,rc);
                if(R_FAILED(rc))return fail("svcUnmapProcessCodeMemory",rc);
                b.borrowed=false;
            }
            if(b.reservation){virtmemLock();virtmemRemoveReservation(b.reservation);virtmemUnlock();}
            std::free(b.original);b={};--backing_count_;
        }
        if(reservation_) {
            virtmemLock();virtmemRemoveReservation(reservation_);virtmemUnlock();reservation_=nullptr;
        }
        base=nullptr;return 0;
    }
    // The probe explicitly closes and logs failures before offering an exit.
    ~NxGuestMemory()=default;
};
