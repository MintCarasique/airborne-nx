#include "ppc_context.h"
static_assert(PPCFPSCRRegister::HostToGuest[0] == PPC_ROUND_NEAREST);
static_assert(PPCFPSCRRegister::HostToGuest[1] == PPC_ROUND_UP);
static_assert(PPCFPSCRRegister::HostToGuest[2] == PPC_ROUND_DOWN);
static_assert(PPCFPSCRRegister::HostToGuest[3] == PPC_ROUND_TOWARD_ZERO);
static_assert(PPCFPSCRRegister::GuestToHost[PPC_ROUND_UP] == (1u << 22));
static_assert(PPCFPSCRRegister::GuestToHost[PPC_ROUND_DOWN] == (2u << 22));
void check_arm64_state(PPCContext& ctx) {
    ctx.fpscr.loadFromHost();
    ctx.fpscr.storeFromGuest(0, 0xff);
    ctx.fpscr.reciprocalSqrt(ctx.f13, ctx.f0.u64);
}
