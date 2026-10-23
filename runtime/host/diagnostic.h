#pragma once
#include "ppc_recomp_shared.h"
#include <stdexcept>
#include <string>
struct DiagnosticStop : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void host_import(const char* name, PPCContext& ctx, uint8_t* base);
size_t host_service_self_test(uint8_t* base);
