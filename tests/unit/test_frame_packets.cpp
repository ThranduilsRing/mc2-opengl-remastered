// tests/unit/test_frame_packets.cpp
// P1 frame packets (RenderCore/FramePackets.h): what a consumer reads is the frame's PUBLISHED view and lighting, it changes only at publish(), and
// the three slot arenas preserve the previous two extractions. No GL, no engine.
#include "doctest.h"
#include "FramePackets.h"
#include "../../code/render_view.h"

using namespace RenderCore;

TEST_CASE("bolt draw packet survives staging mutation and a later extraction") {
    using namespace mc2::render_view;
    alignas(16) unsigned char memory[3][2048]{};
    FrameSlotArenas slots;
    slots.init(memory[0], memory[1], memory[2], sizeof(memory[0]));
    REQUIRE(slots.retain(1));
    RenderList list;
    list.freezeBoltDraws(slots.begin(1));
    CHECK(slots.begin(1).stats().usedBytes == 0);
    list.other.resize(2);
    auto& source = list.other[1];
    source.flags = Visible | BoltGeometry;
    source.bolt.laserVertices[2] = glm::vec3(1, 2, 3);
    source.bolt.laserSide[3] = glm::vec3(4, 5, 6);
    source.bolt.startUV = 0.25f;
    source.bolt.uvRepeat = 2;
    source.bolt.unitLength = 3;
    source.bolt.middleRGB = 0x123456;
    source.bolt.boltAlpha = 127;
    source.bolt.gosTextureHandle = 91;
    source.bolt.mcTextureHandle = 92;
    source.bolt.isBeam = true;
    list.freezeBoltDraws(slots.begin(1));
    CHECK(list.other[0].frozenBolt == nullptr);
    const auto* first = source.frozenBolt;
    REQUIRE(first);
    CHECK(first != &source.bolt);
    source.bolt = {};
    list.clear();
    REQUIRE(slots.retain(2));
    list.other.emplace_back().flags = BoltGeometry;
    list.freezeBoltDraws(slots.begin(2));
    CHECK(list.other[0].frozenBolt != first);
    CHECK(first->laserVertices[2] == glm::vec3(1, 2, 3));
    CHECK(first->laserSide[3] == glm::vec3(4, 5, 6));
    CHECK(first->startUV == 0.25f);
    CHECK(first->uvRepeat == 2);
    CHECK(first->unitLength == 3);
    CHECK(first->middleRGB == 0x123456);
    CHECK(first->boltAlpha == 127);
    CHECK(first->gosTextureHandle == 91);
    CHECK(first->mcTextureHandle == 92);
    CHECK(first->isBeam);
    CHECK_FALSE(slots.retain(4));
    CHECK(slots.retire(2));
    CHECK(slots.retire(1));
    CHECK(slots.retain(4));
    CHECK(slots.retire(4));
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
