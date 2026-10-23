#pragma once
#include "diagnostic.h"

// A zeroed PPCContext is not a native FP control state on x86: MXCSR=0
// enables all host FP traps. Guest exceptions are represented in guestFlags;
// they must never be enabled as native processor traps.
inline void initialize_guest_fp(PPCContext& ctx) {
    uint32_t control=ctx.fpscr.getcsr();
    control&=~uint32_t(PPCFPSCRRegister::RoundMask|PPCFPSCRRegister::FlushMask);
#if defined(__x86_64__) || defined(_M_X64)
    control=(control&~0x3fu)|0x1f80u;
#elif defined(__aarch64__) || defined(_M_ARM64)
    control&=~0x9f00u;
#endif
    ctx.fpscr.csr=control;ctx.fpscr.guestFlags=0;ctx.fpscr.setcsr(control);
}

// Native APIs (including Vulkan drivers) may modify the thread's FP control.
// Restore the incoming guest rounding/flush/trap mask at the API boundary.
class GuestFpScope {
    PPCFPSCRRegister& register_;
    uint32_t saved_;
public:
    explicit GuestFpScope(PPCContext& ctx):register_(ctx.fpscr),saved_(register_.getcsr()){}
    ~GuestFpScope(){register_.setcsr(saved_);}
};
