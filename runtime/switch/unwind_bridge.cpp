#include "ppc_recomp_shared.h"
extern "C" PPC_FUNC(nx_unwind_throw);
// Built with the exact CPU archive flags, including -fno-exceptions. This checks
// the target's unwind metadata before exceptions traverse real generated frames.
extern "C" __attribute__((noinline)) PPC_FUNC(nx_unwind_bridge) {
    nx_unwind_throw(ctx,base);
    ctx.r12.u64=0x1234; // Prevent a tail call from removing this frame.
}
