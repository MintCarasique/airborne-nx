// Memory ordering of UE3's render command queue on the Switch.
//
// The game thread writes a command into the ring, then publishes the write offset; the render thread
// (loop sub_82404CD8) asks sub_823F8448 for the next command, which compares the read and write offsets and
// returns a pointer into the ring, then calls through the command's vtable. The reader has no barrier
// between the offset loads and the command loads: Xenon's in-order cores kept them in order, the Switch's
// Cortex-A57 (out of order) and the host compiler need not. Build a0160465babc died that way in Adanti after
// 95 s: the render thread called guest address 0x4354A232 from sub_82404CD8 (rex_crash.log, abort in
// InvalidFunctionTrap), the same loop that read the vtable 0x3E848C6D in run 9.
//
// The hook runs the original and, when it found a command (r3 = 1), adds an acquire fence: the offset loads
// done inside it are ordered before every load of the command that follows.

#include "airborne_pch.h"

#include <atomic>

#include <rex/hook.h>

REX_EXTERN(__imp__sub_823F8448);

REX_HOOK_RAW(sub_823F8448) {
  __imp__sub_823F8448(ctx, base);
  if (ctx.r3.u32 != 0) {
    std::atomic_thread_fence(std::memory_order_acquire);
  }
}
