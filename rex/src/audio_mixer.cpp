// Native kernels of the game's software audio mixer (RWAudioCore).
//
// On the Xbox 360 the XMA hardware decodes and a whole hardware thread mixes; here the mixer runs as
// recompiled PowerPC on three shared cores, and in Adanti's battles the game's audio thread (RWAudioCore
// Dac) took 50-60 % of a core and still missed its deadlines (audout requests without data: the sound
// crackled). The Switch profile put its hottest function at sub_82989E20, the mix-add kernel:
//
//     sub_82989E20(float* dst = r3, const float* src = r4, float gain = f1, uint32 count = r6)
//         dst[i] += src[i] * gain        (big-endian floats in guest memory)
//
// With dst and src 128-byte aligned and count a multiple of 64 it runs VMX128 (vmulfp128 then vaddfp,
// two roundings, denormals flushed); otherwise a scalar fmadds loop. The recompiled vector code goes
// through the register structure in memory for every instruction; this does the same arithmetic on
// host registers.
//
// audio_native_mixer turns it on. Like the renderer's NEON index path, the first 2,000 calls (and then
// 1 in 4,096) also run the original on a copy of dst and compare bit for bit; at the first difference
// the native path switches itself off for the rest of the session and the original's result is kept.

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("fp-contract=off")
#endif

#include "airborne_pch.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

REXCVAR_DEFINE_BOOL(audio_native_mixer, true, "Airborne",
                    "Run the game's audio mix-add kernel (sub_82989E20) natively instead of the recompiled "
                    "VMX128 loop; it checks itself against the original and switches off at a difference");

REX_EXTERN(__imp__sub_82989E20);

namespace {

std::atomic<bool> g_mixer_off{false};
std::atomic<uint64_t> g_mixer_calls{0};
std::atomic<uint64_t> g_mixer_checked{0};

inline float LoadBe(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

inline void StoreBe(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

void MixAddNative(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, count = ctx.r6.u32;
  uint8_t* d = REX_RAW_ADDR(dst);
  const uint8_t* s = REX_RAW_ADDR(src);
  if (((dst | src) & 0x7F) == 0 && (count & 0x3F) == 0) {
    // Denormals are flushed by the host flush mode, set here like the original sets it (FTZ/DAZ on x86,
    // FZ on ARM), so plain float arithmetic matches VMX128 and the loop vectorizes.
    ctx.fpscr.enableFlushMode();
    const float gain = float(ctx.f1.f64);
    for (uint32_t i = 0; i < count; ++i) {
      const float product = gain * LoadBe(s + i * 4);
      StoreBe(d + i * 4, product + LoadBe(d + i * 4));
    }
  } else {
    ctx.fpscr.disableFlushMode();
    const double gain = ctx.f1.f64;
    for (uint32_t i = 0; i < count; ++i) {
      const double value = std::fma(double(LoadBe(s + i * 4)), gain, double(LoadBe(d + i * 4)));
      StoreBe(d + i * 4, float(value));
    }
  }
}

}  // namespace

REX_HOOK_RAW(sub_82989E20) {
  if (!REXCVAR_GET(audio_native_mixer) || g_mixer_off.load(std::memory_order_relaxed)) {
    __imp__sub_82989E20(ctx, base);
    return;
  }
  const uint64_t n = g_mixer_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool check = n <= 2000 || (n & 4095) == 0;
  if (!check) {
    MixAddNative(ctx, base);
    return;
  }
  // Self-check: native on dst, keep its result, put dst back, run the original, compare.
  const uint32_t count = ctx.r6.u32;
  uint8_t* d = REX_RAW_ADDR(ctx.r3.u32);
  const size_t bytes = size_t(count) * 4;
  thread_local std::vector<uint8_t> before, native;
  before.assign(d, d + bytes);
  PPCContext saved = ctx;
  MixAddNative(ctx, base);
  native.assign(d, d + bytes);
  std::memcpy(d, before.data(), bytes);
  ctx = saved;
  __imp__sub_82989E20(ctx, base);
  const uint64_t checked = g_mixer_checked.fetch_add(1, std::memory_order_relaxed) + 1;
  if (bytes && std::memcmp(native.data(), d, bytes) != 0) {
    size_t at = 0;
    while (at + 4 <= bytes && std::memcmp(native.data() + at, d + at, 4) == 0) {
      at += 4;
    }
    g_mixer_off.store(true, std::memory_order_relaxed);
    REXLOG_ERROR("[audio] native mix-add DIFFERS from the original (call {}, {} floats, {} path, first at float "
                 "{}: native {:08X} original {:08X}); native mixer off for the session",
                 n, count, ((saved.r3.u32 | saved.r4.u32) & 0x7F) == 0 && (count & 0x3F) == 0 ? "vector" : "scalar",
                 at / 4, __builtin_bswap32(*reinterpret_cast<const uint32_t*>(native.data() + at)),
                 __builtin_bswap32(*reinterpret_cast<const uint32_t*>(d + at)));
    return;
  }
  if (checked == 2000) {
    REXLOG_INFO("[audio] native mix-add: 2,000 calls identical to the original; now checking 1 in 4,096");
  }
}
