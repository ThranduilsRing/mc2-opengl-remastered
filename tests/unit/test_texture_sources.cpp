#include "../../RenderCore/TextureService.h"
// Source admission and ownership use the real service and source index, without a renderer.
#include "doctest.h"
#include "../../mclib/texture_sources_internal.h"
#include "../../mclib/texture_ref.h"
#include "../../mclib/sim_overlap.h"
#include <barrier>
#include <future>

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
TEST_CASE("load prewarm selects live cold CPU sources in admission order") {
    Sources s;
    const auto cold=s.add("offscreen"), warm=s.add("first-view"), retired=s.add("retired");
    s.index->get(warm)->cpuTexture=17;
    auto retiredSnapshot=s.index->get(retired);
    mc2::textures::release(retired);
    const auto next=s.add("another-offscreen");
    CHECK(s.index->coldCpuTextures()==std::vector<DWORD>{cold,next});
    s.index->get(cold)->cpuTexture=18;
    s.index->get(next)->cpuTexture=19;
    CHECK(s.index->coldCpuTextures().empty());
    CHECK(retiredSnapshot->cpuTexture==0);
}
TEST_CASE("LOD replacement retains the same painted CPU texture before release") {
    std::vector<DWORD> destroyed;
    Sources s([&](DWORD texture) { destroyed.push_back(texture); });
    const auto old=s.add("painted-atlas",7);
    s.index->get(old)->cpuTexture=71;
    DWORD replacement=0;
    {
        mc2::TextureRef previous(old);
        replacement=mc2::textures::findAndRetain("painted-atlas",gos_Texture_Solid,0,7);
        REQUIRE(replacement==old);
    }
    CHECK(destroyed.empty());
    REQUIRE(s.index->get(replacement));
    CHECK(s.index->get(replacement)->cpuTexture==71);
    CHECK(s.index->get(replacement)->users==1);
    CHECK(s.index->get(replacement)->ownedReferences==0);
    mc2::textures::release(replacement);
    CHECK(destroyed==std::vector<DWORD>{71});
}
TEST_CASE("source admission overlaps named lookup byte interning and snapshots") {
    Sources s;
    const auto base=s.add("concurrent/base");
    auto bytes=std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1,2,3,4});
    const auto interned=s.index->internBytes("concurrent/shared",bytes);
    std::barrier start(2);
    auto run=[&](unsigned worker) {
        start.arrive_and_wait();
        for (unsigned i=0;i<512;++i) {
            const auto name="concurrent/"+std::to_string(worker)+"/"+std::to_string(i);
            const auto id=s.add(name.c_str());
            auto found=s.index->find(name.c_str(),0);
            auto snapshot=s.index->assets();
            if (!found || found->asset!=id || !s.index->find("concurrent/base",0) ||
                s.index->find("missing",0) || snapshot.size()<=id || snapshot[id].expired() ||
                s.index->internBytes("concurrent/shared",bytes)!=interned || !s.index->get(base))
                return false;
        }
        return true;
    };
    auto a=std::async(std::launch::async,run,0);
    auto b=std::async(std::launch::async,run,1);
    CHECK(a.get()); CHECK(b.get());
    CHECK(s.index->capacity()==1026);
}

TEST_CASE("worker texture release retires only at the join") {
    std::vector<DWORD> destroyed;
    Sources s([&](DWORD texture) { destroyed.push_back(texture); });
    const auto id=s.add("worker-retire");
    s.index->get(id)->cpuTexture=71;
    mc2::overlap::runOnWorker([&] {
        s.index->requestCreate(id);
        mc2::textures::release(id);
    });
    mc2::overlap::waitWorker();
    CHECK(destroyed.empty());
    CHECK(s.index->takeCreates()==std::vector<DWORD>{id});
    CHECK(s.index->takeCreates().empty());
    s.index->collectRetired();
    CHECK(destroyed==std::vector<DWORD>{71});
    s.index->collectRetired();
    CHECK(destroyed.size()==1);
}
TEST_CASE("cooked images retire across distinct missions while live handles survive") {
    Sources s;
    RenderCore::TextureKey persistent; persistent.source="persistent-ui";
    const auto live=s.service.acquire(persistent,RenderCore::kNoTextureNode,{32,16,32,16});
    for (int mission=0;mission<4;++mission) {
        std::vector<std::weak_ptr<const RenderCore::KtxImage>> images;
        RenderCore::TextureKey key;
        for (int i=0;i<16;++i) {
            key.source="mission/"+std::to_string(mission)+"/sidecar/"+std::to_string(i);
            auto image=std::make_shared<RenderCore::KtxImage>();
            image->pixels.resize(size_t(mission+1)*(i+1)*1024);
            images.push_back(image); s.service.cacheCooked(key,std::move(image));
            const auto h=s.service.acquire(key);
            REQUIRE(s.service.release(h)); // source retirement alone did not evict cooked bytes
        }
        for (const auto& image:images) REQUIRE_FALSE(image.expired());
        auto pending=s.service.cooked(key); // a pending upload may legitimately retain its pixels
        const auto size=pending->pixels.size();
        s.index->flush(false); // the real mission boundary
        CHECK_FALSE(bool(s.service.cooked(key)));
        CHECK(s.service.isLive(live)); CHECK(s.service.dimensions(live).width==32);
        CHECK(pending->pixels.size()==size);
        for (size_t i=0;i+1<images.size();++i) CHECK(images[i].expired());
        CHECK_FALSE(images.back().expired());
        pending.reset();
        for (const auto& image:images) CHECK(image.expired()); // cache-owned retained bytes return to zero
    }
}

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
