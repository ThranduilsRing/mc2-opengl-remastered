// tests/unit/test_texture_service.cpp
// P4a texture and material identity (RenderCore/TextureService.h, MaterialRegistry.h): key dedupe, mod-precedence keys, paint variants, handles that
// survive a content change, stale handles after the node dies, material and material-set dedupe. No engine, no GPU.
#include "doctest.h"
#include "MaterialRegistry.h"
#include "TextureService.h"

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

}  // TEST_SUITE
