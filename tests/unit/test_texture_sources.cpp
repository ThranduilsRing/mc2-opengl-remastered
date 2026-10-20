// Source admission and ownership use the real service and source index, without a renderer.
#include "doctest.h"
#include "../../mclib/texture_sources_internal.h"
#include "../../mclib/texture_ref.h"

namespace {
struct Sources {
    RenderCore::TextureService service;
    RenderCore::TextureServiceScope scope{service};
    mc2::textures::TextureSources* index;
    explicit Sources(std::function<void(DWORD)> retired={}) {
        auto owner=std::make_unique<mc2::textures::TextureSources>(std::move(retired));
        index=owner.get(); service.ownSources(std::move(owner));
    }
    DWORD add(const char* name, DWORD variant=0) {
        auto source=std::make_shared<mc2::textures::TextureSource>();
        source->name=name; source->variant=variant; source->format=gos_Texture_Solid;
        source->width=source->height=source->logicalWidth=source->logicalHeight=1;
        source->bytes=std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1,2,3,255});
        source->key.source=RenderCore::makeTextureSourceId("unit",name,
            RenderCore::textureContentHash(source->bytes->data(),source->bytes->size()));
        source->bytes=index->internBytes(source->key.source,source->bytes);
        return index->publish(std::move(source));
    }
    RenderCore::TextureHandle handle(DWORD id) { return index->get(id)->handle; }
};
}

TEST_SUITE("TextureSources") {
TEST_CASE("more than 4096 live source assets require no renderer or residency slots") {
    Sources s;
    DWORD last=0;
    for (unsigned i=0;i<5001;++i) {
        const auto name=std::string("sources/")+std::to_string(i);
        last=s.add(name.c_str());
        REQUIRE(last!=0xffffffffu);
    }
    CHECK(last>4096);
    CHECK(s.service.liveCount()==5001);
    CHECK(mc2::textures::assetCapacity()==5002);
    CHECK_FALSE(mc2::textures::isDecoded(last));
    s.index->flush(true);
    CHECK(s.service.liveCount()==0);
}

TEST_CASE("mission roots drop while moved explicit owners survive") {
    Sources s;
    const auto ownedId=s.add("owned");
    const auto rootId=s.add("mission-root");
    const auto ownedHandle=s.handle(ownedId), rootHandle=s.handle(rootId);
    mc2::TextureRef first(ownedId);
    mc2::TextureRef moved(std::move(first));
    CHECK(s.index->get(ownedId)->ownedReferences==1);
    s.index->flush(false);
    CHECK(s.service.isLive(ownedHandle));
    CHECK_FALSE(s.service.isLive(rootHandle));
    CHECK(s.index->get(ownedId)->users==1);
    moved.reset();
    CHECK_FALSE(s.service.isLive(ownedHandle));
}

TEST_CASE("handout returns an owned reference to the caller's mission scope") {
    Sources s;
    const auto id=s.add("handout"); const auto h=s.handle(id);
    mc2::TextureRef owner(id);
    CHECK(owner.release()==id);
    CHECK(s.index->get(id)->ownedReferences==0);
    CHECK(s.index->get(id)->users==1);
    s.index->flush(false);
    CHECK_FALSE(s.service.isLive(h));
}

TEST_CASE("raw release cannot steal an explicitly owned reference") {
    Sources s;
    const auto id=s.add("owned-only"); const auto h=s.handle(id);
    mc2::TextureRef owner(id);
    mc2::textures::release(id);
    CHECK(s.service.isLive(h));
    CHECK(s.index->get(id)->users==1);
    owner.reset();
    CHECK_FALSE(s.service.isLive(h));
}

TEST_CASE("a retained service reference keeps source bytes after the final raw release") {
    Sources s;
    const auto id=s.add("retained"); const auto h=s.handle(id);
    auto retained=RenderCore::ServiceTextureRef::retain(s.service,h);
    std::weak_ptr<RenderCore::TextureSourcePayload> payload=s.service.source(h);
    mc2::textures::release(id);
    CHECK(s.service.isLive(h));
    CHECK(s.service.nodeOf(h)==RenderCore::kNoTextureNode);
    CHECK_FALSE(payload.expired());
    CHECK(mc2::textures::findAndRetain("RETAINED",gos_Texture_Solid,0)==id);
    CHECK(s.service.nodeOf(h)==id);
    mc2::textures::release(id);
    retained={};
    CHECK_FALSE(s.service.isLive(h));
    CHECK(payload.expired());
    const auto replacement=s.add("retained");
    CHECK(replacement!=id);
    CHECK(s.handle(replacement)!=h);
    CHECK_FALSE(static_cast<bool>(s.index->get(id)));
}

TEST_CASE("registration pins retain payloads independently of mission roots") {
    Sources s;
    const auto id=s.add("registered"); const auto h=s.handle(id);
    mc2::textures::pin(id);
    s.index->flush(false);
    CHECK(s.service.isLive(h));
    CHECK(mc2::textures::pinCount(id)==1);
    mc2::textures::unpin(id);
    CHECK_FALSE(s.service.isLive(h));
}

TEST_CASE("CPU consumers retire after the service mutex is released") {
    unsigned callbacks=0;
    Sources s([&](DWORD cpu) {
        CHECK(cpu==42);
        // This re-enters the service: invoking the callback from retire() under
        // its mutex would deadlock here, just as the real UI cleanup would.
        CHECK(RenderCore::textureService().liveCount()==0);
        ++callbacks;
    });
    const auto id=s.add("cpu-consumer"); const auto h=s.handle(id);
    s.index->get(id)->cpuTexture=42;
    auto retained=RenderCore::ServiceTextureRef::retain(s.service,h);
    mc2::textures::release(id);
    CHECK(callbacks==0);
    retained={};
    CHECK(callbacks==0);
    s.service.collectSourceRetirements();
    CHECK(callbacks==1);
    s.service.collectSourceRetirements();
    CHECK(callbacks==1);
}

TEST_CASE("variants share immutable input bytes but own separate source identities") {
    Sources s;
    const auto a=s.add("paint-mask",1), b=s.add("paint-mask",2);
    CHECK(a!=b);
    CHECK(s.handle(a)!=s.handle(b));
    CHECK((s.index->get(a)->bytes==s.index->get(b)->bytes));
    CHECK(mc2::textures::findAndRetain("paint-mask",gos_Texture_Solid,0,2)==b);
    CHECK(s.index->get(b)->users==2);
}
}
