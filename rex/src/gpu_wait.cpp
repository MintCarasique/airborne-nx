#if defined(__aarch64__)
// A sleeping D3D wait for the GPU.
//
// The XDK's D3D waits for the GPU in a loop around 0x829B6490 (callers at 0x829AED1C, 0x829AEC78 and
// 0x829BCB90): each call spins 4 x 8 db16cyc, checks whether the GPU's read pointer moved and returns
// 1 while it should keep waiting (0 after its 5000 ms hang timeout). On the Xbox 360 the GPU catches
// up within microseconds. Here the GPU is the native renderer's ring thread, which needs a core to
// catch up: on the Switch UE3's rendering thread spent 90 % of a core in this loop (PC profiler:
// 76 % of its samples in sub_829B6490), taking it from the ring thread, the main thread and the
// audio mixer. With gpu_wait_sleep_us, each step that has to keep waiting sleeps instead.

#include "airborne_pch.h"

#include <chrono>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/thread.h>

REXCVAR_DEFINE_INT32(gpu_wait_sleep_us, 100, "Airborne",
                     "Microseconds the D3D wait for the GPU (sub_829B6490) sleeps per step while the "
                     "GPU has not caught up. 0 keeps the original busy wait");

REX_EXTERN(__imp__sub_829B6490);

REX_HOOK_RAW(sub_829B6490) {
  __imp__sub_829B6490(ctx, base);
  const int32_t sleep_us = REXCVAR_GET(gpu_wait_sleep_us);
  if (ctx.r3.u32 == 1 && sleep_us > 0) {
    rex::thread::Sleep(std::chrono::microseconds(sleep_us));
  }
}
#endif
