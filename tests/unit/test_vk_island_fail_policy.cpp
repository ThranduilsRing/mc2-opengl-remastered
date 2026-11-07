// The VK island init-failure policy: (island kind, readiness reached, VkResult, build failures) -> wait / retry / degrade / fatal.
#include "doctest.h"

#include "vk_island_fail_policy.h"

using mc2vk::IslandInitAction;
using mc2vk::IslandKind;
using mc2vk::islandInitAction;

namespace {
constexpr IslandKind kRequired[] = {IslandKind::Mech, IslandKind::Terrain, IslandKind::Shadow, IslandKind::ColorProp, IslandKind::Water};
constexpr IslandKind kOptional[] = {IslandKind::Fog, IslandKind::ScreenFx};
}  // namespace

TEST_CASE("island init: a failure before the readiness gates waits, whatever the result") {
    for (IslandKind k : kRequired) {
        CHECK(islandInitAction(k, false, 0, 0) == IslandInitAction::Wait);
        CHECK(islandInitAction(k, false, mc2vk::kVkErrorDeviceLost, 0) == IslandInitAction::Wait);
    }
    CHECK(islandInitAction(IslandKind::Fog, false, 0, 0) == IslandInitAction::Wait);
}

TEST_CASE("island init: a transient VkResult retries once, then follows the island's kind") {
    for (int32_t r : {mc2vk::kVkNotReady, mc2vk::kVkTimeout, mc2vk::kVkErrorOutOfDateKhr, mc2vk::kVkPipelineCompileRequired}) {
        CHECK(mc2vk::vkResultTransient(r));
        for (IslandKind k : kRequired) {
            CHECK(islandInitAction(k, true, r, 1) == IslandInitAction::Retry);
            CHECK(islandInitAction(k, true, r, 2) == IslandInitAction::Fatal);
        }
        for (IslandKind k : kOptional) {
            CHECK(islandInitAction(k, true, r, 1) == IslandInitAction::Retry);
            CHECK(islandInitAction(k, true, r, 2) == IslandInitAction::Degrade);
        }
    }
}

TEST_CASE("island init: a hard build failure is fatal for required islands, degrades optional ones") {
    for (int32_t r : {0, mc2vk::kVkErrorOutOfDeviceMemory, mc2vk::kVkErrorInitializationFailed, mc2vk::kVkErrorDeviceLost}) {
        CHECK_FALSE(mc2vk::vkResultTransient(r));
        for (IslandKind k : kRequired) CHECK(islandInitAction(k, true, r, 1) == IslandInitAction::Fatal);
        for (IslandKind k : kOptional) CHECK(islandInitAction(k, true, r, 1) == IslandInitAction::Degrade);
    }
}

TEST_CASE("island init: names for the log line") {
    CHECK(std::string(mc2vk::islandKindName(IslandKind::ColorProp)) == "props");
    CHECK(std::string(mc2vk::islandInitActionName(IslandInitAction::Degrade)) == "degrade");
    CHECK(std::string(mc2vk::vkResultName(mc2vk::kVkErrorOutOfDateKhr)) == "VK_ERROR_OUT_OF_DATE_KHR");
    CHECK(std::string(mc2vk::vkResultName(0)) == "none");
    CHECK(std::string(mc2vk::vkResultName(-9999)) == "VK_RESULT_OTHER");
}

TEST_CASE("island init: onIslandInitFailure counts build failures and logs a wait once") {
    mc2vk::IslandInitFailures state;
    CHECK(mc2vk::onIslandInitFailure(IslandKind::Fog, false, 0, "not_ready", state) == IslandInitAction::Wait);
    CHECK(state.waitLogged);
    CHECK(state.build == 0);
    CHECK(mc2vk::onIslandInitFailure(IslandKind::Fog, true, mc2vk::kVkTimeout, "pipeline", state) == IslandInitAction::Retry);
    CHECK(mc2vk::onIslandInitFailure(IslandKind::Fog, true, mc2vk::kVkTimeout, "pipeline", state) == IslandInitAction::Degrade);
    CHECK(state.build == 2);
}
