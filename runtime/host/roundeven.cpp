#include <cstdint>
#include <cstring>

// LLVM may lower SIMDe's nearest-even rounding to this C23 libcall, which
// the bundled Windows UCRT does not export. Work on binary32 bits so guest
// rounding-mode changes cannot affect the result. This is a host-only shim.
extern "C" float roundevenf(float value) noexcept {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7fffffffu;
    const unsigned exponent = magnitude >> 23;
    if (exponent >= 150) return value; // Already integral, infinity, or NaN.
    if (exponent < 127) {
        bits = sign | (magnitude > 0x3f000000u ? 0x3f800000u : 0u);
    } else {
        const unsigned shift = 150 - exponent;
        const uint32_t mask = (1u << shift) - 1;
        const uint32_t remainder = magnitude & mask;
        const uint32_t midpoint = 1u << (shift - 1);
        uint32_t rounded = magnitude & ~mask;
        if (remainder > midpoint ||
            (remainder == midpoint && ((rounded >> shift) & 1u)))
            rounded += 1u << shift;
        bits = sign | rounded;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
