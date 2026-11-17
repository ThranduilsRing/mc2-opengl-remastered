// Trigger paths: GameOS/gameos/load_scope.h
#include "doctest.h"
#include "../../GameOS/gameos/load_scope.h"
#include <numeric>
using namespace mc2::load;

TEST_CASE("load_scope/nested partition and lookup") {
    auto* outer = entry("sim.test"); auto* child = entry("io.test"); auto* unknown = entry("untagged");
    begin(100);
    Scope a(outer, 100);
    { Scope b(child, 120); b.end(150); }
    { Scope b(unknown, 150); b.end(160); }
    a.end(200);
    const auto r = report(200);
    CHECK(outer->ns == 100);
    CHECK(outer->exclusive[0] == 60);
    CHECK(child->exclusive[0] == 30);
    CHECK(r.kinds[0][static_cast<int>(Kind::other)] == 10);
    CHECK(std::accumulate(r.kinds[0], r.kinds[0] + 8, int64_t{0}) == r.total);
    CHECK(kind("decode.prefix") == Kind::decode);
    CHECK(kind("TextureSource.CreateCpuTexture") == Kind::decode);
    CHECK(kind("content.loadCached") == Kind::other);
    CHECK(kind("unknown") == Kind::other);
}
TEST_CASE("load_scope/worker exclusive and separate top") {
    auto* outer = entry("cook.worker"); auto* inner = entry("io.worker");
    begin(100);
    std::thread worker([&] {
        Scope a(outer, 110); { Scope b(inner, 120); b.end(140); } a.end(170);
    });
    worker.join(); const auto r = report(200);
    CHECK(outer->exclusive[1] == 40);
    CHECK(inner->exclusive[1] == 20);
    CHECK(r.kinds[0][static_cast<int>(Kind::cook)] == 0);
    CHECK(r.kinds[0][static_cast<int>(Kind::other)] == 100);
    CHECK(r.kinds[1][static_cast<int>(Kind::cook)] == 40);
    CHECK(r.top.find("worker_cook.worker=") != std::string::npos);
}
TEST_CASE("load_scope/reset and crossing boundary") {
    auto* e = entry("io.cross"); begin(100); Scope old(e, 110);
    CHECK(report(200).split.find("open_scopes=1") != std::string::npos);
    begin(300);
    { Scope fresh(e, 310); fresh.end(330); }
    old.end(340);
    const auto r = report(400);
    CHECK(e->calls == 1); CHECK(e->exclusive[0] == 20);
    CHECK(r.kinds[0][static_cast<int>(Kind::io)] == 20);
    CHECK(r.kinds[0][static_cast<int>(Kind::other)] == 80);
    begin(500); CHECK(report(600).scopes.empty());
}
TEST_CASE("load_scope/capacity exhaustion reported") {
    // Restore the registry so this adversarial test cannot affect other cases.
    const int saved = g_entryCount; const auto drops = g_capacityDrops;
    g_entryCount = kMaxEntries; CHECK(entry("capacity.unregistered") == nullptr);
    begin(100); CHECK(report(200).split.find("capacity_drops=1") != std::string::npos);
    g_entryCount = saved; g_capacityDrops = drops;
}
TEST_CASE("load_scope/inactive overhead") {
    auto* e = entry("io.inactive"); begin(); report();
    const auto start = clockNs();
    for (int i = 0; i < 10000; ++i) { Scope s(e); }
    const double ms = (clockNs() - start) / 1e6;
    std::printf("load_scope inactive_10000_ms=%.6f\n", ms);
    CHECK(ms < 1.0);
}
