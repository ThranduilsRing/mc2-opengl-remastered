// tests/unit/test_texture_service.cpp
// P4a texture and material identity (RenderCore/TextureService.h, MaterialRegistry.h): key dedupe, mod-precedence keys, paint variants, handles that
// survive a content change, stale handles after the node dies, material and material-set dedupe. No engine, no GPU.
#include "doctest.h"
#include "MaterialRegistry.h"
#include "TextureService.h"
#include "TextureStream.h"
#include <future>
#include "../../GameOS/gameos/vk_completion_tracker.h"

using namespace RenderCore;

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
    CHECK(table.insert(next,make())==slot);
    CHECK(table.index(old)==0); CHECK_FALSE(table.erase(old));
    auto nextFlight=table.snapshot();
    CHECK(inFlight[slot].handle==old); CHECK(nextFlight[slot].handle==next);
    CHECK_FALSE(service.retain(old)); // stale generation cannot name the replacement
    inFlight.clear(); CHECK(destroyed==2);
    CHECK(table.erase(next)); nextFlight.clear(); CHECK(destroyed==3);
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
    std::promise<void> allow,drained;
    auto gate=allow.get_future().share(); auto done=drained.get_future();
    worker.request(handle,[gate] { gate.wait(); return std::make_shared<const KtxImage>(); },service.contentVersion(handle));
    worker.request(TextureHandle::make(99,1),[&drained] { drained.set_value(); return std::shared_ptr<const KtxImage>{}; });
    REQUIRE(service.rebind(handle,7));
    allow.set_value(); done.get(); // the single worker published the first result before starting the second
    auto results=worker.take();
    REQUIRE_FALSE(results.empty()); CHECK(results.front().handle==handle);
    CHECK(service.isLive(handle));
    CHECK(results.front().contentVersion!=service.contentVersion(handle));
}
