#pragma once
// libnx thread.h: 0x3b on cores 0..2 enables preemptive multithreading.
// Keep virtual Xbox priorities in guest metadata; mapping them to 0x2b/0x2c
// can starve decode/main/supervisor threads on the process default core.
constexpr int nx_guest_priority=0x3b;
inline Result nx_configure_guest_thread(Handle handle,int requested_priority) {
    u64 allowed=0;Result rc=svcGetInfo(&allowed,InfoType_PriorityMask,CUR_PROCESS_HANDLE,0);
    nx_runtime_log("THREAD_POLICY operation=allowed_priorities handle=%08x mask=%016llx rc=%08x\n",handle,(unsigned long long)allowed,rc);
    if(R_FAILED(rc))return rc;
    if(!(allowed&(1ull<<nx_guest_priority)))return MAKERESULT(Module_Libnx,LibnxError_BadInput);
    s32 core=-1;u64 mask=0;rc=svcGetThreadCoreMask(&core,&mask,handle);
    nx_runtime_log("THREAD_POLICY operation=core_mask handle=%08x preferred=%d mask=%016llx rc=%08x\n",handle,core,(unsigned long long)mask,rc);
    if(R_FAILED(rc))return rc;
    if(!mask || (mask&~7ull))return MAKERESULT(Module_Libnx,LibnxError_BadInput);
    rc=svcSetThreadPriority(handle,nx_guest_priority);
    nx_runtime_log("THREAD_POLICY operation=set_priority handle=%08x guest_request=%d native=%d rc=%08x\n",handle,requested_priority,nx_guest_priority,rc);
    if(R_FAILED(rc))return rc;
    s32 actual=0;rc=svcGetThreadPriority(&actual,handle);
    nx_runtime_log("THREAD_POLICY operation=verify_priority handle=%08x actual=%d expected=%d rc=%08x\n",handle,actual,nx_guest_priority,rc);
    if(R_FAILED(rc))return rc;
    return actual==nx_guest_priority?0:MAKERESULT(Module_Libnx,LibnxError_BadInput);
}
