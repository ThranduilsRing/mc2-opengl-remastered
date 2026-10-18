// tests/unit/test_frame_packets.cpp
// P1 frame packets (RenderCore/FramePackets.h): what a consumer reads is the frame's PUBLISHED view and lighting, it changes only at publish(), and
// the slot arenas recycle exactly as the old ping-pong did. No GL, no engine.
#include "doctest.h"
#include "FramePackets.h"

using namespace RenderCore;

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
    CHECK(f1.views[0].uniforms.cameraWorldPos[0] == 1.0f);       // still readable (the frame two back is recycled only when frame 3 begins)
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
    FramePackets* c = pub.begin(3);                              // slot 1 again: frame 1's arena is reset
    REQUIRE(c);
    CHECK(pub.mainView().id == kInvalidViewId);                  // published_ fell back to the zeroed set, not to reset memory
    c->addView(mainView(3.0f, 3));
    pub.publish();
    CHECK(pub.mainView().uniforms.cameraWorldPos[0] == 3.0f);
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
    alignas(16) static uint8_t m0[256], m1[256];
    FrameSlotArenas s;
    s.init(m0, m1, 256);
    FrameArena& a1 = s.begin(1);                                 // slot 1
    CHECK(a1.allocArray<uint32_t>(4, "x").size() == 4);
    const size_t used = a1.bytesUsed();
    FrameArena& a1b = s.begin(1);                                // same frame: no reset
    CHECK(&a1 == &a1b);
    CHECK(a1b.bytesUsed() == used);
    FrameArena& a2 = s.begin(2);                                 // slot 0
    CHECK(a2.bytesUsed() == 0);
    CHECK(a1.bytesUsed() == used);                               // the other slot is untouched
    FrameArena& a3 = s.begin(3);                                 // slot 1 again: reset
    CHECK(&a3 == &a1);
    CHECK(a3.bytesUsed() == 0);
}

}  // TEST_SUITE
