#include "tick_commit.h"
#include "../mclib/sim_overlap.h"
#include "../mclib/texture_assets.h"
#include "render_view.h"
#include <array>
#include <vector>
#include <cassert>

namespace mc2::tick {
namespace {
// One tick producer; main owns these only after the join. No mutex is needed.
std::array<std::vector<std::function<void()>>, size_t(Queue::Count)> queues;
}
bool defer(Queue queue, std::function<void()> command) {
    if (!collecting()) return false;
    queues[size_t(queue)].push_back(std::move(command));
    return true;
}
void commitTickQueues() {
    mc2::overlap::assertMainThread("commitTickQueues");
    assert(!mc2::overlap::jobInFlight() && !collecting());
    // G7b K3: the render list holds object pointers; drop it before the deferred frees (the next extract refills it).
    mc2::render_view::list().clear();
    mc2::render_view::hud().movers.clear(); // release retained waypoint appearances before mission storage/heap teardown
    mc2::textures::commitTickTextures();
    for (auto& queue : queues) {
        // Commands run outside the producer scope, including destructors.
        auto commands = std::move(queue);
        queue.clear();
        for (auto& command : commands) command();
    }
}
} // namespace mc2::tick
