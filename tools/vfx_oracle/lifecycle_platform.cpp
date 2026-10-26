// Reuse the asset inspector's platform sinks, never its placeholder RNG or silent STOP.
#define gos_rand unused_mc2fx_rand
#define InternalFunctionStop unused_mc2fx_stop
#define gos_vfx_mesh_flush vfx_test_unused_mesh_flush
#include "tools/mc2fx/mc2fx_stubs.cpp"
#undef gos_vfx_mesh_flush
#undef gos_rand
#undef InternalFunctionStop
#include "mclib/det_rng.h"

namespace {
std::uint32_t* borrowedRngState = nullptr;
}
void vfx_test_bind_rng(std::uint32_t* state) { borrowedRngState = state; }
int __stdcall gos_rand()
{
    // The test owns the bound RNG state, so production gate configuration cannot affect its stream.
    if (!borrowedRngState) std::abort();
    return static_cast<int>(mc2_det_rng::next15(*borrowedRngState));
}
int InternalFunctionStop(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::abort();
}
int InternalFunctionStop(const char* message, const char* value)
{
    std::fprintf(stderr, "%s %s\n", message, value);
    std::abort();
}
