#include "doctest.h"
#include "../../GameOS/gameos/deferred_submit_batch.h"

// submit-order-guard: the ordering rules of VulkanContext's deferred submit batch (reserved recordings, prefix flush, the present-worker rule).
using Batch = mc2vk::DeferredSubmitBatch<int>;   // handle 0 = null

namespace {
const auto t0 = Batch::Clock::time_point{};
auto runInline = [](int n, const std::function<void(int)>& body) { for (int i = n - 1; i >= 0; --i) body(i); };   // any order: reversed
}

TEST_CASE("submit batch: resolve fills reserved entries in place, done runs in reserved order") {
    Batch b;
    int next = 100;
    std::vector<int> doneOrder;
    b.addPlain(1, 1, t0);
    b.reserve(2, [](int cb) { return cb != 0; }, [&](bool ok) { doneOrder.push_back(ok ? 2 : -2); }, t0);
    b.addPlain(3, 3, t0);
    b.reserve(4, [](int) { return true; }, [&](bool ok) { doneOrder.push_back(ok ? 4 : -4); }, t0);
    REQUIRE(b.hasRecords());
    b.resolve([&] { return next++; }, runInline);
    CHECK_FALSE(b.hasRecords());
    CHECK(doneOrder == std::vector<int>{2, 4});
    const auto f = b.takePrefix(b.size());
    CHECK(f.count == 4);
    CHECK(f.value == 4);   // one submit signals the last value
    // acquire ran in reversed order (any order is legal); the entry order of the buffers is the reserved order, not the recording order
    CHECK(f.cbs == std::vector<int>{1, 101, 3, 100});
}

TEST_CASE("submit batch: a failed recording is skipped but its value is still signalled") {
    Batch b;
    bool doneOk = true;
    b.addPlain(1, 1, t0);
    b.reserve(2, [](int) { return false; }, [&](bool ok) { doneOk = ok; }, t0);
    b.resolve([] { return 7; }, runInline);
    CHECK_FALSE(doneOk);
    const auto f = b.takePrefix(2);
    CHECK(f.cbs == std::vector<int>{1});
    CHECK(f.value == 2);   // the failed island's value is the last of the batch: it must be signalled or every wait on it burns its timeout

    Batch noBuffer;   // the pool could not hand a command buffer out
    bool ok2 = true;
    noBuffer.reserve(5, [](int) { return true; }, [&](bool ok) { ok2 = ok; }, t0);
    noBuffer.resolve([] { return 0; }, runInline);
    CHECK_FALSE(ok2);
    const auto g = noBuffer.takePrefix(1);
    CHECK(g.cbs.empty());
    CHECK(g.value == 5);
}

TEST_CASE("submit batch: the present worker never queues past an unresolved reservation") {
    Batch b;
    b.addPlain(1, 1, t0);
    b.addPlain(2, 2, t0);
    b.reserve(3, [](int) { return true; }, nullptr, t0);   // sky island, not recorded yet
    b.addPlain(4, 4, t0);                                   // composite / post after it
    const auto plan = b.planFlush(true);
    CHECK(plan.violation);
    CHECK_FALSE(plan.resolveFirst);
    CHECK(plan.count == 2);
    const auto f = b.takePrefix(plan.count);
    CHECK(f.cbs == std::vector<int>{1, 2});
    CHECK(f.value == 2);   // never value 4: the timeline may not pass an unrecorded value
    CHECK(b.size() == 2);
    CHECK(b.firstRecordEntry() == 0);   // the reservation moved to the front with the prefix gone
}

TEST_CASE("submit batch: main resolves first, so the worker finds nothing left (the present enqueue contract)") {
    Batch b;
    b.addPlain(1, 1, t0);
    b.reserve(2, [](int) { return true; }, nullptr, t0);
    b.addPlain(3, 3, t0);
    const auto onMain = b.planFlush(false);
    CHECK(onMain.resolveFirst);
    CHECK_FALSE(onMain.violation);
    b.resolve([] { return 9; }, runInline);   // present_thread::enqueue -> resolveRecorded
    const auto onWorker = b.planFlush(true);
    CHECK_FALSE(onWorker.violation);
    CHECK_FALSE(onWorker.resolveFirst);
    CHECK(onWorker.count == 3);
    const auto f = b.takePrefix(onWorker.count);
    CHECK(f.cbs == std::vector<int>{1, 9, 3});
    CHECK(f.value == 3);
    CHECK(b.empty());
}

TEST_CASE("submit batch: partial prefix flushes keep later reservations addressed correctly") {
    Batch b;
    b.addPlain(1, 1, t0);
    b.reserve(2, [](int cb) { return cb == 50; }, nullptr, t0);
    b.addPlain(3, 3, t0);
    b.reserve(4, [](int cb) { return cb == 50; }, nullptr, t0);
    CHECK(b.takePrefix(b.firstRecordEntry()).value == 1);   // the 0.4 ms early flush: the prefix before the first reservation
    CHECK(b.firstRecordEntry() == 0);
    b.resolve([] { return 50; }, runInline);
    const auto f = b.takePrefix(b.size());
    CHECK(f.cbs == std::vector<int>{50, 3, 50});
    CHECK(f.value == 4);
    CHECK(b.takePrefix(1).count == 0);   // an empty batch queues nothing
}
