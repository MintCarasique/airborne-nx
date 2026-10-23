#include "../../runtime/host/guest_fp.h"
#include <cstdio>
#include <cmath>
#include <stdexcept>
int main() {
    PPCContext ctx{};
    const auto original=ctx.fpscr.getcsr();
    initialize_guest_fp(ctx);
    auto require=[](bool ok){if(!ok)throw std::runtime_error("Guest FP startup regression");};
    require(ctx.fpscr.csr==ctx.fpscr.getcsr() && !ctx.fpscr.guestFlags);
#if defined(__x86_64__) || defined(_M_X64)
    require((ctx.fpscr.csr&0x1f80)==0x1f80);
#elif defined(__aarch64__)
    require(!(ctx.fpscr.csr&0x9f00));
#endif
    ctx.fpscr.enableFlushMode();
    volatile double numerator=1.0,denominator=10.0;
    volatile double quotient=numerator/denominator;
    require(std::isfinite(quotient) && quotient>0 && quotient<1);
    const auto guest_control=ctx.fpscr.getcsr();
    {GuestFpScope scope(ctx);ctx.fpscr.setcsr(guest_control ^ PPCFPSCRRegister::RoundMask);}
    require(ctx.fpscr.getcsr()==guest_control);
    ctx.fpscr.disableFlushMode();
    require(!(ctx.fpscr.getcsr()&PPCFPSCRRegister::FlushMask));
    ctx.fpscr.setcsr(original);
    std::puts("Guest FP bootstrap, inexact arithmetic, flush and API restoration passed");
}
