// tests/unit/test_texture_service.cpp
// P4a texture and material identity (RenderCore/TextureService.h, MaterialRegistry.h): key dedupe, mod-precedence keys, paint variants, handles that
// survive a content change, stale handles after the node dies, material and material-set dedupe. No engine, no GPU.
#include "doctest.h"
#include "MaterialRegistry.h"
#include "TextureService.h"
#include "StockPbr.h"
#include "TextureStream.h"
#include "../../GameOS/gameos/vk_texture_pending.h"
#include <future>
#include <barrier>
#include <thread>
#include "../../GameOS/gameos/vk_completion_tracker.h"

using namespace RenderCore;

TEST_CASE("StockPbr resolves stock UV atlas names and preserves mod priority") {
    CHECK(stockPbrArchive(25925639, 0x2620c4b9));
    CHECK(stockPbrArchive(34295854, 0xaef6a9dd));
    CHECK_FALSE(stockPbrArchive(34295854, 0xaef6a9de));
    CHECK_FALSE(stockPbrArchive(25925639, 0x2620c4ba));
    CHECK_FALSE(stockPbrArchive(25925640, 0x2620c4b9));
    CHECK(stockPbrStem("data/tgl/128/MadCatRGB.tga") == "madcat");
    CHECK(stockPbrStem("data\\tgl\\128\\AtlasRGB.tga") == "atlas");
    CHECK(stockPbrStem("ShootistRGB.tga") == "shootist");
    CHECK(stockPbrStem("UrbanMechRGB.ktx2") == "urbanmech");
    CHECK(stockPbrStem("VutlureRGB.tga") == "vutlure");
    CHECK(stockPbrStem("vtolrgb.tga") == "vtolrgb");
    CHECK(stockPbrStem("vedette.tga") == "vedette");
    CHECK(preferStockPbr(true, false, 100, 100));
    CHECK_FALSE(preferStockPbr(false, false, 100, 100));
    CHECK_FALSE(preferStockPbr(true, true, 0, 100));
    CHECK_FALSE(preferStockPbr(true, true, 0, 0));
    CHECK(preferStockPbr(true, true, 1, 0));
}

namespace {
TextureKey keyOf(const char* provider, const char* path, uint64_t hash, uint32_t variant = 0, uint32_t format = 1, uint32_t hints = 0) {
    TextureKey k;
    k.source = makeTextureSourceId(provider, path, hash);
    k.variant = variant;
    k.format = format;
    k.hints = hints;
    return k;
}
}  // namespace

TEST_SUITE("TextureService") {

TEST_CASE("the same key is one handle; a second acquire raises the user count and keeps the first residency") {
    TextureService s;
    const TextureHandle a = s.acquire(keyOf("base", "data/tgl/mech.tga", 0xAB), /*node=*/7);
    const TextureHandle b = s.acquire(keyOf("base", "data/tgl/mech.tga", 0xAB), /*node=*/9);
    CHECK(a == b);
    CHECK(s.users(a) == 2);
    CHECK(s.nodeOf(a) == 7);
    CHECK(s.liveCount() == 1);
}

TEST_CASE("shared-key node aliases survive canonical destruction without leaking users") {
    TextureService s;
    const auto key=keyOf("base","data/art/foo.tga",17);
    const auto first=s.acquire(key,7);
    REQUIRE(s.acquire(key,9)==first);
    CHECK(s.handleOfNode(7)==first); CHECK(s.handleOfNode(9)==first);
    REQUIRE(s.retainNode(9)); CHECK(s.users(first)==3);
    s.dropRef(9); CHECK(s.users(first)==2);
    s.onNodeDestroyed(7);
    CHECK(s.isLive(first)); CHECK(s.nodeOf(first)==9); CHECK(s.users(first)==1);
    CHECK_FALSE(s.handleOfNode(7).isValid());
    const auto replacement=s.acquire(keyOf("base","other",19),7);
    CHECK(replacement!=first); CHECK(s.handleOfNode(9)==first);
    s.dropRef(9); CHECK(s.users(first)==0); CHECK(s.isLive(first));
    s.onNodeDestroyed(9); CHECK_FALSE(s.isLive(first));
    CHECK(s.isLive(replacement));
}

TEST_CASE("shared-key aliases remain indexed across rebind and clear") {
    TextureService s;
    const auto key=keyOf("base","a",1);
    const auto h=s.acquire(key,1); REQUIRE(s.acquire(key,2)==h);
    REQUIRE(s.rebind(h,3));
    CHECK_FALSE(s.handleOfNode(1).isValid());
    CHECK(s.handleOfNode(2)==h); CHECK(s.handleOfNode(3)==h);
    s.onNodeDestroyed(2); CHECK(s.isLive(h)); CHECK(s.users(h)==1);
    s.clear();
    CHECK_FALSE(s.handleOfNode(3).isValid()); CHECK_FALSE(s.isLive(h));
}
TEST_CASE("dense aliases survive swap removal rebind and entry reuse") {
    TextureService s; const auto key = keyOf("base", "aliases", 1);
    const auto h = s.acquire(key, 1);
    for (uint32_t n = 2; n <= 64; ++n) REQUIRE(s.acquire(key, n) == h);
    REQUIRE(s.rebind(h, 64)); CHECK(s.users(h) == 64);
    for (uint32_t n = 2; n < 64; ++n) s.onNodeDestroyed(n);
    CHECK(s.nodeOf(h) == 64); CHECK(s.users(h) == 2);
    REQUIRE(s.retain(h)); s.onNodeDestroyed(64);
    CHECK(s.isLive(h)); CHECK(s.nodeOf(h) == kNoTextureNode);
    REQUIRE(s.retainSource(h, 7)); REQUIRE(s.rebind(h, kNoTextureNode));
    CHECK_FALSE(s.handleOfNode(7).isValid());
    s.clear(); const auto next = s.acquire(key, 8);
    CHECK(next != h); s.onNodeDestroyed(8); CHECK_FALSE(s.isLive(next));
}

TEST_CASE("path spelling is folded: case and separators do not make a new key") {
    TextureKey a = keyOf("base", "Data\\TGL\\Mech.TGA", 0xAB);
    TextureKey b = keyOf("base", "data/tgl/mech.tga", 0xAB);
    CHECK(a == b);
    TextureService s;
    CHECK(s.acquire(a, 1) == s.acquire(b, 2));
}

TEST_CASE("mod precedence: a mod file that outranks the base file is a different key, hence a different handle") {
    TextureService s;
    const TextureHandle base = s.acquire(keyOf("base", "data/tgl/mech.tga", 0xAB), 1);
    const TextureHandle mod = s.acquire(keyOf("mod:Exodus", "data/tgl/mech.tga", 0xCD), 2);
    CHECK(base != mod);
    CHECK(s.nodeOf(base) == 1);
    CHECK(s.nodeOf(mod) == 2);
    // The same provider with different bytes is also distinct (the content hash is part of the identity).
    CHECK(s.acquire(keyOf("mod:Exodus", "data/tgl/mech.tga", 0xCE), 3) != mod);
}

TEST_CASE("paint variants of one source are distinct keys; usage is part of the key") {
    TextureService s;
    const TextureHandle plain = s.acquire(keyOf("base", "mech.tga", 1, /*variant=*/0), 1);
    const TextureHandle paintA = s.acquire(keyOf("base", "mech.tga", 1, /*variant=*/0xA11CE), 2);
    const TextureHandle paintB = s.acquire(keyOf("base", "mech.tga", 1, /*variant=*/0xB0B), 3);
    CHECK(plain != paintA);
    CHECK(paintA != paintB);
    const TextureHandle otherUsage = s.acquire(keyOf("base", "mech.tga", 1, 0, /*format=*/2), 4);
    CHECK(otherUsage != plain);
}

TEST_CASE("a content change keeps the handle, moves the residency and bumps the content version") {
    TextureService s;
    const TextureHandle h = s.acquire(keyOf("base", "a.tga", 1), 5);
    CHECK(s.contentVersion(h) == 0);
    REQUIRE(s.rebind(h, 11));
    CHECK(s.nodeOf(h) == 11);
    CHECK(s.contentVersion(h) == 1);
    CHECK(s.handleOfNode(11) == h);
    CHECK_FALSE(s.handleOfNode(5).isValid());
}

TEST_CASE("negative control: a handle held across the node's death is stale, and a recycled index does not revive it") {
    TextureService s;
    const TextureHandle old = s.acquire(keyOf("base", "a.tga", 1), 5);
    s.onNodeDestroyed(5);
    CHECK_FALSE(s.isLive(old));
    CHECK(s.nodeOf(old) == kNoTextureNode);
    const TextureHandle fresh = s.acquire(keyOf("base", "b.tga", 2), 5);   // the manager reuses node 5; the service reuses the entry index
    CHECK(fresh.index() == old.index());
    CHECK(fresh != old);
    CHECK_FALSE(s.isLive(old));
    CHECK(s.isLive(fresh));
    CHECK_FALSE(s.rebind(old, 99));                                         // a stale handle cannot move the new texture
    CHECK(s.nodeOf(fresh) == 5);
}

TEST_CASE("dropRef mirrors the manager's count and the entry lives until the node dies") {
    TextureService s;
    const TextureHandle h = s.acquire(keyOf("base", "a.tga", 1), 5);
    s.acquire(keyOf("base", "a.tga", 1), 5);
    CHECK(s.users(h) == 2);
    s.dropRef(5);
    CHECK(s.users(h) == 1);
    s.dropRef(5);
    CHECK(s.users(h) == 0);
    CHECK(s.isLive(h));
    s.onNodeDestroyed(5);
    CHECK_FALSE(s.isLive(h));
}

TEST_CASE("clear() at a mission boundary retires every entry") {
    TextureService s;
    const TextureHandle a = s.acquire(keyOf("base", "a.tga", 1), 1);
    const TextureHandle b = s.acquire(keyOf("base", "b.tga", 2), 2);
    s.clear();
    CHECK(s.liveCount() == 0);
    CHECK_FALSE(s.isLive(a));
    CHECK_FALSE(s.isLive(b));
}

}  // TEST_SUITE

namespace {
MaterialKey matKey(uint64_t mesh, uint32_t slot, TextureHandle albedo, uint32_t paint = 0) {
    MaterialKey k;
    k.meshKey = mesh;
    k.slot = slot;
    k.albedo = albedo;
    k.paintVariant = paint;
    return k;
}
}  // namespace

TEST_SUITE("MaterialRegistry") {

TEST_CASE("MaterialRegistry refuses indices beyond the handle field") {
    MaterialRegistry r;
    for (uint32_t i = 0; i < 0x100000u; ++i)
        REQUIRE(r.acquire(matKey(i, 0, TextureHandle::make(1, 1))).index() == i);
    CHECK_FALSE(r.acquire(matKey(0x100000u, 0, TextureHandle::make(1, 1))).isValid());
    CHECK(r.materialCount() == 0x100000u);
    CHECK(r.acquire(matKey(0, 0, TextureHandle::make(1, 1))) == MaterialHandle::make(0, 1));
}

TEST_CASE("equal keys are one material; the semantic of the unified table is the descriptor index") {
    MaterialRegistry r;
    const TextureHandle t = TextureHandle::make(3, 1);
    CHECK(r.acquire(matKey(1, 0, t)) == r.acquire(matKey(1, 0, t)));
    CHECK(r.materialCount() == 1);
    CHECK(r.acquire(matKey(1, 1, t)) != r.acquire(matKey(1, 0, t)));          // another slot of the same mesh
    CHECK(kRegistryTextureSemantic == MaterialTextureSemantic::DescriptorIndex);
}

TEST_CASE("two differently painted instances of one mesh: two materials per painted slot, two sets, one mesh key") {
    MaterialRegistry r;
    const TextureHandle plain = TextureHandle::make(1, 1), paintedA = TextureHandle::make(2, 1), paintedB = TextureHandle::make(3, 1);
    const uint64_t mesh = 0xFEED;
    auto setFor = [&](TextureHandle body, uint32_t paint) {
        std::vector<MaterialHandle> slots = {r.acquire(matKey(mesh, 0, body, paint)), r.acquire(matKey(mesh, 1, plain))};
        return r.acquireSet(slots);
    };
    const MaterialSetRef a = setFor(paintedA, 0xA);
    const MaterialSetRef b = setFor(paintedB, 0xB);
    const MaterialSetRef a2 = setFor(paintedA, 0xA);
    CHECK(a == a2);                                                            // same paint: the set is shared
    CHECK_FALSE(a == b);
    CHECK(r.materialCount() == 3);                                             // body A, body B, the shared unpainted second slot
    CHECK(r.materialOfSetSlot(a, 1) == r.materialOfSetSlot(b, 1));            // the unpainted slot is the same material in both sets
    CHECK(r.materialOfSetSlot(a, 0) != r.materialOfSetSlot(b, 0));
    CHECK_FALSE(r.materialOfSetSlot(a, 2).isValid());                          // past the last slot
}

TEST_CASE("a set is a contiguous range in slot order") {
    MaterialRegistry r;
    const TextureHandle t = TextureHandle::make(1, 1);
    const MaterialHandle m0 = r.acquire(matKey(1, 0, t)), m1 = r.acquire(matKey(1, 1, t)), m2 = r.acquire(matKey(1, 2, t));
    const MaterialSetRef s = r.acquireSet({m0, m1, m2});
    CHECK(s.count == 3);
    CHECK(r.materialOfSetSlot(s, 0) == m0);
    CHECK(r.materialOfSetSlot(s, 1) == m1);
    CHECK(r.materialOfSetSlot(s, 2) == m2);
    const MaterialSetRef s2 = r.acquireSet({m2, m1, m0});                      // a different order is a different set
    CHECK_FALSE(s == s2);
    CHECK(s2.base == s.base + 3);
}

TEST_CASE("material admission overlaps lookups without invalidating keys") {
    MaterialRegistry registry;
    const auto firstKey = matKey(1, 0, TextureHandle::make(1, 1));
    const auto first = registry.acquire(firstKey);
    const auto* saved = registry.keyOf(first);
    std::barrier start(4);
    std::vector<std::future<bool>> workers;
    for (unsigned worker = 0; worker < 4; ++worker) {
        workers.push_back(std::async(std::launch::async, [&, worker] {
            start.arrive_and_wait();
            for (unsigned i = 0; i < 2048; ++i) {
                const auto key = matKey(2 + i, worker, TextureHandle::make(1, 1));
                const auto h = registry.acquire(key);
                const auto set = registry.acquireSet({first, h});
                if (registry.acquire(firstKey) != first || !registry.owns(h) ||
                    registry.keyOf(first) != saved || !(*saved == firstKey) ||
                    !(*registry.keyOf(h) == key) || registry.materialOfSetSlot(set, 1) != h)
                    return false;
            }
            return true;
        }));
    }
    for (auto& worker : workers) CHECK(worker.get());
    CHECK(registry.materialCount() == 8193);
    CHECK(registry.setCount() == 8192);
}

TEST_CASE("handles and set refs the registry did not issue are refused") {
    MaterialRegistry r;
    const TextureHandle t = TextureHandle::make(1, 1);
    const MaterialHandle m0 = r.acquire(matKey(1, 0, t)), m1 = r.acquire(matKey(1, 1, t));
    CHECK(r.keyOf(m0) != nullptr);
    CHECK(r.keyOf(MaterialHandle::make(m0.index(), 2)) == nullptr);            // wrong generation
    CHECK(r.keyOf(MaterialHandle::make(99, 1)) == nullptr);                    // index out of range
    CHECK(r.keyOf(MaterialHandle::invalid()) == nullptr);
    CHECK_FALSE(r.acquireSet({m0, MaterialHandle::make(99, 1)}).valid());      // a foreign handle gets no set
    CHECK_FALSE(r.acquireSet({m0, MaterialHandle::make(m1.index(), 3)}).valid());
    const MaterialSetRef s = r.acquireSet({m0, m1});
    CHECK(r.materialOfSetSlot(s, 1) == m1);
    CHECK_FALSE(r.materialOfSetSlot(s, 2).isValid());                          // slot past the set
    MaterialSetRef forged = s; forged.base = 1;                                // a base in the middle of a set
    CHECK_FALSE(r.materialOfSetSlot(forged, 0).isValid());
    MaterialSetRef big = s; big.count = 50;                                    // a count past the table
    CHECK_FALSE(r.materialOfSetSlot(big, 3).isValid());
}


TEST_CASE("TXM material slots reject registration snapshots after replacement and node reuse") {
    TextureService service;
    TextureKey source; source.source="mod:props/a.tga";
    const auto a=service.acquire(source,7);
    source.source="mod:props/b.tga"; const auto b=service.acquire(source,8);
    auto& materials=service.materials();
    auto bind=[&](TextureHandle texture) { return materials.acquireSet({materials.acquire(matKey(0x1234,0,texture))}); };
    auto textureOf=[&](MaterialSetRef set) { return materials.keyOf(materials.materialOfSetSlot(set,0))->albedo; };
    const auto first=bind(a), sibling=bind(b);
    CHECK_FALSE(first==sibling);
    CHECK(textureOf(first)==a); CHECK(textureOf(sibling)==b);
    const auto registrationSnapshot=first;
    const auto replaced=bind(b);
    CHECK(replaced==sibling);
    CHECK_FALSE(textureOf(registrationSnapshot)==textureOf(replaced)); // negative control must reject the cached registration handle
    service.release(a);
    CHECK(service.isLive(a)); // node identity remains live until the manager destroys its node
    service.onNodeDestroyed(7);
    source.source="mod:props/damage.tga"; const auto damage=service.acquire(source,7);
    CHECK(damage!=a); CHECK_FALSE(service.isLive(textureOf(registrationSnapshot)));
    CHECK(textureOf(bind(damage))==damage);
    CHECK(textureOf(sibling)==b); // late spawn / damage swap never mutates the sibling
    service.clear();
    CHECK_FALSE(service.isLive(damage)); CHECK_FALSE(service.isLive(b));
}

}  // TEST_SUITE

// Keep MSVC from expanding the generation-exhaustion loop into the test body.
DOCTEST_NOINLINE bool recycleTexture(TextureService& service, const TextureKey& key) {
    return service.release(service.acquire(key));
}

TEST_CASE("TextureService: adopt retain dimensions and generation exhaustion") {
    TextureService s;
    const auto h = s.acquire(keyOf("base", "ui.tga", 1), kNoTextureNode, {256, 128, 64, 32});
    {
        auto a = ServiceTextureRef::adopt(s, h);
        { auto b = ServiceTextureRef::retain(s, h); CHECK(s.users(h) == 2); }
        CHECK(s.users(h) == 1);
        CHECK(s.dimensions(h).logicalHeight == 32);
    }
    CHECK_FALSE(s.isLive(h));
    CHECK_FALSE(s.retain(h));
    CHECK(s.dimensions(h).logicalWidth == 16);
    for (int i = 0; i < 4095; ++i) {
        REQUIRE(recycleTexture(s,keyOf("base", "ui.tga", 1)));
    }
    auto fresh = s.acquire(keyOf("base", "ui.tga", 1));
    CHECK_FALSE(s.isLive(h));
    CHECK(fresh.index() != h.index());
}

TEST_CASE("TextureService: provider precedence and cooked cache versions") {
    CHECK_FALSE(preferCookedSource(true, 0, 1000000, true)); // mod TGA beats base KTX
    CHECK(preferCookedSource(false, 1000000, 1000001, false)); // FST KTX only
    CHECK(preferCookedSource(true, 3, 0, false)); // active mod KTX beats dependency source
    CHECK_FALSE(preferCookedSource(true, 0, 1, true)); // dependency KTX cannot outrank active TGA
    TextureService s;
    auto key = keyOf("mod:Exodus", "skin.ktx2", 5);
    auto img = std::make_shared<KtxImage>(); img->width=8;
    s.cacheCooked(key,img); CHECK(s.cooked(key)->width==8);
    ++key.cookVersion; CHECK_FALSE(bool(s.cooked(key)));
    key=keyOf("base", "skin.ktx2",5); CHECK_FALSE(bool(s.cooked(key)));
    key=keyOf("mod:Exodus", "skin.ktx2",6); CHECK_FALSE(bool(s.cooked(key)));
}

TEST_CASE("TextureService: last reference during flight and descriptor reuse") {
    TextureService service;
    TextureDescriptorTable table(2);
    int destroyed=0;
    auto make=[&]{return std::shared_ptr<void>(new int(1),[&](void* p){delete static_cast<int*>(p);++destroyed;});};
    const auto old=service.acquire(keyOf("base","a",1));
    auto owner=make(); CHECK_FALSE(table.full());
    const auto slot=table.insert(old,owner); REQUIRE(slot==1); CHECK(table.full());
    auto inFlight=table.snapshot(); owner.reset();
    CHECK(table.insert(service.acquire(keyOf("base","b",2)),make())==0); // capacity exhaustion
    CHECK(service.release(old)); CHECK(table.erase(old));
    CHECK(destroyed==1); // only the rejected allocation, never the submitted image
    auto next=service.acquire(keyOf("base","c",3));
    CHECK(table.full());
    table.beginFrame(3);
    CHECK(table.insert(next,make())==slot);
    CHECK(table.index(old)==0); CHECK_FALSE(table.erase(old));
    auto nextFlight=table.snapshot();
    CHECK(inFlight[slot].handle==old); CHECK(nextFlight[slot].handle==next);
    CHECK_FALSE(service.retain(old)); // stale generation cannot name the replacement
    inFlight.clear(); CHECK(destroyed==2);
    CHECK(table.erase(next)); nextFlight.clear(); CHECK(destroyed==3);
}

TEST_CASE("TextureService: descriptor quarantine generation and journal replay") {
    TextureService service;
    TextureDescriptorTable table(2);
    const auto a=service.acquire(keyOf("unit","quarantine-a",1));
    const auto b=service.acquire(keyOf("unit","quarantine-b",1));
    auto owner=std::make_shared<int>(1);
    table.beginFrame(10);
    REQUIRE(table.insert(a,owner)==1);
    const auto old=table.token(a);
    uint64_t applied[3]={table.revision(),table.revision(),table.revision()};
    REQUIRE(table.erase(a)); CHECK(table.resolve(old)==0);
    for (uint64_t frame=11;frame<=13;++frame) {
        bool full=false;
        const auto changes=table.changesSince(applied[frame%3],full);
        REQUIRE_FALSE(full); REQUIRE(changes.size()==1); CHECK(changes[0].slot==1);
        CHECK_FALSE(table.slot(1).owner); applied[frame%3]=table.revision();
        if (frame<13) {
            table.beginFrame(frame); CHECK(table.insert(b,owner)==0); CHECK(table.full());
        }
    }
    table.beginFrame(13); REQUIRE(table.insert(b,owner)==1);
    CHECK(table.token(b).gen!=old.gen); CHECK(table.resolve(old)==0);
    CHECK(table.resolve(table.token(b))==1);
    const auto live=table.token(b);
    table.invalidate(b); CHECK(table.token(b).gen==live.gen); CHECK_FALSE(table.slot(1).owner);
    CHECK(table.insert(b,owner)==1); CHECK(table.resolve(live)==0); CHECK(table.slot(1).owner==owner);
    bool full=false;
    CHECK(table.changesSince(table.revision(),full).empty()); CHECK_FALSE(full);
    table.changed(1); table.changed(1); // bounded overflow, as at mission load
    CHECK(table.changesSince(0,full).empty()); CHECK(full);
}

TEST_CASE("TextureService: exhausted descriptor generations never wrap") {
    TextureDescriptorTable table(2,1);
    const auto handle=TextureHandle::make(1,1);
    const auto owner=std::make_shared<int>(1);
    REQUIRE(table.insert(handle,owner)==1);
    const auto ancient=table.token(handle);
    for (uint64_t frame=1;frame<UINT16_MAX;++frame) {
        REQUIRE(table.erase(handle)); table.beginFrame(frame);
        REQUIRE(table.insert(handle,owner)==1); CHECK(table.resolve(ancient)==0);
    }
    REQUIRE(table.erase(handle)); table.beginFrame(UINT16_MAX);
    CHECK(table.insert(handle,owner)==0); CHECK(table.full()); CHECK(table.resolve(ancient)==0);
}

TEST_CASE("TextureService: delayed streaming staging cancellation and reload") {
    TextureService service;
    TextureDescriptorTable table(3);
    TextureStagingBudget budget{16};
    mc2vk::CompletionTracker tracker;
    const auto old=service.acquire(keyOf("base","stream",1));
    const mc2vk::GpuTicket ticket{tracker.noteSubmit(mc2vk::QueueKind::Graphics),0};
    REQUIRE(budget.reserve(16));
    CHECK_FALSE(budget.reserve(1)); // exhausted staging defers work, never waits
    CHECK(table.index(old)==0); // delayed uploads stay visibly pending
    REQUIRE(service.release(old));
    const auto fresh=service.acquire(keyOf("base","stream",1));
    CHECK(old!=fresh);
    CHECK_FALSE(tracker.isComplete(ticket));
    tracker.setCompleted(mc2vk::QueueKind::Graphics,ticket.gfx);
    REQUIRE(tracker.isComplete(ticket)); budget.complete(16);
    CHECK_FALSE(service.isLive(old)); // cancelled result cannot attach to the reused slot
    REQUIRE(budget.reserve(16));
    CHECK(table.insert(fresh,std::make_shared<int>(1))==1); // eventual art after completion
    CHECK(table.index(old)==0);
    budget.complete(16); CHECK(budget.used==0);
}

namespace {
struct PublicationFixture {
    TextureService service;
    TextureDescriptorTable table{2}; // placeholder plus one usable slot
    TextureStagingBudget budget{32};
    mc2vk::CompletionTracker tracker;
    struct Resident { std::shared_ptr<int> image; uint32_t descriptor=0, contentVersion=0; };
    struct Upload { TextureHandle handle; std::shared_ptr<int> image; mc2vk::GpuTicket ticket; size_t bytes; uint32_t contentVersion; };
    std::unordered_map<uint32_t,Resident> residents;
    std::deque<Upload> uploads;
    bool queue(TextureHandle h) {
        const auto slot=table.reserve(h);
        if (!slot || !budget.reserve(4)) return false;
        const auto version=service.contentVersion(h);
        residents[h.raw()].descriptor=slot;
        residents[h.raw()].contentVersion=version;
        uploads.push_back({h,std::make_shared<int>(1),{tracker.noteSubmit(mc2vk::QueueKind::Graphics),0},4,version});
        return true;
    }
    void pump() {
        publishTextureUploads(service,table,uploads,residents,budget,
                              [this](mc2vk::GpuTicket ticket) { return tracker.isComplete(ticket); });
    }
    void complete() { tracker.setCompleted(mc2vk::QueueKind::Graphics,uploads.back().ticket.gfx); }
};
}

TEST_CASE("TextureService: in-flight uploads reserve descriptor capacity") {
    PublicationFixture f;
    const auto a=f.service.acquire(keyOf("unit","a",1));
    const auto b=f.service.acquire(keyOf("unit","b",2));
    REQUIRE(f.queue(a));
    const auto pending=f.table.token(a);
    const auto revision=f.table.revision();
    REQUIRE(pending.slot==1); CHECK_FALSE(f.table.slot(1).owner);
    CHECK(f.table.reserve(a)==1); CHECK(f.table.revision()==revision);
    CHECK_FALSE(f.queue(b)); // the uncompleted A already claims the only slot
    f.pump(); CHECK(f.table.resolve(pending)==1); CHECK(f.uploads.size()==1);
    f.complete(); f.pump();
    REQUIRE(f.table.index(a)==1); REQUIRE(bool(f.residents[a.raw()].image));
    CHECK(f.table.token(a).gen!=pending.gen); CHECK(f.table.resolve(pending)==0);
    bool full=false;
    const auto changes=f.table.changesSince(revision,full);
    REQUIRE(changes.size()==1); CHECK(changes[0].slot==pending.slot); CHECK_FALSE(full);
    CHECK_FALSE(f.queue(b));
    REQUIRE(f.service.release(a)); REQUIRE(f.table.erase(a));
    CHECK_FALSE(f.queue(b));
    f.table.beginFrame(3);
    REQUIRE(f.queue(b));
    f.pump(); CHECK(f.table.index(b)==1); CHECK_FALSE(f.table.slot(1).owner);
    f.complete(); f.pump();
    CHECK(bool(f.residents[b.raw()].image)); CHECK(f.table.index(b)==1);
    CHECK(f.service.isLive(b)); CHECK(f.budget.used==0);
}

TEST_CASE("TextureService: batched uploads share a completion ticket") {
    PublicationFixture f;
    f.table = TextureDescriptorTable(3);
    const auto a=f.service.acquire(keyOf("unit","batch-a",1));
    const auto b=f.service.acquire(keyOf("unit","batch-b",2));
    REQUIRE(f.queue(a));
    REQUIRE(f.queue(b));
    const auto ticket=f.uploads.back().ticket;
    for (auto& upload : f.uploads) upload.ticket=ticket;
    f.tracker.setCompleted(mc2vk::QueueKind::Graphics,ticket.gfx-1);
    f.pump();
    CHECK(f.table.index(a)==1); CHECK_FALSE(f.table.slot(1).owner);
    CHECK(f.table.index(b)==2); CHECK_FALSE(f.table.slot(2).owner);
    CHECK(f.budget.used==8);
    CHECK(f.uploads.size()==2);
    f.tracker.setCompleted(mc2vk::QueueKind::Graphics,ticket.gfx);
    f.pump();
    CHECK(f.table.index(a)!=0);
    CHECK(f.table.index(b)!=0);
    CHECK(f.budget.used==0);
    CHECK(f.uploads.empty());
}

TEST_CASE("TextureService: reserved publication cannot be displaced by another publisher") {
    PublicationFixture f;
    const auto waiting=f.service.acquire(keyOf("unit","waiting",1));
    const auto occupant=f.service.acquire(keyOf("unit","occupant",2));
    REQUIRE(f.queue(waiting)); f.complete();
    REQUIRE(f.table.insert(occupant,std::make_shared<int>(2))==0);
    f.pump();
    CHECK(f.uploads.empty()); CHECK(f.budget.used==0);
    CHECK(f.service.isLive(waiting)); CHECK(f.table.index(waiting)==1);
    CHECK(bool(f.residents[waiting.raw()].image));
}

TEST_CASE("TextureService: cancelled completed upload releases its reservation") {
    PublicationFixture f;
    const auto h=f.service.acquire(keyOf("unit","cancelled",1));
    REQUIRE(f.queue(h)); REQUIRE(f.service.release(h));
    REQUIRE(f.table.erase(h)); // residency retirement pump precedes publication
    f.pump(); CHECK(f.budget.used==4); // GPU still owns staging
    f.complete(); f.pump();
    CHECK(f.uploads.empty()); CHECK(f.budget.used==0);
    CHECK(f.table.index(h)==0); CHECK_FALSE(bool(f.residents[h.raw()].image));
    CHECK(f.table.full()); f.table.beginFrame(3); CHECK_FALSE(f.table.full());
}

TEST_CASE("TextureService: UI ids retain every generation bit and signal node retirement") {
    const auto high=TextureHandle::make(0xfffffu,0xfffu);
    CHECK(uiTextureHandle(uiTextureId(high))==high);
    CHECK_FALSE(isUiTextureId(42));
    CHECK_FALSE(uiTextureHandle(42).isValid());
    TextureService service;
    TextureHandle retired;
    service.onRetired=[&](TextureHandle h) { retired=h; };
    const auto original=service.acquire(keyOf("base","cursor",1),7,{256,128,64,32});
    REQUIRE(service.retain(original)); // copying then releasing the original keeps dimensions
    REQUIRE(service.release(original));
    CHECK(service.dimensions(original).logicalHeight==32);
    CHECK(service.users(original)==1);
    service.onNodeDestroyed(7);
    CHECK(retired==original);
    CHECK_FALSE(service.isLive(uiTextureHandle(uiTextureId(original))));
}

TEST_CASE("TextureService: owned decode worker and oversized staging admission") {
    TextureDecodeWorker worker;
    std::promise<bool> decoded;
    auto result=decoded.get_future();
    std::vector<uint8_t> bytes={1,2,3,255};
    worker.request(TextureHandle::make(1,1),[owned=std::move(bytes),&decoded] {
        auto image=std::make_shared<KtxImage>(); image->pixels=owned;
        decoded.set_value(owned==std::vector<uint8_t>{1,2,3,255});
        return image;
    });
    REQUIRE(result.get());
    worker.waitIdle();
    const auto published=worker.take();
    REQUIRE(published.size()==1);
    REQUIRE(bool(published.front().image));
    CHECK((published.front().image->pixels==std::vector<uint8_t>{1,2,3,255}));
    TextureStagingBudget budget{16};
    REQUIRE(budget.reserve(32)); // one large atlas can make progress
    CHECK_FALSE(budget.reserve(1));
    budget.complete(32); REQUIRE(budget.reserve(16)); budget.complete(16);
    CHECK(budget.used==0);
}

TEST_CASE("TextureService: reload keeps identity and rejects an older decoded result") {
    TextureService service;
    TextureDecodeWorker worker;
    const auto handle=service.acquire(keyOf("base","reload",1),7);
    std::promise<void> allow;
    auto gate=allow.get_future().share();
    worker.request(handle,[gate] { gate.wait(); return std::make_shared<const KtxImage>(); },service.contentVersion(handle));
    worker.request(TextureHandle::make(99,1),[] { return std::shared_ptr<const KtxImage>{}; });
    REQUIRE(service.rebind(handle,7));
    allow.set_value(); worker.waitIdle();
    auto results=worker.take();
    REQUIRE(results.size()==2); CHECK(results.front().handle==handle);
    CHECK(service.isLive(handle));
    CHECK(results.front().contentVersion!=service.contentVersion(handle));
}

TEST_CASE("TextureService: refused upload batches retain decoded requests and retry") {
    // Allocation, command-buffer end and queue-submit failures all return no ticket.
    for (const char* failure : {"allocate", "end", "submit"}) {
        CAPTURE(failure);
        PublicationFixture f;
        const auto older=f.service.acquire(keyOf("unit","older",1));
        REQUIRE(f.queue(older));
        const auto handle=f.service.acquire(keyOf("unit","retry",1));
        struct Pending { TextureHandle handle; std::shared_ptr<int> pixels; };
        auto decoded=std::make_shared<int>(42);
        std::deque<Pending> pending{{handle,decoded}}, batch;
        const size_t firstNew=f.uploads.size();
        REQUIRE(f.budget.reserve(4));
        const auto version=f.service.contentVersion(handle);
        f.residents[handle.raw()].contentVersion=version;
        f.uploads.push_back({handle,std::make_shared<int>(2),{},4,version});
        batch.push_back(std::move(pending.front())); pending.pop_front();
        finishTextureUploadBatch(f.uploads,firstNew,pending,batch,f.budget,mc2vk::GpuTicket{});
        REQUIRE(f.uploads.size()==firstNew);
        CHECK(f.budget.used==4); // Only the earlier submitted batch remains charged.
        REQUIRE(pending.size()==1);
        CHECK(pending.front().handle==handle);
        CHECK(pending.front().pixels.get()==decoded.get());
        CHECK_FALSE(bool(f.residents[handle.raw()].image));
        CHECK(f.table.index(handle)==0);
        f.complete(); f.pump();
        REQUIRE(f.service.release(older)); REQUIRE(f.table.erase(older));
        f.table.beginFrame(3);
        REQUIRE(f.budget.reserve(4));
        f.uploads.push_back({handle,std::make_shared<int>(42),{},4,version});
        batch.push_back(std::move(pending.front())); pending.pop_front();
        const mc2vk::GpuTicket ticket{f.tracker.noteSubmit(mc2vk::QueueKind::Graphics),0};
        finishTextureUploadBatch(f.uploads,0,pending,batch,f.budget,ticket);
        CHECK(batch.empty()); CHECK(pending.empty());
        f.pump(); CHECK_FALSE(bool(f.residents[handle.raw()].image));
        f.complete(); f.pump();
        REQUIRE(bool(f.residents[handle.raw()].image));
        CHECK(*f.residents[handle.raw()].image==42);
        CHECK(f.budget.used==0);
    }
}

TEST_CASE("TextureService: poisoned uploads cannot starve good requests and transient retries retain pixels") {
    PublicationFixture f;
    f.table=TextureDescriptorTable(10);
    struct Pending { TextureHandle handle; std::shared_ptr<KtxImage> image; uint32_t contentVersion=0; unsigned attempts=0; };
    std::deque<Pending> pending,batch;
    std::vector<TextureHandle> handles;
    auto pixels=std::make_shared<KtxImage>(); pixels->pixels.resize(4);
    for (int i=0;i<6;++i) {
        auto key=keyOf("unit","poison-queue",i+1);
        auto h=f.service.acquire(key); handles.push_back(h);
        const auto version=f.service.contentVersion(h);
        f.residents[h.raw()].contentVersion=version;
        pending.push_back({h,pixels,version});
    }
    unsigned poisonAttempts=0,transientAttempts=0,dropped=0;
    for (unsigned pump=0;pump<3;++pump) {
        const size_t firstNew=f.uploads.size();
        mc2vk::pumpPendingTextureUploads(pending,f.uploads,batch,f.budget,
            [](const Pending&) { return true; },[&] { return f.table.full(f.uploads.size()); },
            [&](const Pending& p, PublicationFixture::Upload& next) {
                CHECK(p.image.get()==pixels.get());
                if (p.handle==handles[0]) { ++poisonAttempts; return false; }
                if (p.handle==handles[1] && ++transientAttempts==1) return false;
                next.image=std::make_shared<int>(42); return true;
            },[&](const Pending& p) { CHECK(p.handle==handles[0]); CHECK(p.attempts==3); ++dropped; });
        if (f.uploads.size()>firstNew) {
            const mc2vk::GpuTicket ticket{f.tracker.noteSubmit(mc2vk::QueueKind::Graphics),0};
            finishTextureUploadBatch(f.uploads,firstNew,pending,batch,f.budget,ticket);
            f.complete(); f.pump();
        }
        CHECK(f.budget.used==0);
        // All four unrelated requests publish on the very first pump.
        for (size_t i=2;i<handles.size();++i) CHECK(f.table.index(handles[i])!=0);
    }
    CHECK(pending.empty()); CHECK(batch.empty());
    CHECK(poisonAttempts==3); CHECK(transientAttempts==2); CHECK(dropped==1);
    CHECK(f.table.index(handles[0])==0); CHECK(f.table.index(handles[1])!=0);
}

TEST_CASE("TextureService: dropped dynamic B preserves A and permits C to publish") {
    PublicationFixture f;
    f.table=TextureDescriptorTable(10);
    struct Ref {
        TextureHandle handle;
        Ref() : handle(TextureHandle::invalid()) {}
        explicit Ref(TextureHandle h) : handle(h) {}
        Ref(Ref&& other) noexcept : handle(other.handle) { other.handle=TextureHandle::invalid(); }
        Ref& operator=(Ref&& other) noexcept { handle=other.handle; other.handle=TextureHandle::invalid(); return *this; }
        explicit operator bool() const { return handle.isValid(); }
        TextureHandle get() const { return handle; }
    };
    struct Entry { Ref texture,pending; std::weak_ptr<KtxImage> source; } e;
    const auto a=f.service.acquire(keyOf("unit","dynamic-a",1));
    const auto b=f.service.acquire(keyOf("unit","dynamic-b",1));
    const auto c=f.service.acquire(keyOf("unit","dynamic-c",1));
    REQUIRE(f.queue(a)); f.complete(); f.pump(); e.texture=Ref(a);
    auto pixels=std::make_shared<KtxImage>(); pixels->pixels.resize(4);
    struct Pending { TextureHandle handle; std::shared_ptr<KtxImage> image; uint32_t contentVersion=0; unsigned attempts=0; };
    std::deque<Pending> pending{{b,pixels}},batch;
    f.residents[b.raw()]={}; e.pending=Ref(b); e.source=pixels;
    auto settle=[&] {
        mc2vk::settlePendingUiTexture(e,[&](auto h) { return f.service.isLive(h); },
            [&](auto h) { return f.residents.contains(h.raw()); },
            [&](auto h) { return bool(f.residents.at(h.raw()).image); });
    };
    for (int attempt=0;attempt<3;++attempt) {
        mc2vk::pumpPendingTextureUploads(pending,f.uploads,batch,f.budget,
            [](const Pending&) { return true; },[] { return false; },
            [](const Pending&, PublicationFixture::Upload&) { return false; },
            [&](const Pending& p) { f.residents.erase(p.handle.raw()); });
        settle(); CHECK(e.texture.get()==a); CHECK(f.table.index(a)!=0);
    }
    CHECK_FALSE(bool(e.pending)); CHECK(e.source.expired()); CHECK(f.service.isLive(b));
    REQUIRE(f.queue(c)); e.pending=Ref(c);
    settle(); CHECK(e.texture.get()==a);
    f.complete(); f.pump(); settle();
    CHECK(e.texture.get()==c); CHECK_FALSE(bool(e.pending));
}

TEST_CASE("legacy-cpu residency key text is unchanged by the copy-once refactor") {
    const uint8_t px[8] = {1, 2, 3, 4, 250, 251, 252, 253};
    const uint64_t hash = textureContentHash(px, sizeof(px));
    CHECK(legacyCpuTextureSource(64, 32, 8, true, hash) == makeTextureSourceId("legacy-cpu:P9", "64x32:8:array", hash));
    CHECK(legacyCpuTextureSource(1, 1, 1, false, hash) == makeTextureSourceId("legacy-cpu:P9", "1x1:1:2d", hash));
    CHECK(legacyCpuTextureSource(1, 1, 1, false, 0) == "legacy-cpu:P9|1x1:1:2d|0000000000000000");
}
