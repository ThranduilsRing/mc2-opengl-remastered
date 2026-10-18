// tests/unit/test_render_contract_3.cpp
// CONTRACT-3: per-frame resource-ordering tracking unit tests.
// Standalone binary (own int main) -- no doctest dependency.
// Build: cmake --build build64-tests --config RelWithDebInfo --target test_render_contract_3
// Run:   build64-tests/RelWithDebInfo/test_render_contract_3.exe
//
// Tests the beginPass/endPass/frameBegin/getFrameViolationCount API added in
// render_contract.cpp. All tests call _test_forceOrderAudit(true) to bypass
// MC2_RENDER_PASS_ORDER env var requirement so CI (no env vars) still passes.
//
// Note: MSVC defines NDEBUG for RelWithDebInfo, disabling assert(). We use
// RC3_CHECK (always-active) so test failures abort rather than silently pass.

#include "render_contract.h"
#include "RenderCore/RenderPassContract.h"
#include "RenderCore/RenderResourceRegistry.h"
#include <cstdio>
#include <cstdlib>

// Always-active check: does not depend on NDEBUG.
// Prints the failing condition and aborts so CI sees a non-zero exit code.
#define RC3_CHECK(expr)                                                        \
    do {                                                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL: %s  (line %d)\n", #expr, __LINE__);        \
            fflush(stderr);                                                    \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

static void test_correct_order_no_violation() {
    render_contract::frameBegin();
    render_contract::beginPass(RenderCore::RenderPassId::Shadow);
    render_contract::endPass(RenderCore::RenderPassId::Shadow);
    render_contract::beginPass(RenderCore::RenderPassId::MechOpaque);
    render_contract::endPass(RenderCore::RenderPassId::MechOpaque);
    RC3_CHECK(render_contract::getFrameViolationCount() == 0);
    printf("PASS: correct_order_no_violation\n");
}

static void test_missing_writer_fires_violation() {
    render_contract::frameBegin();
    // MechOpaque reads ShadowDynamicMap -- not yet written (Shadow not called)
    render_contract::beginPass(RenderCore::RenderPassId::MechOpaque);
    render_contract::endPass(RenderCore::RenderPassId::MechOpaque);
    RC3_CHECK(render_contract::getFrameViolationCount() == 1);
    printf("PASS: missing_writer_fires_violation\n");
}

static void test_framebegin_resets_violations() {
    render_contract::frameBegin();
    render_contract::beginPass(RenderCore::RenderPassId::MechOpaque); // violation
    render_contract::endPass(RenderCore::RenderPassId::MechOpaque);
    RC3_CHECK(render_contract::getFrameViolationCount() == 1);
    render_contract::frameBegin(); // reset
    RC3_CHECK(render_contract::getFrameViolationCount() == 0);
    printf("PASS: framebegin_resets_violations\n");
}

static void test_vfx_reads_maindepth_after_geometry() {
    // Shadow pass first to satisfy ShadowDynamicMap dependency, then Terrain,
    // then VFX which reads MainDepth written by Terrain.
    render_contract::frameBegin();
    render_contract::beginPass(RenderCore::RenderPassId::Shadow);
    render_contract::endPass(RenderCore::RenderPassId::Shadow);
    render_contract::beginPass(RenderCore::RenderPassId::Terrain);
    render_contract::endPass(RenderCore::RenderPassId::Terrain);
    render_contract::beginPass(RenderCore::RenderPassId::VFX);
    render_contract::endPass(RenderCore::RenderPassId::VFX);
    RC3_CHECK(render_contract::getFrameViolationCount() == 0);
    printf("PASS: vfx_reads_maindepth_satisfied\n");
}

// Lossy PassIdentity -> RenderPassId mapping.
static void test_to_render_pass_id_mapping() {
    using PI = render_contract::PassIdentity;
    using R  = RenderCore::RenderPassId;
    RC3_CHECK(render_contract::toRenderPassId(PI::StaticProp)      == R::StaticPropOpaque);
    RC3_CHECK(render_contract::toRenderPassId(PI::ShadowCaster)    == R::Shadow);
    RC3_CHECK(render_contract::toRenderPassId(PI::Unknown)         == R::None);
    RC3_CHECK(render_contract::toRenderPassId(PI::OpaqueObject)    == R::MechOpaque);
    RC3_CHECK(render_contract::toRenderPassId(PI::AlphaObject)     == R::MechOpaque);
    RC3_CHECK(render_contract::toRenderPassId(PI::DebugOverlay)    == R::None);
    RC3_CHECK(render_contract::toRenderPassId(PI::Grass)           == R::Terrain);
    RC3_CHECK(render_contract::toRenderPassId(PI::VegetationCards) == R::VegetationCards);
    RC3_CHECK(render_contract::toRenderPassId(PI::PostProcess)     == R::PostProcess);
    printf("PASS: to_render_pass_id_mapping\n");
}

int main() {
    // init from env vars first, then force-enable so CI (no env var) still runs.
    render_contract::initRenderPassOrder();
    render_contract::_test_forceOrderAudit(true);

    test_correct_order_no_violation();
    test_missing_writer_fires_violation();
    test_framebegin_resets_violations();
    test_vfx_reads_maindepth_after_geometry();
    test_to_render_pass_id_mapping();
    printf("All render_contract_3 tests passed.\n");
    return 0;
}
