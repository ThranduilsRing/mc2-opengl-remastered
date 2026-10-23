// Weapon impact decals (RenderCore/ImpactDecals.h, ImpactDecalAtlas.h): event -> recipe mapping, the count budget,
// merge/fade/heat rules, anchors, the GPU record geometry and the procedural atlas. Offline, no engine.
#include "doctest.h"

#include "../../RenderCore/ImpactDecals.h"
#include "../../RenderCore/ImpactDecalAtlas.h"

#include <cmath>
#include <set>

using namespace impact;

namespace {
Event ev(WeaponClass w, float x, float y, float t = 0.0f, uint32_t anchor = 0, AnchorKind k = AnchorKind::World) {
    Event e;
    e.weapon = w; e.pos[0] = x; e.pos[1] = y; e.pos[2] = 0.0f; e.time = t;
    e.dir[0] = 1.0f; e.dir[1] = 0.0f; e.dir[2] = 0.0f;
    e.anchorId = anchor; e.anchorKind = k;
    return e;
}
AnchorPose resolveAll(AnchorKind, uint32_t) { return AnchorPose{}; }
}  // namespace

TEST_CASE("impact: weapon classification from the component form and bolt flags") {
    CHECK(classifyWeapon(0, true, false, false, false) == WeaponClass::Laser);
    CHECK(classifyWeapon(0, false, false, false, false) == WeaponClass::Ppc);
    CHECK(classifyWeapon(1, false, false, false, false) == WeaponClass::Ballistic);
    CHECK(classifyWeapon(2, false, true, false, false) == WeaponClass::Missile);
    CHECK(classifyWeapon(1, false, false, true, false) == WeaponClass::Artillery);
    CHECK(classifyWeapon(0, true, false, false, true) == WeaponClass::Splash);
    CHECK(materialFromContentClass(1, Material::Soil) == Material::Concrete);
    CHECK(materialFromContentClass(3, Material::Soil) == Material::SheetMetal);
    CHECK(materialFromContentClass(0, Material::Concrete) == Material::Concrete);
    CHECK(materialFromTerrainType(10) == Material::Concrete);
    CHECK(materialFromTerrainType(4) == Material::Soil);
    CHECK(terrainTypeIsWater(0));
}

TEST_CASE("impact: event-to-decal mapping gives each weapon class a distinct response per material") {
    const WeaponClass classes[] = {WeaponClass::Ballistic, WeaponClass::Laser, WeaponClass::Missile};
    std::set<int> metalTiles, concreteTiles;
    for (WeaponClass w : classes) {
        const Recipe m = recipeFor(w, Material::SheetMetal, 10.0f);
        const Recipe c = recipeFor(w, Material::Concrete, 10.0f);
        CHECK(m.tileHit != c.tileHit);              // sheet metal and concrete respond differently
        metalTiles.insert(int(m.tileHit));
        concreteTiles.insert(int(c.tileHit));
        CHECK(m.lifetime > 0.0f);
        CHECK(m.radius > 0.0f);
    }
    CHECK(metalTiles.size() == 3);
    CHECK(concreteTiles.size() == 3);
    CHECK(recipeFor(WeaponClass::Ballistic, Material::SheetMetal, 5).tileHit == Tile::BulletMetal);
    CHECK(recipeFor(WeaponClass::Laser, Material::SheetMetal, 5).tileHit == Tile::LaserMetal);
    CHECK(recipeFor(WeaponClass::Missile, Material::SheetMetal, 5).tileHit == Tile::BlastMetal);
    CHECK(recipeFor(WeaponClass::Missile, Material::Concrete, 5).tileHit == Tile::BlastGround);
    // A building hit paints the ground at its foot with the ground response of the same weapon.
    CHECK(recipeFor(WeaponClass::Laser, Material::SheetMetal, 5).tileGround == Tile::LaserConcrete);
    // Lasers glow and cool; missiles flash briefly; splash never glows.
    CHECK(recipeFor(WeaponClass::Laser, Material::SheetMetal, 5).glowTau > recipeFor(WeaponClass::Missile, Material::SheetMetal, 5).glowTau);
    CHECK(recipeFor(WeaponClass::Splash, Material::Soil, 40).glow == 0.0f);
    // Mover plating: smaller, shorter-lived, units only (never the ground a mech walks over).
    const Recipe armor = recipeFor(WeaponClass::Ballistic, Material::Armor, 5);
    CHECK(armor.affect == kAffectUnit);
    CHECK(armor.lifetime <= 25.0f);
    CHECK(affectFor(AnchorKind::World, armor) == (kAffectTerrain | kAffectProp));
}

TEST_CASE("impact: the pool never exceeds its capacity and evicts the least remaining life") {
    Pool pool;
    pool.setCapacity(capacityForQuality(1));
    CHECK(pool.capacity() == 96);
    uint64_t spawned = 0;
    for (int frame = 0; frame < 10; ++frame) {
        pool.beginFrame();
        for (int i = 0; i < 20; ++i) {
            const int k = frame * 20 + i;
            const Event e = ev(WeaponClass::Ballistic, float(k) * 100.0f, 0.0f, float(frame));
            if (pool.spawn(e, recipeFor(e.weapon, Material::Soil, 5), AnchorPose{})) ++spawned;
        }
        CHECK(pool.liveCount() <= 96);
    }
    CHECK(spawned == 200);
    CHECK(pool.liveCount() == 96);
    CHECK(pool.stats().evicted == 200 - 96);
    // The survivors are the newest ones (the oldest had the least remaining life).
    float oldest = 1e9f;
    for (uint32_t i = 0; i < pool.capacity(); ++i) if (pool.at(i).live) oldest = std::fmin(oldest, pool.at(i).spawnTime);
    CHECK(oldest >= 5.0f);

    pool.setCapacity(capacityForQuality(0));   // potato: off
    CHECK(pool.liveCount() == 0);
    pool.beginFrame();
    CHECK(pool.spawn(ev(WeaponClass::Laser, 0, 0), recipeFor(WeaponClass::Laser, Material::Soil, 5), AnchorPose{}).has_value() == false);
    CHECK(capacityForQuality(2) == kMaxDecals);
}

TEST_CASE("impact: a burst beyond the per-frame spawn cap is dropped and counted") {
    Pool pool;
    pool.beginFrame();
    for (uint32_t i = 0; i < kMaxSpawnsPerFrame + 10; ++i)
        pool.spawn(ev(WeaponClass::Ballistic, float(i) * 50.0f, 0.0f), recipeFor(WeaponClass::Ballistic, Material::Soil, 5), AnchorPose{});
    CHECK(pool.liveCount() == kMaxSpawnsPerFrame);
    CHECK(pool.stats().droppedBurst == 10);
}

TEST_CASE("impact: sustained fire on one spot merges instead of stacking") {
    Pool pool;
    pool.beginFrame();
    const Recipe r = recipeFor(WeaponClass::Ballistic, Material::Concrete, 5);
    const auto a = pool.spawn(ev(WeaponClass::Ballistic, 10, 10, 0.0f), r, AnchorPose{});
    const auto b = pool.spawn(ev(WeaponClass::Ballistic, 10.5f, 10, 1.0f), r, AnchorPose{});
    const auto c = pool.spawn(ev(WeaponClass::Laser, 10.5f, 10, 1.0f), recipeFor(WeaponClass::Laser, Material::Concrete, 5), AnchorPose{});
    REQUIRE(a); REQUIRE(b); REQUIRE(c);
    CHECK(*a == *b);
    CHECK(*c != *a);                                   // a different weapon class is its own decal
    CHECK(pool.liveCount() == 2);
    CHECK(pool.stats().merged == 1);
    CHECK(pool.at(*a).spawnTime == 1.0f);   // refreshed
    CHECK(pool.at(*a).sizeScale > 1.0f);    // grown, bounded
    for (int i = 0; i < 50; ++i) pool.spawn(ev(WeaponClass::Ballistic, 10, 10, 2.0f), r, AnchorPose{});
    CHECK(pool.at(*a).sizeScale <= 1.6f);
}

TEST_CASE("impact: fade, laser glow cooling and the hot-first record order") {
    Pool pool;
    pool.beginFrame();
    const Recipe laser = recipeFor(WeaponClass::Laser, Material::SheetMetal, 5);
    const Recipe bullet = recipeFor(WeaponClass::Ballistic, Material::Concrete, 5);
    pool.spawn(ev(WeaponClass::Ballistic, 0, 0, 0.0f), bullet, AnchorPose{});
    pool.spawn(ev(WeaponClass::Laser, 500, 0, 0.0f), laser, AnchorPose{});
    const Pool::Decal& dl = pool.at(1);
    CHECK(Pool::heat(dl, 0.0f) == doctest::Approx(1.0f));
    CHECK(Pool::heat(dl, laser.glowTau) == doctest::Approx(std::exp(-1.0f)));
    CHECK(Pool::heat(dl, 10.0f) < 0.01f);                       // cooled after a few seconds
    CHECK(Pool::fadeAlpha(dl, 1.0f) == doctest::Approx(dl.intensity));
    CHECK(Pool::fadeAlpha(dl, laser.lifetime * 0.85f) < dl.intensity);
    CHECK(Pool::fadeAlpha(dl, laser.lifetime + 0.1f) == 0.0f);

    GpuDecal out[4];
    uint32_t hot = 0;
    CHECK(pool.build(0.5f, resolveAll, out, 4, hot) == 2);
    CHECK(hot == 1);
    CHECK(out[0].v[1][3] > 0.5f);                               // the glowing laser comes first
    CHECK(out[0].v[4][3] == float(Tile::LaserMetal));
    CHECK(pool.build(20.0f, resolveAll, out, 4, hot) == 2);
    CHECK(hot == 0);                                            // cooled: no additive draw

    pool.update(bullet.lifetime + 1.0f, resolveAll);
    CHECK(pool.liveCount() == 1);                               // the bullet expired, the laser (longer life) remains
    pool.update(laser.lifetime + 1.0f, resolveAll);
    CHECK(pool.liveCount() == 0);
    CHECK(pool.stats().expired == 2);
}

TEST_CASE("impact: mover decals follow their anchor and die with it") {
    Pool pool;
    pool.beginFrame();
    AnchorPose mech; mech.pos[0] = 100; mech.pos[1] = 200; mech.pos[2] = 0; mech.yawDeg = 0;
    Event e = ev(WeaponClass::Ballistic, 103, 200, 0.0f, 42, AnchorKind::Mover);
    e.pos[2] = 20.0f;
    pool.spawn(e, recipeFor(e.weapon, Material::Armor, 5), mech);
    AnchorPose moved = mech; moved.pos[0] = 150; moved.yawDeg = 90.0f;
    bool alive = true;
    auto resolve = [&](AnchorKind, uint32_t) { AnchorPose p = moved; p.valid = alive; return p; };
    GpuDecal g[1];
    uint32_t hot = 0;
    REQUIRE(pool.build(0.1f, resolve, g, 1, hot) == 1);
    // Local offset (3, 0, 20) rotated by 90 degrees about z lands at (150, 203, 20).
    CHECK(g[0].v[0][0] == doctest::Approx(150.0f));
    CHECK(g[0].v[0][1] == doctest::Approx(203.0f));
    CHECK(g[0].v[0][2] == doctest::Approx(20.0f));
    CHECK(uint32_t(g[0].v[6][3]) == kAffectUnit);
    alive = false;
    pool.update(0.2f, resolve);
    CHECK(pool.liveCount() == 0);
    CHECK(pool.stats().anchorLost == 1);
}

TEST_CASE("impact: the GPU box contains the hit point and its axes are orthogonal") {
    Pool pool;
    pool.beginFrame();
    Event e = ev(WeaponClass::Missile, 10, 20, 0.0f, 7, AnchorKind::Static);
    e.pos[2] = 30.0f; e.extent = 40.0f;
    e.dir[0] = 0.6f; e.dir[1] = 0.8f; e.dir[2] = 0.0f;
    pool.spawn(e, recipeFor(e.weapon, Material::SheetMetal, 10), AnchorPose{});
    GpuDecal g;
    Pool::writeGpu(pool.at(0), 0.0f, AnchorPose{}, g);
    auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    CHECK(std::fabs(dot3(g.v[1], g.v[2])) < 1e-3f);
    CHECK(std::fabs(dot3(g.v[1], g.v[3])) < 1e-3f);
    CHECK(std::fabs(dot3(g.v[2], g.v[3])) < 1e-3f);
    // Local coordinates of the hit point: centred across the shot, inside the box along it.
    const float rel[3] = {e.pos[0] - g.v[0][0], e.pos[1] - g.v[0][1], e.pos[2] - g.v[0][2]};
    CHECK(std::fabs(dot3(rel, g.v[4])) < 1e-3f);
    CHECK(std::fabs(dot3(rel, g.v[5])) < 1e-3f);
    const float lz = dot3(rel, g.v[6]);
    CHECK(lz > 0.0f);
    CHECK(lz < 1.0f);
    // The projection axis follows the shot.
    CHECK(dot3(g.v[3], e.dir) > 0.0f);
    // A world (terrain) decal projects straight down.
    pool.spawn(ev(WeaponClass::Missile, 300, 0), recipeFor(WeaponClass::Missile, Material::Soil, 10), AnchorPose{});
    Pool::writeGpu(pool.at(1), 0.0f, AnchorPose{}, g);
    CHECK(g.v[3][2] < 0.0f);
    CHECK(std::fabs(g.v[3][0]) < 1e-5f);
}

TEST_CASE("impact: the event queue is bounded and drops the oldest") {
    EventQueue q;
    for (uint32_t i = 0; i < kMaxEventsPerFrame + 5; ++i) { Event e; e.tick = i; q.push(e); }
    CHECK(q.size() == kMaxEventsPerFrame);
    CHECK(q.dropped() == 5);
    uint32_t first = 0, n = 0;
    q.drain([&](const Event& e) { if (n++ == 0) first = e.tick; });
    CHECK(first == 5);
    CHECK(q.size() == 0);
}

TEST_CASE("impact: presentation variation is a pure function of the event") {
    const Event a = ev(WeaponClass::Laser, 12.3f, 45.6f, 1.0f);
    Event b = a;
    CHECK(presentationSeed(a) == presentationSeed(b));
    b.tick = 99;
    CHECK(presentationSeed(a) != presentationSeed(b));
}

TEST_CASE("impact: the procedural atlas is deterministic, edge-clean and readable") {
    const atlas::Layers a = atlas::generate();
    const atlas::Layers b = atlas::generate();
    CHECK(a.surface == b.surface);
    CHECK(a.normal == b.normal);
    const uint32_t e = atlas::kSize;
    REQUIRE(a.surface.size() == size_t(e) * e * 4 * kTileCount);
    for (uint32_t layer = 0; layer < kTileCount; ++layer) {
        const uint8_t* s = a.surface.data() + size_t(layer) * e * e * 4;
        // No coverage on the border: the box edge never shows.
        for (uint32_t i = 0; i < e; ++i) {
            CHECK(s[(size_t(0) * e + i) * 4 + 3] == 0);
            CHECK(s[(size_t(e - 1) * e + i) * 4 + 3] == 0);
        }
    }
    // The bullet hole and the breach centres are black and fully covered; the laser groove core glows.
    auto texel = [&](Tile t, uint32_t x, uint32_t y) { return a.surface.data() + (size_t(uint32_t(t)) * e * e + size_t(y) * e + x) * 4; };
    CHECK(texel(Tile::BulletMetal, e / 2, e / 2)[0] < 10);
    CHECK(texel(Tile::BulletMetal, e / 2, e / 2)[3] == 255);
    CHECK(texel(Tile::BlastMetal, e / 2, e / 2)[0] < 10);
    CHECK(texel(Tile::LaserMetal, e / 2, e / 2)[1] > 128);
    CHECK(texel(Tile::SplashScorch, e / 2, e / 2)[1] == 0);
}
