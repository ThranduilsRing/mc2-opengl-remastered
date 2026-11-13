// Deterministic queued-path admission. Searches finish; one request (and player
// deadlines) may overshoot the soft budget, within a hard solve cap per tick.
// No wall clock or diagnostic gate.
#pragma once

#include <cstdint>

namespace mc2::path_work {

inline constexpr std::uint64_t kTickBudget = 4096;
inline constexpr long kPlayerDeadline = 2; // third service opportunity, counting age zero
inline constexpr long kMaxSolvesPerTick = 6; // legacy cap, including due players

// The simulation runs an update and its synchronous solves on the same thread,
// whether serial or on the sim worker. Other threads never contribute its work.
inline thread_local std::uint64_t expansions = 0;

template<class Heap>
auto popNode(Heap& heap) {
    auto node = heap.popMin();
    if (node)
        ++expansions;
    return node;
}

template<class Request, class IsPlayer>
Request* nextRequest(Request* front, std::uint64_t work, long frame, IsPlayer isPlayer,
                     long served = 0) {
    // A shared deadline must not turn a group order into an unbounded burst.
    // Count all admissions, including the solve that first exhausts the budget.
    if (served >= kMaxSolvesPerTick)
        return nullptr;
    if (work < kTickBudget)
        return front;
    // Preserve FIFO among due player requests. Background backlog remains in
    // place; a newly queued/replaced request cannot reset another one's age.
    for (auto* rec = front; rec; rec = rec->next)
        if (frame - rec->enqueueFrame >= kPlayerDeadline && isPlayer(rec))
            return rec;
    return nullptr;
}

template<class Request>
void promote(Request*& front, Request*& end, Request* rec) {
    if (rec == front)
        return;
    rec->prev->next = rec->next;
    if (rec->next)
        rec->next->prev = rec->prev;
    else
        end = rec->prev;
    rec->prev = nullptr;
    rec->next = front;
    front->prev = rec;
    front = rec;
}

} // namespace mc2::path_work
