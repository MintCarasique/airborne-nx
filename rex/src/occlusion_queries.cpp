// Instant answers for the game's occlusion queries.
//
// UE3 reads each occlusion query result in a busy loop (0x82453D70):
//     do r = D3DQuery_GetData(query, &pixels, 4, D3DGETDATA_FLUSH);
//     while (r == S_FALSE || r < 0);
// On the Xbox 360 the GPU writes the result within microseconds. Under GPU
// emulation it appears only when the command processor thread reaches the
// query in the ring buffer, so the game thread spins on a whole core: on the
// Switch the menu ran at 0.7 FPS with that thread at 100 % (SDK profiler
// stacks: sub_829B0E38 called from sub_82453C88, 87 % of samples).
//
// sub_829B0E38 is the XDK's D3DQuery_GetData (query type at +4; 9 is
// D3DQUERYTYPE_OCCLUSION). With occlusion_queries_instant, occlusion queries
// report "visible" immediately; every other query goes to the original. The
// cost is lost occlusion culling: objects the GPU would have hidden are drawn.

#include "airborne_pch.h"

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(occlusion_queries_instant, true, "Airborne",
                    "Answer occlusion queries as visible at once instead of waiting for the "
                    "emulated GPU (UE3 spins on them)");

REX_EXTERN(__imp__sub_829B0E38);

namespace {

constexpr uint32_t kQueryTypeOffset = 4;
constexpr uint32_t kD3DQueryTypeOcclusion = 9;
// Any non-zero count means visible to UE3; a large one keeps it so even if a
// threshold is applied.
constexpr uint32_t kVisiblePixels = 0x10000;
constexpr uint32_t kS_OK = 0;

}  // namespace

REX_HOOK_RAW(sub_829B0E38) {
  const uint32_t query = ctx.r3.u32;
  if (REXCVAR_GET(occlusion_queries_instant) && query != 0 &&
      REX_LOAD_U32(query + kQueryTypeOffset) == kD3DQueryTypeOcclusion) {
    const uint32_t data = ctx.r4.u32;
    if (data != 0 && ctx.r5.u32 >= 4) {
      REX_STORE_U32(data, kVisiblePixels);
    }
    ctx.r3.u64 = kS_OK;
    return;
  }
  __imp__sub_829B0E38(ctx, base);
}
