#include "tick_commit.h"
#include "../mclib/sim_overlap.h"
#include "../mclib/texture_assets.h"
#include "render_view.h"
#include "../GameOS/gameos/diagnostic_trace.h"
#include "mc2fmt.h"
#include <array>
#include <vector>
#include <cassert>

namespace mc2::tick {
namespace {
// One tick producer; main owns these only after the join. No mutex is needed.
std::array<std::vector<std::function<void()>>, size_t(Queue::Count)> queues;
// CPU packet leases are independent of GPU tickets. A release waits for every
// packet admitted before it, including when recording completes out of order.
std::array<uint64_t, 3> packets{};
uint64_t packetSequence = 0;
struct RetiredCommand {
    uint64_t sequence;
    std::function<void()> command;
};
std::vector<RetiredCommand> graveyard;
}
bool defer(Queue queue, std::function<void()> command) {
    if (!collecting()) return false;
    queues[size_t(queue)].push_back(std::move(command));
    return true;
}
bool deferRetirement(Queue queue, std::function<void()> command) {
    if (collecting()) {
        // Preserve tick command order. Only destruction gets the extra CPU
        // lifetime hold; paint/FX creation/RNG commands in Release still commit.
        return defer(queue, [queue, command = std::move(command)] {
            if (!deferRetirement(queue, command)) command();
        });
    }
    mc2::overlap::assertMainThread("defer render packet release");
    for (const auto sequence : packets) {
        if (!sequence) continue;
        graveyard.push_back({packetSequence, std::move(command)});
        return true;
    }
    return false;
}
uint64_t retainRenderPacket() {
    mc2::overlap::assertMainThread("retainRenderPacket");
    const uint64_t sequence = packetSequence + 1;
    auto& slot = packets[sequence % packets.size()];
    // Never recycle a CPU slot while a reader still holds it, even in release.
    if (!sequence || slot) std::abort();
    slot = sequence;
    packetSequence = sequence;
    return sequence;
}
void retireRenderPacket(uint64_t sequence) {
    mc2::overlap::assertMainThread("retireRenderPacket");
    // The graveyard commands destroy appearances (AppearanceTypeList refcounts) and the worker also pushes to the
    // graveyard, so this must run after the join.
    mc2::overlap::assertSimQuiescent("graveyard drain");
    auto& slot = packets[sequence % packets.size()];
    if (!sequence || slot != sequence) std::abort();
    slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (const auto active : packets)
        if (active && active < oldest) oldest = active;
    // Remove ready work before invoking destructors, which can enqueue releases.
    std::vector<std::function<void()>> ready;
    size_t count = 0;
    while (count < graveyard.size() && graveyard[count].sequence < oldest)
        ready.push_back(std::move(graveyard[count++].command));
    graveyard.erase(graveyard.begin(), graveyard.begin() + count);
    {
        static uint64_t s_calls = 0;
        if (mc2_diag::tagRequested("GRAVEYARD") && (++s_calls % 120) == 0)
            mc2::fmt::print_out("[GRAVEYARD v1] retire_calls={:d} left={:d} oldest={:d} seq={:d}\n", mc2::fmt::as_i32(static_cast<int>(s_calls)), mc2::fmt::as_i32(static_cast<int>(graveyard.size())), mc2::fmt::as_i32(static_cast<int>(oldest & 0x7fffffff)), mc2::fmt::as_i32(static_cast<int>(sequence)));
    }
    if (!ready.empty()) {
        // [GRAVEYARD v1]: how many drains destroyed real work (the R1 proof needs this to be >= 20 under the soak knob).
        static uint64_t s_drains = 0, s_commands = 0;
        ++s_drains;
        s_commands += ready.size();
        if (mc2_diag::tagRequested("GRAVEYARD") && (s_drains <= 2 || (s_drains % 10) == 0))
            mc2::fmt::print_out("[GRAVEYARD v1] drains={:d} commands={:d}\n", mc2::fmt::as_i32(static_cast<int>(s_drains)), mc2::fmt::as_i32(static_cast<int>(s_commands)));
    }
    for (auto& command : ready) command();
}
void commitTickQueues() {
    mc2::overlap::assertMainThread("commitTickQueues");
    assert(!mc2::overlap::jobInFlight() && !collecting());
    // G7b K3: the render list holds object pointers; drop it before the deferred frees (the next extract refills it).
    mc2::render_view::stagingList().clear();
    mc2::render_view::stagingHud().movers.clear(); // release retained waypoint appearances before mission storage/heap teardown
    mc2::textures::commitTickTextures();
    for (auto& queue : queues) {
        // Commands run outside the producer scope, including destructors.
        auto commands = std::move(queue);
        queue.clear();
        for (auto& command : commands) command();
    }
}
} // namespace mc2::tick
