#include "doctest.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <cstdlib>
#if defined(_WIN32)
#  include <stdlib.h>
#endif

#include "render_contract.h"
#include "RenderCore/RenderPassContract.h"
#include "RenderCore/RenderResourceRegistry.h"
#include "Gates/gate_cold.h"
#include "contract_test_support.h"

namespace {
namespace rc = render_contract;
using PI  = render_contract::PassIdentity;
using RPI = RenderCore::RenderPassId;

struct RenderContractFixture {
    ContractEnvironment scopeEnabled{"MC2_RENDER_PASS_CONTRACT_ASSERT", "1"};
    RenderContractFixture() {
        mc2::gate::markMainEntered();
        rc::initRenderPassScope();
        rc::initRenderPassOrder();
        rc::_test_forceOrderAudit(true);
    }
    ~RenderContractFixture() {
        rc::renderPassScopeFrameBoundary();
        rc::_test_forceOrderAudit(false);
        rc::frameBegin();
    }
};

TEST_CASE_FIXTURE(RenderContractFixture, "render_state_contract_harness/balanced_begin_end") {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::beginPassScope(PI::PostProcess, "harness");
    rc::endPassScope(PI::PostProcess, "harness");
    CHECK((rc::getPassScopeViolationCount() - before == 0));
    return;
}

TEST_CASE_FIXTURE(RenderContractFixture, "render_state_contract_harness/end_without_begin") {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::endPassScope(PI::UI, "harness");
    CHECK((rc::getPassScopeViolationCount() - before == 1));
    return;
}

TEST_CASE_FIXTURE(RenderContractFixture, "render_state_contract_harness/owner_mismatch") {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::beginPassScope(PI::TerrainOverlay, "outer");
    rc::endPassScope(PI::PostProcess, "wrong-owner");
    CHECK((rc::getPassScopeViolationCount() - before == 1));
    rc::renderPassScopeFrameBoundary();
    return;
}

TEST_CASE_FIXTURE(RenderContractFixture, "render_state_contract_harness/nested_scopes_ok") {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::beginPassScope(PI::TerrainOverlay, "outer");
    rc::beginPassScope(PI::TerrainDecal, "inner");
    rc::endPassScope(PI::TerrainDecal, "inner");
    rc::endPassScope(PI::TerrainOverlay, "outer");
    CHECK((rc::getPassScopeViolationCount() - before == 0));
    return;
}

TEST_CASE_FIXTURE(RenderContractFixture, "render_state_contract_harness/missing_end_at_frame_boundary") {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::beginPassScope(PI::PostProcess, "leaked");
    rc::renderPassScopeFrameBoundary();
    CHECK((rc::getPassScopeViolationCount() - before >= 1));
    return;
}

TEST_CASE_FIXTURE(RenderContractFixture, "render_state_contract_harness/violation_counter_increments") {
    const uint32_t before = rc::getPassScopeViolationCount();
    rc::endPassScope(PI::DebugOverlay, "underflow");
    CHECK((rc::getPassScopeViolationCount() > before));
    return;
}

TEST_CASE_FIXTURE(RenderContractFixture, "render_state_contract_harness/order_correct_no_violation") {
    rc::frameBegin();
    rc::beginPass(RPI::Shadow);     rc::endPass(RPI::Shadow);
    rc::beginPass(RPI::MechOpaque); rc::endPass(RPI::MechOpaque);
    CHECK((rc::getFrameViolationCount() == 0));
    return;
}

TEST_CASE_FIXTURE(RenderContractFixture, "render_state_contract_harness/order_missing_writer_violation") {
    rc::frameBegin();
    rc::beginPass(RPI::VFX); rc::endPass(RPI::VFX);
    CHECK((rc::getFrameViolationCount() == 1));
    return;
}



}
