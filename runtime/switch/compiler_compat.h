#pragma once
#include <cstdint>

// The existing emitter uses Clang rotate builtins. The Switch GCC build aliases
// those two names to these helpers through compiler flags, without rewriting
// generated functions or changing the already validated host build.
constexpr uint32_t nx_ppc_rotl32(uint32_t value,unsigned count) noexcept {
    const unsigned shift=count&31;
    return (value<<shift)|(value>>((0u-shift)&31));
}
constexpr uint64_t nx_ppc_rotl64(uint64_t value,unsigned count) noexcept {
    const unsigned shift=count&63;
    return (value<<shift)|(value>>((0u-shift)&63));
}
