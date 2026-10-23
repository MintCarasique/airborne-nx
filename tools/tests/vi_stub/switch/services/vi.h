#pragma once
// Host-only VI lifecycle test seam; never on the target compiler include path.
#include <cstdint>
using Result=uint32_t;
struct ViDisplay {uint64_t display_id=0;char name[64]{};bool initialized=false;};
#define R_SUCCEEDED(result) ((result)==0)
