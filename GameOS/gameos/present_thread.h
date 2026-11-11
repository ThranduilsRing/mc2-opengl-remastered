#pragma once

#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>

namespace mc2vk::present_thread {
namespace detail {
// The continuation owns only captured WSI handles; Render bookkeeping must finish before it can block.
template<class Finish, class Seal, class Tail>
bool afterRenderTail(Finish&& finish, Seal&& seal, Tail&& tail) {
    const bool ready = finish();
    seal();
    return ready && tail();
}
// Called with the mailbox mutex held. A failed worker drains queued Render packets without entering WSI again.
constexpr bool discardPresent(bool failed) { return failed; }
constexpr bool canEnqueue(bool pending, bool sealed) { return !pending && sealed; }
constexpr bool drained(bool pending, bool busy) { return !pending && !busy; }
inline bool takeDeferredFailure(bool& failed) {
    const bool result = failed;
    failed = false;
    return result;
}
// A Render completion or a replacement signaled Render fence says nothing about the previous blit using this slot.
template<class Wait, class Reset>
bool prepareBlitSlot(uint32_t slot, Wait&& wait, Reset&& reset) {
    return wait(slot) && reset(slot);
}
}
// Only the game opts in. Editors/viewers keep the same present function inline.
void start();
void stop();
bool enabled();
bool onWorker();
// The caller label (function name of the call site) is recorded for [PERF_COUNTERS] when the wait blocks.
void waitSealed(std::source_location site = std::source_location::current());
void waitIdle();
// The packet owns no game objects. Draw/HUD data belong to EnginePresentVk until sealed.
void enqueue(uint32_t width, uint32_t height, bool verify, uint32_t renderFrame,
             uint32_t fireFrame);
// The cap the worker holds between present starts (0 = none).
void pace(int fps);
// Called after submission, readback and frame retirement, before entering WSI.
void seal();
void forgetTexture(uint32_t handle);
// Worker only, including after seal: the present path is broken (swapchain lost, submit refused, slot fence timed out). Main takes it at the
// next frame top and runs the host's teardown and deferred-init retry, the same recovery the inline present path runs.
void reportFailure(std::string_view reason);
// Main only: true once per reported failure, after the worker went idle, so main may destroy the present path. Works after stop() too.
bool takeFailure(std::string& reason);
// Main thread only (a no-op elsewhere): dispatch window messages other threads sent to it, so a WSI call on the worker can return.
void pumpWindowMessages();
// Main only, once a wait passed its hang bound: exits through fatalBackendFailure(reason) unless a debugger is attached (then it returns).
void hungWait(std::string_view reason);
}
