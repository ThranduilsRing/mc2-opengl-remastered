// tests/unit/test_frame_packets.cpp
// P1 frame packets (RenderCore/FramePackets.h): what a consumer reads is the frame's PUBLISHED view and lighting, it changes only at publish(), and
// the three slot arenas preserve the previous two extractions. No GL, no engine.
#include "doctest.h"
#include <memory>
#include "FramePackets.h"
#include "../../code/render_view.h"

using namespace RenderCore;

TEST_CASE("ground footprint suppresses jumping and airborne movers and restores on landing") {
    using mc2::render_view::groundFootprintRadius;
    mc2::render_view::RenderView view;
    for (float extent : {2.0f, 9.0f}) {
        const float landed = extent < 4.0f ? 4.0f : extent;
        view.value[0] = groundFootprintRadius(extent, false, false);
        CHECK(view.value[0] == landed);
        for (auto state : {std::pair{true, false}, std::pair{true, true}, std::pair{false, true}}) {
            view.value[0] = groundFootprintRadius(extent, state.first, state.second);
            CHECK(view.value[0] == 0.0f);
        }
        view.value[0] = groundFootprintRadius(extent, false, false);
        CHECK(view.value[0] == landed);
    }
}

TEST_CASE("frame draws own ordered streams and HUD values across staging reuse") {
    using namespace mc2::render_view;
    static_assert(std::is_const_v<std::remove_reference_t<decltype(list())>>);
    static_assert(std::is_const_v<std::remove_reference_t<decltype(hud())>>);
    static_assert(std::is_const_v<std::remove_reference_t<decltype(mc2::render_view::clock())>>);
    stagingList().clear();
    stagingHud() = {};
    stagingClock().turn = 17;
    stagingList().terrain.resize(2);
    stagingList().terrain[0].watchId = 12;
    stagingList().terrain[1].watchId = 34;
    stagingList().movers.emplace_back().watchId = 56;
    int effectToken = 0;  // address token only; never dereferenced
    auto* const effect = reinterpret_cast<gosFX::Effect*>(&effectToken);
    auto& fx = stagingList().fx.emplace_back();
    fx.bolt.middleRGB = 0x123456;
    fx.effects[0] = stagingList().retain(effect);
    auto& row = stagingHud().movers.emplace_back();
    row.name = "retained mover";
    row.pilotName = "retained pilot";
    row.waypoints.emplace_back(1, 2, 3);
    row.overviewPath.emplace_back(4, 5, 6);
    auto markerOwner = std::make_shared<int>(42);
    std::weak_ptr<int> markerAlive = markerOwner;
    row.waypointMarkers.emplace_back(markerOwner, nullptr);
    markerOwner.reset();
    stagingHud().roster.push_back(0);
    stagingHud().objectives.emplace_back().description = "retained objective";
    stagingHud().vtols.emplace_back().position = glm::vec3(7, 8, 9);
    auto first = freezeDrawFrame(101);
    REQUIRE(first->sequence == 101);
    FrameArena arena;
    CHECK(drawArena() == nullptr);
    {
        DrawFrameScope scope(*first, &arena);
        CHECK(drawArena() == &arena);
        // Same operations used by extraction/commit must only affect staging.
        stagingList().clear();
        stagingHud() = {};
        stagingClock().turn = 18;
        auto second = freezeDrawFrame(102);
        REQUIRE(second->sequence == 102);
        CHECK(mc2::render_view::clock().turn == 17);
        REQUIRE(list().terrain.size() == 2);
        CHECK(list().terrain[0].watchId == 12);
        CHECK(list().terrain[1].watchId == 34);
        REQUIRE(list().movers.size() == 1);
        CHECK(list().movers[0].watchId == 56);
        REQUIRE(list().fx.size() == 1);
        CHECK(list().fx[0].bolt.middleRGB == 0x123456);
        CHECK(list().effect(list().fx[0].effects[0]) == effect);  // retained table travels with the frozen frame
        REQUIRE(hud().movers.size() == 1);
        CHECK(hud().movers[0].name == "retained mover");
        CHECK(hud().movers[0].pilotName == "retained pilot");
        REQUIRE(hud().movers[0].waypoints.size() == 1);
        CHECK(hud().movers[0].waypoints[0] == glm::vec3(1, 2, 3));
        CHECK(hud().movers[0].overviewPath[0] == glm::vec3(4, 5, 6));
        CHECK(hud().roster[0] == 0);
        CHECK(hud().objectives[0].description == "retained objective");
        CHECK(hud().vtols[0].position == glm::vec3(7, 8, 9));
        CHECK_FALSE(markerAlive.expired());
        {
            DrawFrameScope nested(*second);
            CHECK(drawArena() == nullptr);
            CHECK(mc2::render_view::clock().turn == 18);
            CHECK(list().terrain.empty());
            CHECK(hud().movers.empty());
        }
        CHECK(mc2::render_view::clock().turn == 17);
        CHECK(drawArena() == &arena);
    }
    CHECK(drawArena() == nullptr);
    CHECK(mc2::render_view::clock().turn == 18); // synchronous tool fallback
    CHECK(list().terrain.empty());
    CHECK(hud().movers.empty());
    first.reset();
    CHECK(markerAlive.expired());
    stagingClock() = {};
}

TEST_CASE("fx packets are self-contained values that survive source destruction and a delayed record") {
    using namespace mc2::render_view;
    static_assert(std::is_trivially_copyable_v<FxPacket>);
    // Stand-in sim object: what a bolt extract reads. The effect is only an address token; record never dereferences it here.
    struct FakeBolt { glm::vec3 position; BoltDraw bolt; gosFX::Effect* hit; bool hitExecuted; uint32_t watch; };
    alignas(16) unsigned char effectStorage[2][16]{};
    auto* hitFx = reinterpret_cast<gosFX::Effect*>(effectStorage[0]);
    auto* missFx = reinterpret_cast<gosFX::Effect*>(effectStorage[1]);
    auto extract = [](RenderList& list, const FakeBolt& src) {
        FxPacket& p = list.fx.emplace_back();
        p.position = src.position;
        p.bolt = src.bolt;
        p.watchId = src.watch;
        p.effects[2] = list.retain(src.hit);
        if (src.hit && src.hitExecuted) p.flags |= EffectLive0 << 2;
    };
    RenderList list;
    CHECK(list.retain(nullptr) == 0);
    CHECK(list.effect(0) == nullptr);
    {
        auto source = std::make_unique<FakeBolt>();
        source->position = glm::vec3(7, 8, 9);
        source->bolt.laserVertices[2] = glm::vec3(1, 2, 3);
        source->bolt.middleRGB = 0x123456;
        source->bolt.gosTextureHandle = 91;
        source->bolt.isBeam = true;
        source->hit = hitFx;
        source->hitExecuted = true;
        source->watch = 42;
        extract(list, *source);
        // Sim advances: the pooled slot is re-inited for a different bolt, then the object is destroyed.
        *source = FakeBolt{glm::vec3(-1), BoltDraw{}, missFx, false, 99};
        source.reset();
    }
    const FxPacket frozen = list.fx[0];   // FrameDraws keeps a copy like this until the frame retires
    list.clear();
    FakeBolt next{glm::vec3(5), BoltDraw{}, missFx, false, 7};
    extract(list, next);                  // next frame's extraction reuses the staging list
    REQUIRE(list.fx.size() == 1);
    CHECK(list.effect(list.fx[0].effects[2]) == missFx);
    CHECK_FALSE(list.fx[0].executed(2));
    // The delayed record of the old packet sees only extract-time values.
    CHECK(frozen.position == glm::vec3(7, 8, 9));
    CHECK(frozen.bolt.laserVertices[2] == glm::vec3(1, 2, 3));
    CHECK(frozen.bolt.middleRGB == 0x123456);
    CHECK(frozen.bolt.gosTextureHandle == 91);
    CHECK(frozen.bolt.isBeam);
    CHECK(frozen.watchId == 42);
    CHECK(frozen.executed(2));
    CHECK_FALSE(frozen.executed(3));
    CHECK(frozen.effects[2] == 1);
}

TEST_CASE("fx packet effect ids resolve only against their own list") {
    using namespace mc2::render_view;
    alignas(16) unsigned char storage[3][16]{};
    RenderList list;
    const EffectId a = list.retain(reinterpret_cast<gosFX::Effect*>(storage[0]));
    const EffectId b = list.retain(reinterpret_cast<gosFX::Effect*>(storage[1]));
    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(list.effect(a) == reinterpret_cast<gosFX::Effect*>(storage[0]));
    CHECK(list.effect(b) == reinterpret_cast<gosFX::Effect*>(storage[1]));
    CHECK(list.effect(3) == nullptr);
    RenderList copy = list;               // the frozen frame copy keeps its own table
    list.clear();
    list.retain(reinterpret_cast<gosFX::Effect*>(storage[2]));
    CHECK(copy.effect(a) == reinterpret_cast<gosFX::Effect*>(storage[0]));
    CHECK(list.effect(b) == nullptr);
}

TEST_CASE("terrain dust view keeps its retained effect and live bit after the owner drops it") {
    using namespace mc2::render_view;
    alignas(16) unsigned char storage[16]{};
    RenderView view;                       // extract: effect pointer and live bit sampled once
    view.effects[0] = reinterpret_cast<gosFX::Effect*>(storage);
    view.flags |= EffectLive0;
    RenderView drawn = view;               // the frozen copy the draw reads
    view = RenderView{};                   // owner released (slot re-inited); retire holds the effect itself
    CHECK(drawn.effects[0] == reinterpret_cast<gosFX::Effect*>(storage));
    CHECK(drawn.has(EffectLive0));
    CHECK_FALSE(view.has(EffectLive0));
    CHECK(view.effects[0] == nullptr);     // a stale handle resolves to nothing, never to the old effect
}

namespace {
ViewPacket mainView(float x, uint64_t frame) {
    ViewPacket v;
    v.id = kMainSceneViewId;
    v.frame = frame;
    v.uniforms.cameraWorldPos[0] = x;
    return v;
}
}  // namespace

TEST_SUITE("FramePackets") {

TEST_CASE("before the first publish the main view is the zeroed one") {
    FramePacketPublisher pub;
    CHECK(pub.mainView().id == kInvalidViewId);
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 0.0f);
    CHECK(pub.publishes() == 0);
}

TEST_CASE("a consumer sees the published view; the open set is invisible until publish()") {
    FramePacketPublisher pub;
    FramePackets* a = pub.begin(1);
    REQUIRE(a);
    a->addView(mainView(7.0f, 1));
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 0.0f);   // not published yet
    pub.publish();
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 7.0f);
    CHECK(pub.published().frame == 1);
}

TEST_CASE("negative control: a camera that moves after publish does not change what recording reads") {
    FramePacketPublisher pub;
    float liveCamera = 3.0f;                                     // the live global the islands used to read
    FramePackets* a = pub.begin(1);
    a->addView(mainView(liveCamera, 1));
    pub.publish();
    liveCamera = 99.0f;                                          // a later writer moves the camera mid-frame
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 3.0f);    // the packet still holds the published value
    CHECK(liveCamera != pub.mainView().uniforms.cameraWorldPos[0]);
}

TEST_CASE("the next frame's set replaces the published one only at its publish; the previous set stays intact meanwhile") {
    FramePacketPublisher pub;
    pub.begin(1)->addView(mainView(1.0f, 1));
    pub.publish();
    const FramePackets& f1 = pub.published();
    FramePackets* b = pub.begin(2);
    b->addView(mainView(2.0f, 2));
    CHECK(f1.views[0].uniforms.cameraWorldPos[0] == 1.0f);       // frame 1's slot is untouched while frame 2 is built
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 1.0f);
    pub.publish();
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 2.0f);
    CHECK(f1.views[0].uniforms.cameraWorldPos[0] == 1.0f);
    pub.begin(3)->addView(mainView(3.0f, 3));
    pub.publish();
    CHECK(f1.views[0].uniforms.cameraWorldPos[0] == 1.0f);       // frame 1 is recycled only when frame 4 begins
}

TEST_CASE("a second set in the same frame supersedes the first without disturbing it") {
    FramePacketPublisher pub;
    pub.begin(5)->addView(mainView(1.0f, 5));
    pub.publish();
    const FramePackets& first = pub.published();
    pub.begin(5)->addView(mainView(2.0f, 5));
    pub.publish();
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 2.0f);
    CHECK(first.views[0].uniforms.cameraWorldPos[0] == 1.0f);
}

TEST_CASE("a slot recycled under the published set (a skipped publish) drops it instead of dangling") {
    FramePacketPublisher pub;
    pub.begin(1)->addView(mainView(1.0f, 1));
    pub.publish();
    FramePackets* c = pub.begin(4);                              // slot 1 again: frame 1's arena is reset
    REQUIRE(c);
    CHECK(pub.mainView().id == kInvalidViewId);                  // published_ fell back to the zeroed set, not to reset memory
    c->addView(mainView(4.0f, 4));
    pub.publish();
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 4.0f);
}

TEST_CASE("lighting is carried with the set and a full view list refuses the extra view") {
    FramePacketPublisher pub;
    FramePackets* a = pub.begin(1);
    a->lighting.valid = true;
    a->lighting.scene.sky_number = 4;
    for (uint32_t i = 0; i < kMaxViewsPerFrame; ++i) CHECK(a->addView(mainView(float(i), 1)));
    CHECK_FALSE(a->addView(mainView(9.0f, 1)));
    pub.publish();
    CHECK(pub.published().lighting.valid);
    CHECK(pub.published().lighting.scene.sky_number == 4);
    CHECK(pub.published().viewCount == kMaxViewsPerFrame);
}

TEST_CASE("slot arenas: the first user of a frame resets its slot, later users keep bumping, the other slot is untouched") {
    alignas(16) static uint8_t m0[256], m1[256], m2[256];
    FrameSlotArenas s;
    s.init(m0, m1, m2, 256);
    FrameArena& a1 = s.begin(1);                                 // slot 1
    CHECK(a1.allocArray<uint32_t>(4, "x").size() == 4);
    const size_t used = a1.bytesUsed();
    FrameArena& a1b = s.begin(1);                                // same frame: no reset
    CHECK(&a1 == &a1b);
    CHECK(a1b.bytesUsed() == used);
    FrameArena& a2 = s.begin(2);                                 // slot 2
    CHECK(a2.bytesUsed() == 0);
    CHECK(a1.bytesUsed() == used);                               // the other slot is untouched
    FrameArena& a3 = s.begin(3);                                 // slot 0, both earlier slots untouched
    CHECK(&a3 != &a1);
    CHECK(&a3 != &a2);
    CHECK(a3.bytesUsed() == 0);
    CHECK(a1.bytesUsed() == used);
    CHECK(s.frameOfSlot(1) == 1);
    CHECK(s.frameOfSlot(2) == 2);
    CHECK(s.frameOfSlot(0) == 3);
    FrameArena& a4 = s.begin(4);                                 // slot 1 again: reset
    CHECK(&a4 == &a1);
    CHECK(a4.bytesUsed() == 0);
    CHECK(s.frameOfSlot(1) == 4);
    CHECK(FrameSlotArenas::slotOf(0x100000001ull) == 2);          // reduce the full sequence before narrowing
}

TEST_CASE("retained CPU arenas reject reuse and stale retirement without changing data") {
    alignas(16) uint8_t memory[3][256]{};
    FrameSlotArenas slots;
    slots.init(memory[0], memory[1], memory[2], 256);
    const uint64_t first = 0x100000001ull;
    REQUIRE(slots.retain(first));
    auto values = slots.begin(first).allocArray<uint32_t>(1, "retained");
    values[0] = 42;
    REQUIRE(slots.retain(first + 1));
    REQUIRE(slots.retain(first + 2));
    CHECK_FALSE(slots.retain(first + 3));
    CHECK_FALSE(slots.retire(first + 3));
    CHECK(values[0] == 42);
    CHECK(slots.retire(first + 1));
    CHECK_FALSE(slots.retire(first + 1));
    CHECK_FALSE(slots.retain(first + 3)); // another slot's completion grants nothing
    CHECK(slots.retire(first));
    CHECK(slots.retain(first + 3));
    CHECK(slots.begin(first + 3).bytesUsed() == 0);
    CHECK(slots.retire(first + 2));
    CHECK(slots.retire(first + 3));
}

TEST_CASE("CPU sequence retains published values independently of display frame") {
    FramePacketPublisher pub;
    pub.retain(10);
    pub.begin(7)->addView(mainView(10.0f, 7));
    const auto& first = pub.publish();
    pub.retain(11);
    pub.begin(7)->addView(mainView(11.0f, 7)); // same display frame, different lease
    pub.publish();
    pub.retain(12);
    pub.begin(8)->addView(mainView(12.0f, 8));
    pub.publish();
    CHECK(first.frame == 7);
    CHECK(first.sequence == 10);
    CHECK(first.views[0].uniforms.cameraWorldPos[0] == 10.0f);
    pub.retire(11);
    pub.retire(10);
    pub.retain(13);
    pub.begin(9)->addView(mainView(13.0f, 9));
    CHECK(pub.published().sequence == 12);
    pub.publish();
    pub.retire(12);
    pub.retire(13);
    pub.retain(16); // recycles the latest publication, without publishing
    CHECK(pub.mainView().id == kInvalidViewId);
    pub.retire(16);
}

}  // TEST_SUITE

TEST_CASE("R1 double buffer: extract of N+1 and N+2 never touch the frame N a recorder still reads") {
    using namespace mc2::render_view;
    stagingList().clear();
    stagingHud() = {};
    stagingClock() = {};
    auto extract = [](int turn, uint32_t watch) {   // what extractRenderFrame writes, minus the engine
        stagingClock().turn = turn;
        stagingList().movers.emplace_back().watchId = watch;
        stagingHud().movers.emplace_back().name = "m" + std::to_string(turn);
    };
    extract(1, 100);
    auto n = freezeDrawFrame(201);
    const RenderView* nMover = n->list.movers.data();
    DrawFrameScope recordN(*n);                  // frame N is being recorded ...
    extract(2, 200);                             // ... while N+1 is extracted into the other slot
    CHECK(mc2::render_view::clock().turn == 1);
    REQUIRE(list().movers.size() == 1);
    CHECK(list().movers[0].watchId == 100);
    CHECK(list().movers.data() == nMover);       // N's storage was not reallocated or aliased by the extract
    CHECK(hud().movers[0].name == "m1");
    auto n1 = freezeDrawFrame(202);              // hand-off: the flip. Staging is empty again, N is unchanged
    CHECK(stagingList().movers.empty());
    CHECK(stagingHud().movers.empty());
    extract(3, 300);                             // N+2 extracted while both N and N+1 are alive
    CHECK(n->list.movers[0].watchId == 100);
    REQUIRE(n1->list.movers.size() == 1);
    CHECK(n1->list.movers[0].watchId == 200);
    CHECK(n1->hud.movers[0].name == "m2");
    CHECK(n1->list.movers.data() != n->list.movers.data());
    CHECK(mc2::render_view::clock().turn == 1);  // the recorder's clock is N's
    CHECK(n1->clock.turn == 2);
    stagingList().clear();
    stagingHud() = {};
    stagingClock() = {};
}
