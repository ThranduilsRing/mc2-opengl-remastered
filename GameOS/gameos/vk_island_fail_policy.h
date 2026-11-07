// vk_island_fail_policy.h -- what a Vulkan island does when its init fails.
//
// WHAT: islandInitAction() maps (island kind, did init pass its readiness gates, failing VkResult, build failures so far) to
// Wait / Retry / Degrade / Fatal; onIslandInitFailure() logs one [VK_ISLAND_INIT] line and, for Fatal, exits through the engine's
// one fatal path (mc2::fatalBackendFailure). The policy part is constexpr and Vulkan-free (tests/unit/test_vk_island_fail_policy.cpp).
// WHY: there is no GL renderer to fall back to. An island that declined silently dropped mechs, terrain, shadows, props or water
// from every frame with only a log line. Policy (GL-removal study, class c, lane 6):
//   - init not past its readiness gates (shared context / scene GBuffer not born yet) -> Wait: retry next frame, log once.
//   - a transient VkResult (out of date, not ready, timeout, pipeline compile required) on the first build failure -> Retry once.
//   - otherwise a required island (its absence makes the frame wrong) -> Fatal; an optional effect (fog, screen effects) -> Degrade:
//     the effect stays off for the process, logged once.
// "Nothing to draw" (no water on the map, no instances this frame) is not an init failure and never reaches this policy.
#ifndef MC2_VK_ISLAND_FAIL_POLICY_H
#define MC2_VK_ISLAND_FAIL_POLICY_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "render_backend_select.h"   // mc2::fatalBackendFailure

namespace mc2vk {

enum class IslandKind : uint8_t { Mech, Terrain, Shadow, ColorProp, Water, Fog, ScreenFx };
enum class IslandInitAction : uint8_t { Wait, Retry, Degrade, Fatal };

constexpr const char* islandKindName(IslandKind kind) {
    switch (kind) {
    case IslandKind::Mech: return "mech";
    case IslandKind::Terrain: return "terrain";
    case IslandKind::Shadow: return "shadow";
    case IslandKind::ColorProp: return "props";
    case IslandKind::Water: return "water";
    case IslandKind::Fog: return "fog";
    case IslandKind::ScreenFx: return "screenfx";
    }
    return "unknown";
}

constexpr const char* islandInitActionName(IslandInitAction action) {
    switch (action) {
    case IslandInitAction::Wait: return "wait";
    case IslandInitAction::Retry: return "retry";
    case IslandInitAction::Degrade: return "degrade";
    case IslandInitAction::Fatal: return "fatal";
    }
    return "unknown";
}

// Required islands draw part of every frame; fog and the screen-space effects are optional.
constexpr bool islandRequired(IslandKind kind) { return kind != IslandKind::Fog && kind != IslandKind::ScreenFx; }

// VkResult values as plain integers, so this header needs no Vulkan include.
inline constexpr int32_t kVkNotReady = 1, kVkTimeout = 2, kVkErrorOutOfHostMemory = -1, kVkErrorOutOfDeviceMemory = -2,
                         kVkErrorInitializationFailed = -3, kVkErrorDeviceLost = -4, kVkErrorUnknown = -13,
                         kVkErrorOutOfDateKhr = -1000001004, kVkPipelineCompileRequired = 1000297000;
// Not a Vulkan value: the residency budget (infra::Admission, vk_owned.cpp) refused the allocation. Transient: memory retired by a
// resize or mission load can free the budget by the next frame.
inline constexpr int32_t kVkAdmissionDenied = 1999000001;

constexpr bool vkResultTransient(int32_t result) {
    return result == kVkNotReady || result == kVkTimeout || result == kVkErrorOutOfDateKhr || result == kVkPipelineCompileRequired ||
           result == kVkAdmissionDenied;
}

constexpr const char* vkResultName(int32_t result) {
    switch (result) {
    case 0: return "none";   // the failure was not a Vulkan call (missing SPIR-V, residency admission, ...)
    case kVkNotReady: return "VK_NOT_READY";
    case kVkTimeout: return "VK_TIMEOUT";
    case kVkErrorOutOfHostMemory: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case kVkErrorOutOfDeviceMemory: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case kVkErrorInitializationFailed: return "VK_ERROR_INITIALIZATION_FAILED";
    case kVkErrorDeviceLost: return "VK_ERROR_DEVICE_LOST";
    case kVkErrorUnknown: return "VK_ERROR_UNKNOWN";
    case kVkErrorOutOfDateKhr: return "VK_ERROR_OUT_OF_DATE_KHR";
    case kVkPipelineCompileRequired: return "VK_PIPELINE_COMPILE_REQUIRED";
    case kVkAdmissionDenied: return "RESIDENCY_ADMISSION_DENIED";
    }
    return "VK_RESULT_OTHER";
}

// reachedBuild: init passed its readiness gates before it failed. buildFailures: build-phase failures this session, this one included.
constexpr IslandInitAction islandInitAction(IslandKind kind, bool reachedBuild, int32_t vkResult, int buildFailures) {
    if (!reachedBuild) return IslandInitAction::Wait;
    if (vkResultTransient(vkResult) && buildFailures <= 1) return IslandInitAction::Retry;
    return islandRequired(kind) ? IslandInitAction::Fatal : IslandInitAction::Degrade;
}

// One island's failure count. Reset after a successful init and at the mission boundary (resetAllIslandInitFailures), so a transient
// recovered once does not make the next mission's or resize's first transient fatal.
struct IslandInitFailures {
    int build = 0;
    bool waitLogged = false;
    void reset() { *this = IslandInitFailures{}; }
};

// The per-island counts (render thread only).
inline IslandInitFailures& islandInitFailures(IslandKind kind) {
    static IslandInitFailures failures[static_cast<int>(IslandKind::ScreenFx) + 1];
    return failures[static_cast<int>(kind)];
}
inline void resetAllIslandInitFailures() {
    for (int k = 0; k <= static_cast<int>(IslandKind::ScreenFx); ++k) islandInitFailures(static_cast<IslandKind>(k)).reset();
}

// Applies the policy to one init failure. Fatal does not return; Wait and Retry mean "try init again next frame"; Degrade means "leave the
// effect off for the process".
inline IslandInitAction onIslandInitFailure(IslandKind kind, bool reachedBuild, int32_t vkResult, std::string_view reason,
                                            IslandInitFailures& state) {
    if (reachedBuild) ++state.build;
    const IslandInitAction action = islandInitAction(kind, reachedBuild, vkResult, state.build);
    const std::string why(reason.empty() ? std::string_view("unspecified") : reason);
    if (action == IslandInitAction::Wait && state.waitLogged) return action;
    if (action == IslandInitAction::Wait) state.waitLogged = true;
    std::fprintf(stderr, "[VK_ISLAND_INIT] island=%s action=%s vkresult=%s(%d) reason=%s\n", islandKindName(kind),
                 islandInitActionName(action), vkResultName(vkResult), static_cast<int>(vkResult), why.c_str());
    std::fflush(stderr);
    if (action == IslandInitAction::Fatal) {
        const std::string message = std::string("the ") + islandKindName(kind) + " renderer failed to initialise (" + vkResultName(vkResult) +
                                    ", " + why + ")";
        mc2::fatalBackendFailure(message.c_str());
    }
    return action;
}

// The same against the island's own count.
inline IslandInitAction onIslandInitFailure(IslandKind kind, bool reachedBuild, int32_t vkResult, std::string_view reason) {
    return onIslandInitFailure(kind, reachedBuild, vkResult, reason, islandInitFailures(kind));
}

}  // namespace mc2vk

#endif  // MC2_VK_ISLAND_FAIL_POLICY_H
