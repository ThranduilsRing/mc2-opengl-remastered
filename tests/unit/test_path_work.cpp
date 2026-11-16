#include "doctest.h"
#include "../../mclib/path_work.h"
#include "../../mclib/open_heap.h"
#include <thread>

namespace {
struct Request {
    long enqueueFrame = 0;
    Request* prev = nullptr;
    Request* next = nullptr;
    bool player = false;
};
auto isPlayer = [](Request* rec) { return rec->player; };
}

TEST_CASE("PathWork/long solved chains retain their first 200 steps and drop tail doors") {
    for (long length : {199, 200, 201, 1200}) for (long door : {-1, 2}) {
        const auto prefix = mc2::path_work::boundedPrefix(length, 200, length - (door != -1), 7, door,
            [](long row, long col) { return std::pair{row - 1, col}; });
        CHECK(prefix.steps == std::min(length, 200L));
        CHECK(prefix.row == prefix.steps - (prefix.door != -1));
        CHECK(prefix.col == 7);
        CHECK(prefix.truncated == (length > 200));
        CHECK(prefix.door == (length > 200 ? -1 : door));
        for (long index = prefix.steps; index > 0; --index) CHECK(index - 1 < 200);
    }
}

TEST_CASE("PathWork/empty zero cost exact threshold overshoot and next tick") {
    using namespace mc2::path_work;
    Request a, b;
    a.next = &b;
    b.prev = &a;
    CHECK(nextRequest<Request>(nullptr, 0, 0, isPlayer) == nullptr);
    CHECK(nextRequest(&a, 0, 0, isPlayer) == &a);
    CHECK(nextRequest(&b, 0, 0, isPlayer) == &b); // zero-cost request did not consume work
    CHECK(nextRequest(&b, kTickBudget - 1, 0, isPlayer) == &b);
    CHECK(nextRequest(&b, kTickBudget, 0, isPlayer) == nullptr);
    CHECK(nextRequest(&b, kTickBudget + 20000, 0, isPlayer) == nullptr);
    CHECK(nextRequest(&b, 0, 1, isPlayer) == &b); // next update uses a fresh delta
}

TEST_CASE("PathWork/player third opportunity bypasses budget with stable FIFO carry") {
    using namespace mc2::path_work;
    Request a, b, c, d;
    a.next = &b; b.prev = &a; b.next = &c; c.prev = &b; c.next = &d; d.prev = &c;
    b.player = d.player = true;
    auto* front = &a;
    auto* end = &d;
    CHECK(nextRequest(front, kTickBudget, 0, isPlayer) == nullptr);
    CHECK(nextRequest(front, kTickBudget, 1, isPlayer) == nullptr);
    auto* due = nextRequest(front, kTickBudget, 2, isPlayer);
    REQUIRE(due == &b);
    promote(front, end, due);
    CHECK(front == &b);
    CHECK(front->prev == nullptr);
    CHECK(a.next == &c);
    CHECK(c.prev == &a);
    CHECK(end == &d);
    // Pop b as the production manager does, then service the next due player.
    front = b.next; front->prev = nullptr;
    REQUIRE(nextRequest(front, kTickBudget + 100, 2, isPlayer) == &d);
    promote(front, end, &d);
    CHECK(front == &d);
    CHECK(end == &c);
    CHECK(c.next == nullptr);
    front = d.next; front->prev = nullptr;
    CHECK(front == &a);
    CHECK(front->next == &c);
    CHECK(nextRequest(front, 0, 3, isPlayer) == &a);
    promote(front, end, front);
    CHECK(front == &a);
}

TEST_CASE("PathWork/twelve player group obeys six solve cap across deadline cliff") {
    using namespace mc2::path_work;
    Request group[12];
    for (int i = 0; i < 12; ++i) {
        group[i].player = true;
        group[i].next = i < 11 ? &group[i + 1] : nullptr;
    }
    auto* front = group;
    for (long tick = 0; tick < 4; ++tick) {
        long served = 0;
        while (nextRequest(front, served * kTickBudget, tick, isPlayer, served)) {
            front = front->next;
            ++served;
        }
        CHECK(served == (tick < 2 ? 1 : tick == 2 ? 6 : 4));
        CHECK(nextRequest(group, 0, tick, isPlayer, 6) == nullptr);
    }
    CHECK(front == nullptr);
}

TEST_CASE("PathWork/counts real heap pops without diagnostic gates and isolates worker") {
    using namespace mc2::path_work;
    const auto before = expansions;
    mc2::OpenHeap heap(3);
    heap.insert({20, 0, 0, 0});
    heap.insert({10, 1, 0, 1});
    REQUIRE(popNode(heap).has_value());
    CHECK(expansions - before == 1);
    REQUIRE(popNode(heap).has_value());
    CHECK(expansions - before == 2);
    CHECK_FALSE(popNode(heap).has_value());
    CHECK(expansions - before == 2);
    std::uint64_t workerCount = 0;
    std::thread worker([&] {
        mc2::OpenHeap local(1);
        local.insert({1, 0, 0, 0});
        popNode(local);
        workerCount = expansions;
    });
    worker.join();
    CHECK(workerCount == 1);
    CHECK(expansions - before == 2);
}
