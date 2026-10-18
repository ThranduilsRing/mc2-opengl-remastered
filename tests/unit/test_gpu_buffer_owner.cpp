// tests/unit/test_gpu_buffer_owner.cpp
// GL-free unit tests for the GpuBufferOwner record
// and the 3 new RenderResourceIds. No GL context, no engine, pure value logic.
#include "doctest.h"
#include "GpuBufferOwner.h"
#include "RenderResourceRegistry.h"
#include <cstring>
#include <type_traits>

using namespace RenderCore;

TEST_SUITE("GpuBufferOwner") {

TEST_CASE("GpuBufferOwner default is invalid (zero handle, null name, Unset)") {
    GpuBufferOwner o{};
    CHECK((o.id == RenderResourceId::Unknown));
    CHECK((o.lifetime == RenderResourceLifetime::Unset));
    CHECK(o.debugName == nullptr);
    CHECK(o.backendHandle == 0u);
    CHECK_FALSE(o.valid());
}

TEST_CASE("GpuBufferOwner valid() requires a non-zero handle AND a non-null name") {
    GpuBufferOwner o{};
    o.id        = RenderResourceId::ViewUniformsUbo;
    o.lifetime  = RenderResourceLifetime::Persistent;
    o.debugName = "ViewUniformsUbo";
    o.backendHandle    = 7u;
    CHECK(o.valid());

    // Zero handle -> invalid even with a name.
    GpuBufferOwner noHandle = o;
    noHandle.backendHandle = 0u;
    CHECK_FALSE(noHandle.valid());

    // Null name -> invalid even with a handle.
    GpuBufferOwner noName = o;
    noName.debugName = nullptr;
    CHECK_FALSE(noName.valid());
}

TEST_CASE("GpuBufferOwner id and lifetime round-trip the stored fields") {
    GpuBufferOwner o{};
    o.id        = RenderResourceId::TerrainCementSsbo;
    o.lifetime  = RenderResourceLifetime::Mission;
    o.debugName = "TerrainCementSsbo";
    o.backendHandle    = 4242u;

    CHECK((o.id == RenderResourceId::TerrainCementSsbo));
    CHECK((o.lifetime == RenderResourceLifetime::Mission));
    CHECK(o.backendHandle == 4242u);
    CHECK(std::strcmp(o.debugName, "TerrainCementSsbo") == 0);
    CHECK(o.valid());
}

// The LOD-chunk height SSBO (id 14, already-existing, already-registered) is
// narrowed behind a GpuBufferOwner like type/cement.
TEST_CASE("TERRAIN-HEIGHT-SSBO-OWNER-1 height owner round-trips id/lifetime/name and valid->invalid") {
    GpuBufferOwner o{
        RenderResourceId::TerrainHeightSsbo,
        RenderResourceLifetime::Mission,
        "TerrainHeightSsbo",
        0u};
    // Newly-constructed (backendHandle 0) -> not yet allocated -> invalid.
    CHECK_FALSE(o.valid());

    // After glGen stores a handle -> valid, fields preserved.
    o.backendHandle = 14u;
    CHECK((o.id == RenderResourceId::TerrainHeightSsbo));
    CHECK((o.lifetime == RenderResourceLifetime::Mission));
    CHECK(std::strcmp(o.debugName, "TerrainHeightSsbo") == 0);
    CHECK(o.valid());

    // Invalidate-on-destroy (backendHandle cleared) -> invalid again.
    o.backendHandle = 0u;
    CHECK_FALSE(o.valid());

    CHECK(std::strcmp(toString(RenderResourceId::TerrainHeightSsbo), "TerrainHeightSsbo") == 0);
    CHECK(int(RenderResourceId::TerrainHeightSsbo) < int(RenderResourceId::Count));
}

TEST_CASE("MECH-MATERIAL-SSBO-OWNER-1 material owner round-trips id/lifetime/name and valid->invalid") {
    GpuBufferOwner o{
        RenderResourceId::MaterialGpuBuffer,
        RenderResourceLifetime::Persistent,
        "MaterialGpuBuffer",
        0u};
    // Newly-constructed (backendHandle 0) -> not yet allocated -> invalid.
    CHECK_FALSE(o.valid());

    // After glGen stores a handle -> valid, fields preserved.
    o.backendHandle = 22u;
    CHECK((o.id == RenderResourceId::MaterialGpuBuffer));
    CHECK((o.lifetime == RenderResourceLifetime::Persistent));
    CHECK(std::strcmp(o.debugName, "MaterialGpuBuffer") == 0);
    CHECK(o.valid());

    // Invalidate-on-destroy (backendHandle cleared) -> invalid again.
    o.backendHandle = 0u;
    CHECK_FALSE(o.valid());

    CHECK(std::strcmp(toString(RenderResourceId::MaterialGpuBuffer), "MaterialGpuBuffer") == 0);
    CHECK(int(RenderResourceId::MaterialGpuBuffer) < int(RenderResourceId::Count));
}

TEST_CASE("STATICPROP-MATERIAL-SSBO-OWNER-1 static-prop material owner round-trips id/lifetime/name and valid->invalid") {
    GpuBufferOwner o{
        RenderResourceId::StaticPropMaterialGpuBuffer,
        RenderResourceLifetime::Mission,
        "StaticPropMaterialGpuBuffer",
        0u};
    // Newly-constructed (backendHandle 0) -> not yet allocated -> invalid.
    CHECK_FALSE(o.valid());

    // After glGen stores a handle -> valid, fields preserved.
    o.backendHandle = 28u;
    CHECK((o.id == RenderResourceId::StaticPropMaterialGpuBuffer));
    CHECK((o.lifetime == RenderResourceLifetime::Mission));
    CHECK(std::strcmp(o.debugName, "StaticPropMaterialGpuBuffer") == 0);
    CHECK(o.valid());

    // Invalidate-on-destroy (backendHandle cleared) -> invalid again.
    o.backendHandle = 0u;
    CHECK_FALSE(o.valid());

    CHECK(std::strcmp(toString(RenderResourceId::StaticPropMaterialGpuBuffer), "StaticPropMaterialGpuBuffer") == 0);
    CHECK(int(RenderResourceId::StaticPropMaterialGpuBuffer) < int(RenderResourceId::Count));
    // Distinct id from the mech-material table (binding 2) — separate buffer/consumer.
    CHECK((RenderResourceId::StaticPropMaterialGpuBuffer != RenderResourceId::MaterialGpuBuffer));
}

TEST_CASE("MECH-PROFILE-SSBO-OWNER-1 mech-profile material owner round-trips id/lifetime/name and valid->invalid") {
    GpuBufferOwner o{
        RenderResourceId::MechProfileMaterialGpuBuffer,
        RenderResourceLifetime::Persistent,
        "MechProfileMaterialGpuBuffer",
        0u};
    // Newly-constructed (backendHandle 0) -> not yet allocated -> invalid.
    // Default run (MC2_MECH_SURFACE_MATERIAL unset) never creates the buffer.
    CHECK_FALSE(o.valid());

    // After glGen stores a handle -> valid, fields preserved.
    o.backendHandle = 29u;
    CHECK((o.id == RenderResourceId::MechProfileMaterialGpuBuffer));
    CHECK((o.lifetime == RenderResourceLifetime::Persistent));
    CHECK(std::strcmp(o.debugName, "MechProfileMaterialGpuBuffer") == 0);
    CHECK(o.valid());

    // Invalidate-on-destroy (backendHandle cleared) -> invalid again.
    o.backendHandle = 0u;
    CHECK_FALSE(o.valid());

    CHECK(std::strcmp(toString(RenderResourceId::MechProfileMaterialGpuBuffer), "MechProfileMaterialGpuBuffer") == 0);
    CHECK(int(RenderResourceId::MechProfileMaterialGpuBuffer) < int(RenderResourceId::Count));
    // Distinct id from the static-prop (binding 5) and mech-material (binding 2) tables.
    CHECK((RenderResourceId::MechProfileMaterialGpuBuffer != RenderResourceId::StaticPropMaterialGpuBuffer));
    CHECK((RenderResourceId::MechProfileMaterialGpuBuffer != RenderResourceId::MaterialGpuBuffer));
}

TEST_CASE("TERRAIN-VISUAL-HEIGHT-SSBO-OWNER-1 visual-height owner round-trips id/lifetime/name and valid->invalid") {
    GpuBufferOwner o{
        RenderResourceId::TerrainVisualHeightSsbo,
        RenderResourceLifetime::Mission,
        "TerrainVisualHeightSsbo",
        0u};
    // Newly-constructed (backendHandle 0) -> not yet allocated -> invalid. WIP feature: the
    // create path is gated upstream (MC2_TERRAIN_VISUAL_HEIGHT/_DISPLACE + a bake that
    // does NOT ship), so default smoke never creates the buffer — backendHandle stays 0.
    CHECK_FALSE(o.valid());

    // After glGen stores a handle -> valid, fields preserved.
    o.backendHandle = 30u;
    CHECK((o.id == RenderResourceId::TerrainVisualHeightSsbo));
    CHECK((o.lifetime == RenderResourceLifetime::Mission));
    CHECK(std::strcmp(o.debugName, "TerrainVisualHeightSsbo") == 0);
    CHECK(o.valid());

    // Invalidate-on-destroy (backendHandle cleared) -> invalid again.
    o.backendHandle = 0u;
    CHECK_FALSE(o.valid());

    CHECK(std::strcmp(toString(RenderResourceId::TerrainVisualHeightSsbo), "TerrainVisualHeightSsbo") == 0);
    CHECK(int(RenderResourceId::TerrainVisualHeightSsbo) < int(RenderResourceId::Count));
    // Distinct id from the 1x live height field and the type/cement siblings.
    CHECK((RenderResourceId::TerrainVisualHeightSsbo != RenderResourceId::TerrainHeightSsbo));
    CHECK((RenderResourceId::TerrainVisualHeightSsbo != RenderResourceId::TerrainTypeSsbo));
    CHECK((RenderResourceId::TerrainVisualHeightSsbo != RenderResourceId::TerrainCementSsbo));
}

TEST_CASE("TERRAIN-LIGHTING-SSBO-OWNER-1 the 3 compute SSBO owners round-trip id/lifetime/name and valid->invalid") {
    struct Case { RenderResourceId id; const char* name; uint32_t handle; };
    const Case cases[] = {
        { RenderResourceId::TerrainLightVertexInputSsbo,   "TerrainLightVertexInputSsbo",   31u },
        { RenderResourceId::TerrainLightInputSsbo,         "TerrainLightInputSsbo",         32u },
        { RenderResourceId::TerrainLightComputeOutputSsbo, "TerrainLightComputeOutputSsbo", 33u },
    };
    for (const auto& c : cases) {
        GpuBufferOwner o{ c.id, RenderResourceLifetime::Mission, c.name, 0u };
        // Newly-constructed (backendHandle 0) -> not yet allocated -> invalid.
        CHECK_FALSE(o.valid());
        // After glGen stores a handle -> valid, fields preserved.
        o.backendHandle = c.handle;
        CHECK((o.id == c.id));
        CHECK((o.lifetime == RenderResourceLifetime::Mission));
        CHECK(std::strcmp(o.debugName, c.name) == 0);
        CHECK(o.valid());
        // Invalidate-on-destroy (backendHandle cleared) -> invalid again.
        o.backendHandle = 0u;
        CHECK_FALSE(o.valid());
        // Registered id + matching toString label, strictly below Count.
        CHECK(std::strcmp(toString(c.id), c.name) == 0);
        CHECK(int(c.id) < int(RenderResourceId::Count));
    }
    // The 3 lighting ids are distinct from each other and from the LOD-chunk terrain SSBOs.
    CHECK((RenderResourceId::TerrainLightVertexInputSsbo   != RenderResourceId::TerrainLightInputSsbo));
    CHECK((RenderResourceId::TerrainLightVertexInputSsbo   != RenderResourceId::TerrainLightComputeOutputSsbo));
    CHECK((RenderResourceId::TerrainLightInputSsbo         != RenderResourceId::TerrainLightComputeOutputSsbo));
    CHECK((RenderResourceId::TerrainLightComputeOutputSsbo != RenderResourceId::TerrainHeightSsbo));
    CHECK((RenderResourceId::TerrainLightComputeOutputSsbo != RenderResourceId::TerrainCementSsbo));
}

TEST_CASE("LIGHTDATA-SSBO-OWNER-1 light-data owner round-trips id/lifetime/name and valid->invalid (Persistent, grow-once)") {
    // Persistent lifetime: lazy-created on first upload, destroyed at renderer teardown.
    GpuBufferOwner o{ RenderResourceId::LightDataSsbo, RenderResourceLifetime::Persistent,
                      "LightDataSsbo", 0u };
    CHECK_FALSE(o.valid());                       // backendHandle 0 -> unallocated
    o.backendHandle = 34u;                               // glGen stores a handle
    CHECK((o.id == RenderResourceId::LightDataSsbo));
    CHECK((o.lifetime == RenderResourceLifetime::Persistent));
    CHECK(std::strcmp(o.debugName, "LightDataSsbo") == 0);
    CHECK(o.valid());
    // Grow-once: handle recreated -> backendHandle updated, still valid, id/name preserved.
    o.backendHandle = 77u;
    CHECK(o.valid());
    CHECK((o.id == RenderResourceId::LightDataSsbo));
    // Invalidate-on-destroy.
    o.backendHandle = 0u;
    CHECK_FALSE(o.valid());
    // Registered id + matching toString label, strictly below Count, distinct.
    CHECK(std::strcmp(toString(RenderResourceId::LightDataSsbo), "LightDataSsbo") == 0);
    CHECK(int(RenderResourceId::LightDataSsbo) < int(RenderResourceId::Count));
    CHECK((RenderResourceId::LightDataSsbo != RenderResourceId::TerrainLightComputeOutputSsbo));
    CHECK((RenderResourceId::LightDataSsbo != RenderResourceId::TerrainHeightSsbo));
}

TEST_CASE("SCENE-SSBO-OWNER-SWEEP-1 dynamic prop-shadow owner round-trips id/lifetime/name and valid->invalid (Mission, per-frame orphan)") {
    // Mission lifetime: handle glGen'd once per map, freed on onMapUnload. Per-frame
    // orphan-on-write bufferData refreshes size WITHOUT a new handle.
    GpuBufferOwner o{ RenderResourceId::DynamicPropShadowSsbo, RenderResourceLifetime::Mission,
                      "DynamicPropShadowSsbo", 0u };
    CHECK_FALSE(o.valid());                       // backendHandle 0 -> unallocated (no shadow set yet)
    o.backendHandle = 36u;                               // glGenBuffers stores a handle
    CHECK((o.id == RenderResourceId::DynamicPropShadowSsbo));
    CHECK((o.lifetime == RenderResourceLifetime::Mission));
    CHECK(std::strcmp(o.debugName, "DynamicPropShadowSsbo") == 0);
    CHECK(o.valid());
    // Per-frame re-register keeps the SAME handle -> still valid, id/name preserved.
    CHECK(o.valid());
    CHECK((o.id == RenderResourceId::DynamicPropShadowSsbo));
    // Invalidate-on-destroy (onMapUnload).
    o.backendHandle = 0u;
    CHECK_FALSE(o.valid());
    // Registered id + matching toString label, strictly below Count, distinct.
    CHECK(std::strcmp(toString(RenderResourceId::DynamicPropShadowSsbo), "DynamicPropShadowSsbo") == 0);
    CHECK(int(RenderResourceId::DynamicPropShadowSsbo) < int(RenderResourceId::Count));
    CHECK((RenderResourceId::DynamicPropShadowSsbo != RenderResourceId::LightDataSsbo));
    CHECK((RenderResourceId::DynamicPropShadowSsbo != RenderResourceId::StaticPropMaterialGpuBuffer));
}

TEST_CASE("VK-SHADOW-RING-OWNER-1 the 4 shadow ring SSBO owners round-trip id/lifetime/name and valid->invalid (Persistent, realloc-on-grow)") {
    struct Case { RenderResourceId id; const char* name; uint32_t handle; };
    const Case cases[] = {
        { RenderResourceId::MechInstanceRingSsbo,       "MechInstanceRingSsbo",       39u },
        { RenderResourceId::MechBoneRingSsbo,           "MechBoneRingSsbo",           40u },
        { RenderResourceId::StaticPropInstanceRingSsbo, "StaticPropInstanceRingSsbo", 41u },
        { RenderResourceId::StaticPropColorRingSsbo,    "StaticPropColorRingSsbo",    42u },
    };
    for (const auto& c : cases) {
        // Persistent lifetime: persistent-mapped ring, realloc-on-grow, freed on unload.
        GpuBufferOwner o{ c.id, RenderResourceLifetime::Persistent, c.name, 0u };
        CHECK_FALSE(o.valid());                 // backendHandle 0 -> unallocated
        o.backendHandle = c.handle;             // glGenBuffers stores a handle
        CHECK((o.id == c.id));
        CHECK((o.lifetime == RenderResourceLifetime::Persistent));
        CHECK(std::strcmp(o.debugName, c.name) == 0);
        CHECK(o.valid());
        // realloc-on-grow: handle recreated -> still valid, id/name preserved.
        o.backendHandle = c.handle + 100u;
        CHECK(o.valid());
        CHECK((o.id == c.id));
        // Invalidate-on-destroy (grow-realloc / onMapUnload).
        o.backendHandle = 0u;
        CHECK_FALSE(o.valid());
        CHECK(std::strcmp(toString(c.id), c.name) == 0);
        CHECK(int(c.id) < int(RenderResourceId::Count));
    }
    // The 4 ring ids are distinct from each other and from the material tables.
    CHECK((RenderResourceId::MechInstanceRingSsbo       != RenderResourceId::MechBoneRingSsbo));
    CHECK((RenderResourceId::StaticPropInstanceRingSsbo != RenderResourceId::StaticPropColorRingSsbo));
    CHECK((RenderResourceId::MechInstanceRingSsbo       != RenderResourceId::StaticPropInstanceRingSsbo));
    CHECK((RenderResourceId::MechInstanceRingSsbo       != RenderResourceId::MaterialGpuBuffer));
    CHECK((RenderResourceId::StaticPropInstanceRingSsbo != RenderResourceId::StaticPropMaterialGpuBuffer));
    CHECK((RenderResourceId::StaticPropColorRingSsbo    != RenderResourceId::DynamicPropShadowSsbo));
}

TEST_CASE("GpuBufferOwner is a trivially-copyable POD") {
    CHECK(std::is_trivially_copyable<GpuBufferOwner>::value);
}

// The 3 new ids round-trip through toString and sit strictly below Count.
TEST_CASE("GPU-BUFFER-OWNER-SKELETON-1 new ids toString and ordering") {
    CHECK(std::strcmp(toString(RenderResourceId::ViewUniformsUbo),   "ViewUniformsUbo")   == 0);
    CHECK(std::strcmp(toString(RenderResourceId::TerrainTypeSsbo),   "TerrainTypeSsbo")   == 0);
    CHECK(std::strcmp(toString(RenderResourceId::TerrainCementSsbo), "TerrainCementSsbo") == 0);

    CHECK(int(RenderResourceId::ViewUniformsUbo)   < int(RenderResourceId::Count));
    CHECK(int(RenderResourceId::TerrainTypeSsbo)   < int(RenderResourceId::Count));
    CHECK(int(RenderResourceId::TerrainCementSsbo) < int(RenderResourceId::Count));
}

} // TEST_SUITE("GpuBufferOwner")

// Offline guard that the 4 GPU-owner
// RenderResourceIds (view-UBO / terrain type / cement / height), when
// registered as valid Buffer resources, carry a concrete (non-Unset) lifetime
// and pass the registry lifetime validator. This locks the owner registrations
// against a future registration that defaults the lifetime to Unset.
// GL-free, offline; uses only the existing registry API.
TEST_SUITE("GpuBufferOwnerLifetimeCheck") {

namespace {
// The 4 owner ids paired with their registered lifetime + debugName literal.
struct OwnerExpectation {
    RenderCore::RenderResourceId     id;
    RenderCore::RenderResourceLifetime lifetime;
    const char*                      name;
};
const OwnerExpectation kOwners[] = {
    { RenderCore::RenderResourceId::ViewUniformsUbo,   RenderCore::RenderResourceLifetime::Persistent, "ViewUniformsUbo"   },
    { RenderCore::RenderResourceId::TerrainTypeSsbo,   RenderCore::RenderResourceLifetime::Mission,     "TerrainTypeSsbo"   },
    { RenderCore::RenderResourceId::TerrainCementSsbo, RenderCore::RenderResourceLifetime::Mission,     "TerrainCementSsbo" },
    { RenderCore::RenderResourceId::TerrainHeightSsbo, RenderCore::RenderResourceLifetime::Mission,     "TerrainHeightSsbo" },
};
} // namespace

TEST_CASE("the 4 GPU-owner ids register as Buffer resources with non-Unset lifetimes that pass the validator") {
    REQUIRE(getRenderResourceCount() == 0);  // clean registry from prior tests

    for (const auto& e : kOwners) {
        RenderResourceDesc d{};
        d.id        = e.id;
        d.kind      = RenderResourceKind::Buffer;
        d.format    = RenderResourceFormat::BufferRaw;
        d.lifetime  = e.lifetime;
        d.debugName = e.name;
        d.glName    = 100u + uint32_t(e.id);  // nonzero handle
        d.sizeBytes = 4096u;
        d.valid     = true;
        registerOrUpdateRenderResource(d);
    }
    REQUIRE(getRenderResourceCount() == 4);

    // Every valid resource carries a lifetime -> validator clean.
    RenderResourceId off = RenderResourceId::MainColor;  // poison
    CHECK(validateRenderResourceLifetimes(&off));
    CHECK((off == RenderResourceId::Unknown));

    // Per-owner contract: Buffer kind, non-Unset lifetime, nonzero handle,
    // and toString(id) matches the debugName literal.
    for (const auto& e : kOwners) {
        const RenderResourceDesc* got = getRenderResource(e.id);
        REQUIRE(got != nullptr);
        CHECK((got->kind == RenderResourceKind::Buffer));
        CHECK((got->lifetime != RenderResourceLifetime::Unset));
        CHECK((got->lifetime == e.lifetime));
        CHECK(got->glName != 0u);
        CHECK(got->valid);
        CHECK(std::strcmp(toString(e.id), e.name) == 0);
    }

    // Clean up so sibling suites see an empty registry.
    for (const auto& e : kOwners) {
        RenderResourceDesc inv{};
        inv.id = e.id; inv.valid = false;
        registerOrUpdateRenderResource(inv);
    }
    CHECK(getRenderResourceCount() == 0);
}

// Guards the failure direction: an owner id registered with a DEFAULTED
// (Unset) lifetime must be caught by the validator and named. This is exactly
// the future regression this guard exists to block.
TEST_CASE("a GPU-owner id registered with a defaulted Unset lifetime is detected by the validator") {
    REQUIRE(getRenderResourceCount() == 0);

    RenderResourceDesc bad{};
    bad.id        = RenderResourceId::TerrainHeightSsbo;
    bad.kind      = RenderResourceKind::Buffer;
    bad.debugName = "TerrainHeightSsbo";
    bad.glName    = 314u;
    bad.valid     = true;
    // bad.lifetime deliberately left at the struct default (Unset).
    REQUIRE((bad.lifetime == RenderResourceLifetime::Unset));
    registerOrUpdateRenderResource(bad);

    RenderResourceId off = RenderResourceId::Unknown;
    CHECK_FALSE(validateRenderResourceLifetimes(&off));
    CHECK((off == RenderResourceId::TerrainHeightSsbo));

    // Clean up.
    RenderResourceDesc inv{};
    inv.id = RenderResourceId::TerrainHeightSsbo; inv.valid = false;
    registerOrUpdateRenderResource(inv);
    CHECK(getRenderResourceCount() == 0);
}

} // TEST_SUITE("GpuBufferOwnerLifetimeCheck")
