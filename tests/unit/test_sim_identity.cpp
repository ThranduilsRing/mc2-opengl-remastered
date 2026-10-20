// Offline tests for the pure world-hash kernel.
//
// The kernel has no engine deps by design, so every property the refuse-to-grade
// gate leans on is provable here with no GPU, no game, and no capture.
//
// The properties under test are exactly the ones whose failure would make the
// gate a lying instrument:
//   * order-independence   -- else a harmless iteration-order change refuses
//   * count sensitivity    -- else a vanished mover hashes clean
//   * visual/non-visual    -- else the t=34.1s target-only case refuses (wallpaper)
//                             or the t=34.4s position case passes (false green)
//   * quantum sensitivity  -- else sub-quantum float noise refuses
//   * NaN totality         -- else a NaN run hashes unstably

#include "doctest.h"
#include "../../mclib/sim_identity.h"

#include <vector>
#include <string>
#include <cmath>
#include <cstring>

using namespace mc2_sim_identity;

namespace {

MoverRecord mk(int32_t id, float x, float y, float z) {
    MoverRecord r;
    r.id = id; r.team = 1;
    r.px = x; r.py = y; r.pz = z;
    r.rot = 90.0f; r.hp = 1.0f;
    r.gesture = 2; r.animFrame = 7; r.status = 0;
    r.target = -1; r.brainState = 3; r.moveState = 1;
    r.pathStep = 0; r.pathSteps = 4;
    return r;
}

uint64_t world(std::vector<MoverRecord> v, DigestKind k) {
    return digestWorld(v, k);
}

} // namespace

TEST_CASE("sim_identity: digest is independent of mover iteration order") {
    // The load-bearing invariant. Mover-list order is not guaranteed stable
    // across maps; the gate must not depend on it.
    std::vector<MoverRecord> a{mk(711, 10.0f, 20.0f, 30.0f),
                               mk(102, -5.0f, 0.0f, 2.5f),
                               mk(940, 100.0f, 3.0f, -7.0f)};
    std::vector<MoverRecord> b{a[2], a[0], a[1]};
    std::vector<MoverRecord> c{a[1], a[2], a[0]};

    CHECK(world(a, DIGEST_GRADED) == world(b, DIGEST_GRADED));
    CHECK(world(a, DIGEST_GRADED) == world(c, DIGEST_GRADED));
    CHECK(world(a, DIGEST_STRICT) == world(b, DIGEST_STRICT));
    CHECK(world(a, DIGEST_VISUAL) == world(c, DIGEST_VISUAL));
}

TEST_CASE("sim_identity: duplicate ids still order totally") {
    // Two records sharing an id must not leave the sort free to permute them.
    MoverRecord p = mk(500, 1.0f, 2.0f, 3.0f);
    MoverRecord q = mk(500, 9.0f, 8.0f, 7.0f);
    CHECK(world({p, q}, DIGEST_GRADED) == world({q, p}, DIGEST_GRADED));
    CHECK(world({p, q}, DIGEST_STRICT) == world({q, p}, DIGEST_STRICT));
}

TEST_CASE("sim_identity: a vanished mover changes the digest") {
    std::vector<MoverRecord> full{mk(1, 0.0f, 0.0f, 0.0f),
                                  mk(2, 1.0f, 1.0f, 1.0f)};
    std::vector<MoverRecord> gone{full[0]};
    CHECK(world(full, DIGEST_GRADED) != world(gone, DIGEST_GRADED));
    CHECK(world(full, DIGEST_VISUAL) != world(gone, DIGEST_VISUAL));
}

TEST_CASE("sim_identity: target-only divergence is invisible to the VISUAL digest") {
    // THE mc2_10 t=34.1s case:
    //   r1: id=711 target=-1     r2/r3: id=711 target=84146
    // with every position identical. The graded digest must SEE it (the sim
    // really did diverge) while the visual digest must NOT (no pixel can move
    // this frame). That gap is what lets the gate grade here instead of
    // refusing -- and refusing here is how UNGRADED becomes wallpaper.
    MoverRecord r1 = mk(711, -2112.0f, 2922.7f, 252.4f);
    MoverRecord r2 = r1;
    r2.target = 84146;

    CHECK(world({r1}, DIGEST_GRADED) != world({r2}, DIGEST_GRADED));
    CHECK(world({r1}, DIGEST_VISUAL) == world({r2}, DIGEST_VISUAL));
}

TEST_CASE("sim_identity: every non-visual field is invisible to the VISUAL digest") {
    MoverRecord base = mk(7, 1.0f, 2.0f, 3.0f);
    const uint64_t v0 = world({base}, DIGEST_VISUAL);

    MoverRecord t = base; t.target     = 999;   CHECK(world({t}, DIGEST_VISUAL) == v0);
    MoverRecord b = base; b.brainState = 42;    CHECK(world({b}, DIGEST_VISUAL) == v0);
    MoverRecord m = base; m.moveState  = 5;     CHECK(world({m}, DIGEST_VISUAL) == v0);
    MoverRecord s = base; s.pathStep   = 11;    CHECK(world({s}, DIGEST_VISUAL) == v0);
    MoverRecord n = base; n.pathSteps  = 88;    CHECK(world({n}, DIGEST_VISUAL) == v0);

    // ...and all of them ARE visible to the graded digest.
    CHECK(world({t}, DIGEST_GRADED) != world({base}, DIGEST_GRADED));
    CHECK(world({b}, DIGEST_GRADED) != world({base}, DIGEST_GRADED));
    CHECK(world({m}, DIGEST_GRADED) != world({base}, DIGEST_GRADED));
}

TEST_CASE("sim_identity: every visual field IS visible to the VISUAL digest") {
    MoverRecord base = mk(7, 1.0f, 2.0f, 3.0f);
    const uint64_t v0 = world({base}, DIGEST_VISUAL);

    MoverRecord x = base; x.px        += 10.0f; CHECK(world({x}, DIGEST_VISUAL) != v0);
    MoverRecord y = base; y.py        += 10.0f; CHECK(world({y}, DIGEST_VISUAL) != v0);
    MoverRecord z = base; z.pz        += 10.0f; CHECK(world({z}, DIGEST_VISUAL) != v0);
    MoverRecord r = base; r.rot       += 10.0f; CHECK(world({r}, DIGEST_VISUAL) != v0);
    MoverRecord h = base; h.hp         = 0.5f;  CHECK(world({h}, DIGEST_VISUAL) != v0);
    MoverRecord g = base; g.gesture    = 9;     CHECK(world({g}, DIGEST_VISUAL) != v0);
    MoverRecord a = base; a.animFrame  = 99;    CHECK(world({a}, DIGEST_VISUAL) != v0);
    MoverRecord u = base; u.status     = 4;     CHECK(world({u}, DIGEST_VISUAL) != v0);
    MoverRecord e = base; e.team       = 2;     CHECK(world({e}, DIGEST_VISUAL) != v0);
}

TEST_CASE("sim_identity: the t=34.4s position case DOES refuse") {
    // 0.3s after the target-only seed, positions diverged. The visual digest
    // must catch that -- this is the case where a pixel comparison is genuinely
    // meaningless and the gate has to refuse.
    MoverRecord r1 = mk(711, -2112.0f, 2922.7f, 252.4f);
    MoverRecord r2 = mk(711, -2112.0f, 2925.9f, 252.4f);  // 3.2 units apart
    CHECK(world({r1}, DIGEST_VISUAL) != world({r2}, DIGEST_VISUAL));
}

TEST_CASE("sim_identity: sub-quantum float noise does not move the graded digest") {
    // A 1-ULP-ish wobble far below kPosQuantum must NOT refuse; grading on raw
    // bits is how a gate refuses on noise that cannot move a pixel.
    MoverRecord a = mk(1, 100.0f, 200.0f, 300.0f);
    MoverRecord b = a;
    b.px = 100.0f + kPosQuantum * 0.01f;   // 1% of a quantum

    CHECK(world({a}, DIGEST_GRADED) == world({b}, DIGEST_GRADED));
    CHECK(world({a}, DIGEST_VISUAL) == world({b}, DIGEST_VISUAL));
    // ...and the strict digest still SEES it, so the difference stays reportable.
    CHECK(world({a}, DIGEST_STRICT) != world({b}, DIGEST_STRICT));
}

TEST_CASE("sim_identity: a super-quantum move does change the graded digest") {
    MoverRecord a = mk(1, 100.0f, 200.0f, 300.0f);
    MoverRecord b = a;
    b.px = 100.0f + kPosQuantum * 4.0f;
    CHECK(world({a}, DIGEST_GRADED) != world({b}, DIGEST_GRADED));
    CHECK(world({a}, DIGEST_VISUAL) != world({b}, DIGEST_VISUAL));
}

TEST_CASE("sim_identity: quantize is total, symmetric and NaN-safe") {
    CHECK(quantize(std::nanf(""), kPosQuantum) == kQuantNaN);
    CHECK(quantize(INFINITY,  kPosQuantum) == kQuantPosInf);
    CHECK(quantize(-INFINITY, kPosQuantum) == kQuantNegInf);
    // symmetric about zero
    CHECK(quantize(1.5f, 1.0f) == -quantize(-1.5f, 1.0f));
    CHECK(quantize(0.0f, kPosQuantum) == 0);
    // -0.0f and +0.0f must not be distinguishable: no renderer can show a sign
    // of zero, so treating them as divergence would be pure false refusal.
    MoverRecord p = mk(1, 0.0f, 0.0f, 0.0f);
    MoverRecord n = mk(1, -0.0f, -0.0f, -0.0f);
    CHECK(world({p}, DIGEST_STRICT) == world({n}, DIGEST_STRICT));
    CHECK(world({p}, DIGEST_GRADED) == world({n}, DIGEST_GRADED));
}

TEST_CASE("sim_identity: a NaN position is stable and distinguishable") {
    // Two runs that both went NaN must agree (stable), and a NaN run must not
    // collide with a finite one.
    MoverRecord bad1 = mk(1, std::nanf(""), 0.0f, 0.0f);
    MoverRecord bad2 = mk(1, std::nanf(""), 0.0f, 0.0f);
    MoverRecord good = mk(1, 0.0f, 0.0f, 0.0f);
    CHECK(world({bad1}, DIGEST_GRADED) == world({bad2}, DIGEST_GRADED));
    CHECK(world({bad1}, DIGEST_GRADED) != world({good}, DIGEST_GRADED));
}

TEST_CASE("sim_identity: digest kinds do not collide with each other") {
    std::vector<MoverRecord> v{mk(1, 1.0f, 2.0f, 3.0f)};
    const uint64_t g = world(v, DIGEST_GRADED);
    const uint64_t s = world(v, DIGEST_VISUAL);
    const uint64_t t = world(v, DIGEST_STRICT);
    CHECK(g != s);
    CHECK(g != t);
    CHECK(s != t);
}

TEST_CASE("sim_identity: empty world hashes to a defined, count-distinct value") {
    std::vector<MoverRecord> none;
    std::vector<MoverRecord> one{mk(1, 0.0f, 0.0f, 0.0f)};
    CHECK(digestWorld(none, DIGEST_GRADED) !=
          world(one, DIGEST_GRADED));
}

TEST_CASE("sim_identity: toHex16 is fixed-width lowercase hex") {
    char h[17];
    toHex16(0ULL, h);
    CHECK(std::string(h) == "0000000000000000");
    toHex16(0xDEADBEEFCAFEF00DULL, h);
    CHECK(std::string(h) == "deadbeefcafef00d");
    CHECK(std::strlen(h) == 16);
}

// Schema 2: buildings/turrets and ABL instances carry their own digests on the
// same line, so a divergence names the population that moved first.
TEST_CASE("sim_identity: object digest is order-independent and count/field sensitive") {
    using mc2_sim_identity::ObjectRecord;
    using mc2_sim_identity::digestObjects;
    auto obj = [](int id, int kind, float dmg, int team) {
        ObjectRecord r; r.id = id; r.kind = kind; r.damage = dmg; r.team = team; return r;
    };
    std::vector<ObjectRecord> a{obj(1, 1, 0.0f, 0), obj(2, 2, 5.0f, 1), obj(3, 1, 1.0f, 2)};
    std::vector<ObjectRecord> b{a[2], a[0], a[1]};
    CHECK(digestObjects(a) == digestObjects(b));
    std::vector<ObjectRecord> fewer{a[0], a[1]};
    CHECK(digestObjects(a) != digestObjects(fewer));
    std::vector<ObjectRecord> captured = a;
    captured[0].commander = 1;
    CHECK(digestObjects(a) != digestObjects(captured));
    std::vector<ObjectRecord> aimed = a;
    aimed[1].target = 77;
    CHECK(digestObjects(a) != digestObjects(aimed));
    std::vector<ObjectRecord> noise = a;
    noise[2].damage += 1.0f / 4096.0f;   // under the quantum: graded alike
    CHECK(digestObjects(a) == digestObjects(noise));
    std::vector<ObjectRecord> none;
    CHECK(digestObjects(none) != digestObjects(fewer));
}

TEST_CASE("sim_identity: ABL digest keeps registry order and sees state, init and statics") {
    using mc2_sim_identity::AblRecord;
    using mc2_sim_identity::digestAbl;
    using mc2_sim_identity::fnvBytes;
    using mc2_sim_identity::kFnvOffset;
    AblRecord x; x.id = 0; x.handle = 3; x.state = 1; x.statics = fnvBytes(kFnvOffset, "abc", 3);
    AblRecord y; y.id = 1; y.handle = 3; y.state = 2; y.statics = fnvBytes(kFnvOffset, "abd", 3);
    std::vector<AblRecord> a{x, y};
    std::vector<AblRecord> swapped{y, x};
    CHECK(digestAbl(a) != digestAbl(swapped));
    std::vector<AblRecord> moved = a;
    moved[0].state = 4;
    CHECK(digestAbl(a) != digestAbl(moved));
    std::vector<AblRecord> inited = a;
    inited[1].initCalled = 1;
    CHECK(digestAbl(a) != digestAbl(inited));
    std::vector<AblRecord> other = a;
    other[1].statics = x.statics;
    CHECK(digestAbl(a) != digestAbl(other));
    CHECK(fnvBytes(kFnvOffset, "abc", 3) == fnvBytes(kFnvOffset, "abc", 3));
    CHECK(fnvBytes(kFnvOffset, "abc", 3) != fnvBytes(kFnvOffset, "abc", 2));
}

// LG17 span guards: the span signatures replaced (const T* recs, size_t n).
// Each reference below is that old loop written out literally, so a change to
// the span kernel that would have moved game digests fails here.
namespace {

uint64_t oldDigestWorld(const MoverRecord* recs, size_t n, DigestKind kind) {
    std::vector<MoverRecord> sorted;
    if (n) sorted.assign(recs, recs + n);
    sortRecords(sorted);   // same in-place total sort, same insertion order
    uint64_t h = foldHeader(kFnvOffset, n);
    h = fnvI32(h, static_cast<int32_t>(kind));
    for (size_t i = 0; i < n; ++i) {
        uint64_t m = (kind == DIGEST_STRICT) ? digestMoverStrict(sorted[i])
                   : (kind == DIGEST_VISUAL) ? digestMoverVisual(sorted[i])
                                             : digestMoverGraded(sorted[i]);
        for (int b = 0; b < 8; ++b)
            h = fnvByte(h, static_cast<uint8_t>(((m >> (b * 8)) & 0xFFu)));
    }
    return h;
}

uint64_t oldDigestObjects(const ObjectRecord* recs, size_t n) {
    std::vector<ObjectRecord> sorted;
    if (n) sorted.assign(recs, recs + n);
    sortObjects(sorted);
    uint64_t h = foldHeader(kFnvOffset, n);
    for (size_t i = 0; i < n; ++i) {
        const ObjectRecord& r = sorted[i];
        h = fnvI32(h, r.id);
        h = fnvI32(h, r.kind);
        h = fnvI32(h, r.team);
        h = fnvI32(h, r.commander);
        h = fnvI32(h, r.status);
        h = fnvI32(h, quantize(r.damage, kPosQuantum));
        h = fnvI32(h, quantize(r.rot, kAngQuantum));
        h = fnvI32(h, r.target);
    }
    return h;
}

uint64_t oldDigestAbl(const AblRecord* recs, size_t n) {
    uint64_t h = foldHeader(kFnvOffset, n);
    for (size_t i = 0; i < n; ++i) {
        const AblRecord& r = recs[i];
        h = fnvI32(h, r.id);
        h = fnvI32(h, r.handle);
        h = fnvI32(h, r.state);
        h = fnvI32(h, r.prevState);
        h = fnvI32(h, r.initCalled);
        for (int b = 0; b < 8; ++b)
            h = fnvByte(h, static_cast<uint8_t>((r.statics >> (b * 8)) & 0xFFu));
    }
    return h;
}

} // namespace

TEST_CASE("sim_identity: span digests match the old pointer+count loops") {
    std::vector<MoverRecord> movers{mk(940, 100.0f, 3.0f, -7.0f),
                                    mk(102, -5.0f, 0.0f, 2.5f),
                                    mk(711, 10.0f, 20.0f, 30.0f)};
    for (const DigestKind k : {DIGEST_GRADED, DIGEST_VISUAL, DIGEST_STRICT}) {
        std::vector<MoverRecord> bySpan = movers;
        CHECK(digestWorld(bySpan, k) == oldDigestWorld(movers.data(), movers.size(), k));
    }

    std::vector<ObjectRecord> objects(2);
    objects[0].id = 3; objects[0].kind = 2; objects[0].team = 1; objects[0].damage = 12.5f;
    objects[0].rot = 45.0f; objects[0].target = 8;
    objects[1].id = 1; objects[1].kind = 1; objects[1].team = 0; objects[1].commander = 2;
    objects[1].damage = 0.25f;
    std::vector<ObjectRecord> objBySpan = objects;
    CHECK(digestObjects(objBySpan) == oldDigestObjects(objects.data(), objects.size()));

    std::vector<AblRecord> abl(2);
    abl[0].id = 0; abl[0].handle = 3; abl[0].state = 1; abl[0].statics = 0x0123456789ABCDEFULL;
    abl[1].id = 1; abl[1].handle = 4; abl[1].state = 7; abl[1].prevState = 1; abl[1].initCalled = 1;
    abl[1].statics = 0xFEDCBA9876543210ULL;
    const std::vector<AblRecord>& constAbl = abl;   // the span is span<const AblRecord>
    CHECK(digestAbl(constAbl) == oldDigestAbl(abl.data(), abl.size()));
}

TEST_CASE("sim_identity: empty and one-element span digests are defined and distinct") {
    std::vector<MoverRecord> none;
    std::vector<MoverRecord> one{mk(500, 1.0f, 2.0f, 3.0f)};
    CHECK(digestWorld(none, DIGEST_GRADED) == oldDigestWorld(nullptr, 0, DIGEST_GRADED));
    CHECK(digestWorld(none, DIGEST_VISUAL) == oldDigestWorld(nullptr, 0, DIGEST_VISUAL));
    CHECK(digestWorld(none, DIGEST_STRICT) == oldDigestWorld(nullptr, 0, DIGEST_STRICT));
    CHECK(digestWorld(one, DIGEST_GRADED) == oldDigestWorld(one.data(), 1, DIGEST_GRADED));
    CHECK(digestWorld(none, DIGEST_GRADED) != digestWorld(one, DIGEST_GRADED));

    std::vector<ObjectRecord> noObjects;
    CHECK(digestObjects(noObjects) == oldDigestObjects(nullptr, 0));
    ObjectRecord single;
    single.id = 77; single.kind = 1; single.damage = 3.5f;
    std::vector<ObjectRecord> oneObject{single};
    CHECK(digestObjects(oneObject) == oldDigestObjects(oneObject.data(), 1));
    CHECK(digestObjects(noObjects) != digestObjects(oneObject));

    std::vector<AblRecord> noAbl;
    std::vector<AblRecord> oneAbl(1);
    CHECK(digestAbl(noAbl) == oldDigestAbl(nullptr, 0));
    CHECK(digestAbl(oneAbl) == oldDigestAbl(oneAbl.data(), 1));
    CHECK(digestAbl(noAbl) != digestAbl(oneAbl));
}
