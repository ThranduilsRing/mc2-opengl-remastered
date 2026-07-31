// tests/unit/test_rendercore.cpp
// GL-free unit tests for RenderCore substrate invariants.
// No game startup, no GL context, no asset loading required.
#include "doctest.h"
#include "RenderResourceRegistry.h"
#include "frame_graph_validate.h"       // REGISTRY-LIFETIME-CLASS-1: kExternalResources drift guard
#include "RenderDebugView.h"
#include "RendererFeatureRegistry.h"
#include "IblShRegistry.h"
#include "PipelineDesc.h"               // PIPELINE-DESC-SCAFFOLD-1: target type
#include "render_contract_pipeline.h"   // PIPELINE-DESC-SCAFFOLD-1: adapter under test
#include "RenderPassContract.h"        // PIPELINE-DESC-REGISTERED-AUDIT-1: descriptive pass table
#include "PipelineRegistry.h"          // PIPELINE-DESC-REGISTERED-AUDIT-1: PipelineId coverage
#include "MechVisualState.h"           // GAMEADAPTERS-VISUAL-STATE-BRIDGE-1: bridge POD
#include <cstring>
#include <cmath>                       // NAN / INFINITY for sanitizer test
#include <type_traits>                 // is_trivially_copyable
#include <set>                         // REGISTRY-COMPUTE-IDS-1: unique-name check
#include <string>                      // REGISTRY-COMPUTE-IDS-1: unique-name check

using namespace RenderCore;

// ---------------------------------------------------------------------------
// RenderResourceRegistry
// ---------------------------------------------------------------------------
// NOTE: s_registry is process-global static. Tests below run in definition
// order and each TEST_CASE leaves the registry in a clean state (count == 0).

TEST_SUITE("RenderCore") {

TEST_CASE("RenderResourceRegistry initial count is zero") {
    CHECK(getRenderResourceCount() == 0);
}

TEST_CASE("RenderResourceRegistry register get invalidate") {
    RenderResourceDesc d{};
    d.id        = RenderResourceId::MainColor;
    d.kind      = RenderResourceKind::Texture2D;
    d.format    = RenderResourceFormat::RGBA8;
    d.debugName = "MainColor";
    d.width     = 1920;
    d.height    = 1080;
    d.valid     = true;
    registerOrUpdateRenderResource(d);

    CHECK(getRenderResourceCount() == 1);

    const RenderResourceDesc* got = getRenderResource(RenderResourceId::MainColor);
    REQUIRE(got != nullptr);
    CHECK(got->width == 1920);
    CHECK(got->height == 1080);
    CHECK((got->format == RenderResourceFormat::RGBA8));
    CHECK((got->kind == RenderResourceKind::Texture2D));

    // Mark invalid -- slot hidden from count and get.
    RenderResourceDesc inv{};
    inv.id    = RenderResourceId::MainColor;
    inv.valid = false;
    registerOrUpdateRenderResource(inv);

    CHECK(getRenderResourceCount() == 0);
    CHECK(getRenderResource(RenderResourceId::MainColor) == nullptr);
}

TEST_CASE("RenderResourceRegistry Unknown id is no-op") {
    size_t before = getRenderResourceCount();
    CHECK(before == 0);  // state invariant: prior tests must leave registry clean
    RenderResourceDesc d{};
    d.id    = RenderResourceId::Unknown;
    d.valid = true;
    registerOrUpdateRenderResource(d);
    CHECK(getRenderResourceCount() == 0);
    CHECK(getRenderResource(RenderResourceId::Unknown) == nullptr);
}

TEST_CASE("RenderResourceRegistry byIndex enumerates in slot order") {
    RenderResourceDesc d{};
    d.valid = true;

    d.id    = RenderResourceId::MainDepth;
    d.width = 200;
    registerOrUpdateRenderResource(d);

    d.id    = RenderResourceId::MainColor;
    d.width = 100;
    registerOrUpdateRenderResource(d);

    CHECK(getRenderResourceCount() == 2);

    // Dense index follows ascending slot order (MainColor=1 < MainDepth=2).
    const RenderResourceDesc* r0 = getRenderResourceByIndex(0);
    const RenderResourceDesc* r1 = getRenderResourceByIndex(1);
    const RenderResourceDesc* r2 = getRenderResourceByIndex(2);

    REQUIRE(r0 != nullptr);
    REQUIRE(r1 != nullptr);
    CHECK(r2 == nullptr);
    CHECK((r0->id == RenderResourceId::MainColor));
    CHECK((r1->id == RenderResourceId::MainDepth));

    // Clean up.
    d.valid = false;
    d.id = RenderResourceId::MainColor;
    registerOrUpdateRenderResource(d);
    d.id = RenderResourceId::MainDepth;
    registerOrUpdateRenderResource(d);
    CHECK(getRenderResourceCount() == 0);
}

TEST_CASE("RenderResourceRegistry toString") {
    CHECK(std::strcmp(toString(RenderResourceId::Unknown),   "Unknown")   == 0);
    CHECK(std::strcmp(toString(RenderResourceId::MainColor), "MainColor") == 0);
    CHECK(std::strcmp(toString(RenderResourceKind::Texture2D), "Texture2D") == 0);
    CHECK(std::strcmp(toString(RenderResourceKind::Buffer),    "Buffer")    == 0);
    CHECK(std::strcmp(toString(RenderResourceFormat::RGBA8),   "RGBA8")     == 0);
    CHECK(std::strcmp(toString(RenderResourceFormat::Depth32F),"Depth32F")  == 0);
}

// REGISTRY-COMPUTE-IDS-1: the 4 new compute-intermediate ids round-trip
// through toString with non-empty, unique names; Count covers them.
TEST_CASE("RenderResourceRegistry compute-intermediate ids toString") {
    CHECK(std::strcmp(toString(RenderResourceId::ClusterDepthPyramid),    "ClusterDepthPyramid")    == 0);
    CHECK(std::strcmp(toString(RenderResourceId::LightgridGrid),          "LightgridGrid")          == 0);
    CHECK(std::strcmp(toString(RenderResourceId::LightgridIndex),         "LightgridIndex")         == 0);
    CHECK(std::strcmp(toString(RenderResourceId::PostprocessComputeBlur), "PostprocessComputeBlur") == 0);

    // Count is the last enumerator; the new ids are strictly below it.
    CHECK(int(RenderResourceId::PostprocessComputeBlur) < int(RenderResourceId::Count));
    CHECK(int(RenderResourceId::ClusterDepthPyramid)    < int(RenderResourceId::Count));
}

TEST_CASE("RenderResourceRegistry all ids have non-empty unique names") {
    std::set<std::string> seen;
    for (int i = 0; i < int(RenderResourceId::Count); ++i) {
        const char* name = toString(RenderResourceId(i));
        REQUIRE(name != nullptr);
        CHECK(name[0] != '\0');
        // No duplicate names across distinct ids.
        CHECK(seen.insert(std::string(name)).second);
    }
}

TEST_CASE("RenderResourceRegistry compute-intermediate ids register and invalidate") {
    RenderResourceDesc d{};
    d.id        = RenderResourceId::ClusterDepthPyramid;
    d.kind      = RenderResourceKind::Texture2D;
    d.glName    = 4242;
    d.valid     = true;
    registerOrUpdateRenderResource(d);
    const RenderResourceDesc* got = getRenderResource(RenderResourceId::ClusterDepthPyramid);
    REQUIRE(got != nullptr);
    CHECK(got->glName == 4242);
    CHECK((got->id == RenderResourceId::ClusterDepthPyramid));

    RenderResourceDesc inv{};
    inv.id = RenderResourceId::ClusterDepthPyramid;
    registerOrUpdateRenderResource(inv);
    CHECK(getRenderResource(RenderResourceId::ClusterDepthPyramid) == nullptr);
    CHECK(getRenderResourceCount() == 0);
}

// ---------------------------------------------------------------------------
// REGISTRY-LIFETIME-CLASS-1: lifetime classification metadata
// ---------------------------------------------------------------------------

TEST_CASE("RenderResourceLifetime toString round-trips every value") {
    CHECK(std::strcmp(toString(RenderResourceLifetime::Unset),      "Unset")      == 0);
    CHECK(std::strcmp(toString(RenderResourceLifetime::FrameLocal), "FrameLocal") == 0);
    CHECK(std::strcmp(toString(RenderResourceLifetime::Mission),    "Mission")    == 0);
    CHECK(std::strcmp(toString(RenderResourceLifetime::Persistent), "Persistent") == 0);
    CHECK(std::strcmp(toString(RenderResourceLifetime::External),   "External")   == 0);
}

TEST_CASE("RenderResourceDesc defaults lifetime to Unset (no safe default)") {
    RenderResourceDesc d{};
    CHECK((d.lifetime == RenderResourceLifetime::Unset));
}

TEST_CASE("validateRenderResourceLifetimes flags a valid resource left at Unset") {
    REQUIRE(getRenderResourceCount() == 0);  // clean registry from prior tests

    // Empty registry: vacuously valid.
    RenderResourceId off = RenderResourceId::MainColor;  // poison
    CHECK(validateRenderResourceLifetimes(&off));
    CHECK((off == RenderResourceId::Unknown));

    // Register a valid resource WITHOUT a lifetime -> validator must fail and
    // name it.
    RenderResourceDesc bad{};
    bad.id    = RenderResourceId::MainColor;
    bad.valid = true;
    // bad.lifetime deliberately left at Unset
    registerOrUpdateRenderResource(bad);
    off = RenderResourceId::Unknown;
    CHECK_FALSE(validateRenderResourceLifetimes(&off));
    CHECK((off == RenderResourceId::MainColor));

    // Set the lifetime -> validator passes.
    bad.lifetime = RenderResourceLifetime::Persistent;
    registerOrUpdateRenderResource(bad);
    off = RenderResourceId::MainColor;
    CHECK(validateRenderResourceLifetimes(&off));
    CHECK((off == RenderResourceId::Unknown));

    // Clean up.
    RenderResourceDesc inv{};
    inv.id = RenderResourceId::MainColor; inv.valid = false;
    registerOrUpdateRenderResource(inv);
    CHECK(getRenderResourceCount() == 0);
}

TEST_CASE("validateRenderResourceLifetimes ignores invalid (gated-absent) slots") {
    REQUIRE(getRenderResourceCount() == 0);
    // A registered-but-invalid slot with Unset lifetime is NOT an error:
    // gated-absent is separate from lifetime.
    RenderResourceDesc inv{};
    inv.id = RenderResourceId::SsaoOcclusion; inv.valid = false;
    registerOrUpdateRenderResource(inv);
    CHECK(validateRenderResourceLifetimes(nullptr));
    CHECK(getRenderResourceCount() == 0);
}

// Drift guard: every resource whose lifetime class is External MUST appear in
// frame_graph_validate.h::kExternalResources. (The frame-graph "external" set is
// broader — it also contains seeded Mission/Persistent resources — so this is a
// SUBSET assertion, not equality. See the comment on kExternalResources.)
TEST_CASE("lifetime==External resources are all listed in kExternalResources") {
    using namespace RenderCore::framegraph;
    // Mirror the production classification for the External-class resources.
    const RenderResourceId externals[] = {
        RenderResourceId::WaterReflectionColor,
        RenderResourceId::WaterReflectionDepth,
    };
    for (RenderResourceId id : externals)
        CHECK(isInExternalResourceList(id));

    // Conversely, nothing classified FrameLocal/Mission-only sneaks in as a
    // surprise temporal: spot-check a couple that must NOT be External-class.
    // (They may still be in kExternalResources if seeded — e.g.
    // TerrainHeightTexture — so we only assert the water pair above.)
    CHECK(isInExternalResourceList(RenderResourceId::WaterReflectionColor));
    CHECK(isInExternalResourceList(RenderResourceId::WaterReflectionDepth));
}

// ---------------------------------------------------------------------------
// RenderDebugView
// ---------------------------------------------------------------------------

TEST_CASE("RenderDebugView all names non-null and non-empty") {
    for (int i = 0; i < int(RenderDebugView::_Count); ++i) {
        const char* name = RenderDebugViewName(RenderDebugView(i));
        REQUIRE(name != nullptr);
        CHECK(name[0] != '\0');
    }
}

TEST_CASE("RenderDebugView all descriptions non-null") {
    for (int i = 0; i < int(RenderDebugView::_Count); ++i) {
        const char* desc = RenderDebugViewDescription(RenderDebugView(i));
        CHECK(desc != nullptr);
    }
}

TEST_CASE("RenderDebugView spot-check known names") {
    CHECK(std::strcmp(RenderDebugViewName(RenderDebugView::Final),  "Final")  == 0);
    CHECK(std::strcmp(RenderDebugViewName(RenderDebugView::Albedo), "Albedo") == 0);
}

TEST_CASE("RenderDebugView StaticPropOpaque mask covers expected views") {
    CHECK(RenderDebugViewSupported(RenderDebugView::Final,        kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::Albedo,       kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::Normal,       kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::Roughness,    kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::Metallic,     kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::MaterialIdx,  kDebugViewMask_StaticPropOpaque));
    CHECK(RenderDebugViewSupported(RenderDebugView::TexArrayLayer,kDebugViewMask_StaticPropOpaque));
}

TEST_CASE("RenderDebugView StaticPropOpaque mask excludes lighting-only views") {
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::LightingOnly, kDebugViewMask_StaticPropOpaque));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::IblOnly,      kDebugViewMask_StaticPropOpaque));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::SpecularOnly, kDebugViewMask_StaticPropOpaque));
}

TEST_CASE("RenderDebugView placeholder lane masks are zero") {
    CHECK(kDebugViewMask_Terrain == 0u);
    CHECK(kDebugViewMask_Shadow  == 0u);
}

TEST_CASE("RenderDebugView Vfx mask covers expected views") {
    // VFX-DEBUG-VIEWS-1: only Final/Albedo map to the canonical enum. The
    // shader's Alpha/ParticleKind/Overdraw modes are VFX-local (no enum slot).
    CHECK(RenderDebugViewSupported(RenderDebugView::Final,  kDebugViewMask_Vfx));
    CHECK(RenderDebugViewSupported(RenderDebugView::Albedo, kDebugViewMask_Vfx));
    // Particles have no normal/roughness/metallic/IBL/specular/materialIdx data.
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Normal,        kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Roughness,     kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Metallic,      kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::LightingOnly,  kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::IblOnly,       kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::SpecularOnly,  kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::MaterialIdx,   kDebugViewMask_Vfx));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::TexArrayLayer, kDebugViewMask_Vfx));
}

TEST_CASE("RenderDebugView Mech mask covers expected views") {
    // MECH-DEBUG-VIEWS-1: mech.frag has Final/Albedo/Normal/LightingOnly branches.
    CHECK(RenderDebugViewSupported(RenderDebugView::Final,        kDebugViewMask_Mech));
    CHECK(RenderDebugViewSupported(RenderDebugView::Albedo,       kDebugViewMask_Mech));
    CHECK(RenderDebugViewSupported(RenderDebugView::Normal,       kDebugViewMask_Mech));
    CHECK(RenderDebugViewSupported(RenderDebugView::LightingOnly, kDebugViewMask_Mech));
    // mech.frag has no roughness/metallic/IBL/specular/materialIdx/texArrayLayer data.
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Roughness,     kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::Metallic,      kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::MaterialIdx,   kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::TexArrayLayer, kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::IblOnly,       kDebugViewMask_Mech));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView::SpecularOnly,  kDebugViewMask_Mech));
}

TEST_CASE("RenderDebugView out-of-range returns false") {
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView(-1),  kDebugViewMask_StaticPropOpaque));
    CHECK_FALSE(RenderDebugViewSupported(RenderDebugView(100), 0xFFFFFFFFu));
}

// ---------------------------------------------------------------------------
// RendererFeatureRegistry
// ---------------------------------------------------------------------------

TEST_CASE("RendererFeatureRegistry COUNT is 63") {
    // 63 since MECH-LIGHTROW-FRESHNESS-1 (MechLightFreshness append).
    // Was 62 at LIGHTROW-FRESHNESS-1 (StaticPropLightFreshness append).
    //
    // FOUND STALE 2026-07-31 (LIGHTROW-FRESHNESS-1): this assertion still read
    // 46 -- "46 since 65bae0f6" -- while RendererFeature::COUNT had already
    // reached 61. Fifteen appends landed without anyone bumping it, so whatever
    // tripwire it was meant to be has not been armed for those fifteen. It is
    // corrected here rather than deleted because the sibling assertions below
    // (table length == COUNT, every entry non-null) are the ones with real
    // content, and this one is cheap; but do not read a green run of THIS case
    // as evidence that appends are being reviewed -- read it as evidence the
    // number was updated. If it drifts again the same way, delete it in favour
    // of the length/non-null checks rather than re-pinning a number nobody
    // maintains.
    CHECK(static_cast<int>(RendererFeature::COUNT) == 63);
}

TEST_CASE("RendererFeatureRegistry kFeatureTable length matches COUNT") {
    constexpr size_t len = sizeof(kFeatureTable) / sizeof(kFeatureTable[0]);
    CHECK(len == static_cast<size_t>(RendererFeature::COUNT));
}

TEST_CASE("RendererFeatureRegistry all entries have non-null featureId and doc") {
    for (int i = 0; i < static_cast<int>(RendererFeature::COUNT); ++i) {
        REQUIRE(kFeatureTable[i].featureId != nullptr);
        CHECK(kFeatureTable[i].featureId[0] != '\0');
        CHECK(kFeatureTable[i].doc != nullptr);
    }
}

TEST_CASE("RendererFeatureRegistry ViewUniforms is default-on with expected envVar") {
    const EnvVarDesc& e = kFeatureTable[static_cast<int>(RendererFeature::ViewUniforms)];
    CHECK(e.defaultOn == true);
    REQUIRE(e.envVar != nullptr);
    CHECK(std::strcmp(e.envVar, "MC2_VIEW_UNIFORMS") == 0);
}

TEST_CASE("RendererFeatureRegistry always-on features have null envVar") {
    // ReverseZ has no env var kill-switch.
    // (TerrainTessellation gained the MC2_TERRAIN_TESS default-ON killswitch
    // in the ISSUE-34 fix batch — asserted in the next case instead.)
    CHECK(kFeatureTable[static_cast<int>(RendererFeature::ReverseZ)].envVar           == nullptr);
}

TEST_CASE("RendererFeatureRegistry TerrainTessellation is a default-ON killswitch (ISSUE-34)") {
    const EnvVarDesc& e = kFeatureTable[static_cast<int>(RendererFeature::TerrainTessellation)];
    CHECK(e.defaultOn == true);
    REQUIRE(e.envVar != nullptr);
    CHECK(std::strcmp(e.envVar, "MC2_TERRAIN_TESS") == 0);
}

TEST_CASE("RendererFeatureRegistry Track V post/grounding gates are registered default-OFF") {
    // TRACKV-GATE-DEFAULT-OFF-TEST-1: every experimental Track V visual gate
    // MUST be a Feature-kind entry that defaults OFF, so a fresh checkout or a
    // CI run never silently enables a visual feature. Promoting one to
    // default-ON is a deliberate edit in RendererFeatureRegistry.h AND here --
    // this guardrail trips on accidental flips and forces explicit review.
    struct Row { RendererFeature f; const char* env; };
    const Row trackV[] = {
        { RendererFeature::HdrPost,     "MC2_HDR_POST" },
        { RendererFeature::Bloom,       "MC2_BLOOM" },
        { RendererFeature::TonemapAces, "MC2_TONEMAP_ACES" },
        { RendererFeature::Ssao,        "MC2_SSAO" },
    };
    for (const Row& r : trackV) {
        const EnvVarDesc& e = kFeatureTable[static_cast<int>(r.f)];
        CHECK((e.kind == EnvVarKind::Feature));
        CHECK(e.defaultOn == false);
        REQUIRE(e.envVar != nullptr);
        CHECK(std::strcmp(e.envVar, r.env) == 0);
    }
}

TEST_CASE("RendererFeatureRegistry Track V VFX payoff gates are registered default-OFF") {
    // TRACKV-VFX-PAYOFF-OPUS-1: VFX visual-payoff gates MUST be Feature-kind
    // entries that default OFF, so a fresh checkout never silently enables a
    // VFX visual change. Promoting one to default-ON is a deliberate edit in
    // RendererFeatureRegistry.h AND here.
    struct Row { RendererFeature f; const char* env; };
    const Row vfx[] = {
        { RendererFeature::VfxSoftParticles, "MC2_VFX_SOFT_PARTICLES" },
        { RendererFeature::VfxLitParticles,  "MC2_VFX_LIT_PARTICLES" },
    };
    for (const Row& r : vfx) {
        const EnvVarDesc& e = kFeatureTable[static_cast<int>(r.f)];
        CHECK((e.kind == EnvVarKind::Feature));
        CHECK(e.defaultOn == false);
        REQUIRE(e.envVar != nullptr);
        CHECK(std::strcmp(e.envVar, r.env) == 0);
    }
}

TEST_CASE("RendererFeatureRegistry Retired entries have Retired kind") {
    CHECK((kFeatureTable[static_cast<int>(RendererFeature::GpuMechs)].kind              == EnvVarKind::Retired));
    CHECK((kFeatureTable[static_cast<int>(RendererFeature::StaticPropIndirect)].kind   == EnvVarKind::Retired));
    CHECK((kFeatureTable[static_cast<int>(RendererFeature::DrawPacketStaticPropV6)].kind == EnvVarKind::Retired));
}

// ---------------------------------------------------------------------------
// IblShRegistry
// ---------------------------------------------------------------------------

TEST_CASE("IblShRegistry default set exists") {
    const IblShSet* s = findShSetByName("default");
    REQUIRE(s != nullptr);
    CHECK(std::strcmp(s->name, "default") == 0);
    CHECK(s->coeffs != nullptr);
}

TEST_CASE("IblShRegistry findShSetByName is case-insensitive") {
    CHECK(findShSetByName("DEFAULT") != nullptr);
    CHECK(findShSetByName("Default") != nullptr);
}

TEST_CASE("IblShRegistry findShSetByName returns null for unknown") {
    CHECK(findShSetByName("nonexistent_set") == nullptr);
    CHECK(findShSetByName(nullptr)           == nullptr);
    CHECK(findShSetByName("")                == nullptr);
}

TEST_CASE("IblShRegistry lookupShSet always returns default fallback") {
    const IblShSet& def = lookupShSet(nullptr);
    CHECK(std::strcmp(def.name, "default") == 0);

    const IblShSet& unknown = lookupShSet("totally_unknown_mission");
    CHECK(std::strcmp(unknown.name, "default") == 0);

    // kMissionShMap currently empty; any real mission name falls to default.
    const IblShSet& mc201 = lookupShSet("mc2_01");
    CHECK(std::strcmp(mc201.name, "default") == 0);
}

TEST_CASE("IblShRegistry kIblShSetCount is at least 1") {
    CHECK(kIblShSetCount >= 1u);
    CHECK(std::strcmp(kIblShSets[0].name, "default") == 0);
}

// ---------------------------------------------------------------------------
// RenderPassContract.pipelineDescRegistered audit
// PIPELINE-DESC-REGISTERED-AUDIT-1: the header's static_assert catches
// kRenderPassContracts[] array-length drift but explicitly NOT field-value
// staleness ("that is on you"). These cases machine-check the
// pipelineDescRegistered booleans against current PipelineRegistry coverage,
// so an aspirational flip (a pass claiming PipelineDesc routing it doesn't
// have) fails the build's test gate instead of silently mis-reporting in the
// editor inspector / closure-audit doc.
//
// NOTE: this slice does NOT flip any pipelineDescRegistered value. It locks
// the current shipped truth: only StaticPropOpaque routes through PipelineDesc.
// ---------------------------------------------------------------------------

TEST_CASE("RenderPassContract pipelineDescRegistered matches current shipped truth") {
    // Per-row expected state from kRenderPassContracts at branch tip
    // (FRAMEGRAPH-STATEPACK-SKELETON-1 a6854d69 flipped six lanes from the
    // earlier StaticProp/Mech-only truth to applyPipeline()-routed):
    //   true:  StaticPropOpaque, MechOpaque, Terrain, TerrainDecal,
    //          TerrainOverlay, Water, VFX, PostProcess
    //   false: Shadow (descriptive-only PipelineIds), VegetationCards, UI.
    int trueCount = 0;
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        const RenderPassContract& c = kRenderPassContracts[i];
        const bool expected = (c.id == RenderPassId::StaticPropOpaque ||
                               c.id == RenderPassId::MechOpaque ||
                               c.id == RenderPassId::Terrain ||
                               c.id == RenderPassId::TerrainDecal ||
                               c.id == RenderPassId::TerrainOverlay ||
                               c.id == RenderPassId::Water ||
                               c.id == RenderPassId::VFX ||
                               c.id == RenderPassId::PostProcess);
        CHECK(c.pipelineDescRegistered == expected);
        if (c.pipelineDescRegistered) ++trueCount;
    }
    // Eight passes route through applyPipeline() today (see list above).
    CHECK(trueCount == 8);
}

TEST_CASE("RenderPassContract: PipelineId-family passes back their flag with a real enumerator") {
    // PipelineRegistry registers the static-prop family + MechOpaque
    // (MECH-PIPELINEDESC-1) plus a set of DESCRIPTIVE-ONLY rows (shadow casters,
    // terrain overlay/decal, water, VFX, post-process) that are NOT routed through
    // applyPipeline. The enum has since grown to Count_=25:
    //   0 Invalid, 1 StaticPropOpaque, 2 StaticPropAlphaTest, 3 MechOpaque,
    //   4 StaticPropDepth, 5..24 descriptive-only families, 25 Count_.
    //
    // Since FRAMEGRAPH-STATEPACK-SKELETON-1 (a6854d69) pipelineDescRegistered
    // means "routes through applyPipeline()", which is BROADER than "owns a
    // dedicated PipelineId enumerator": Terrain/Water/VFX/PostProcess/
    // TerrainDecal/TerrainOverlay route applyPipeline with a StatePack whose
    // PipelineId is Invalid (multi-sub-pipeline lanes), yet they are correctly
    // flagged true. So the old "only a registered PipelineId family may claim"
    // invariant no longer holds. What still holds: the two lanes that DO own a
    // PipelineId family (StaticPropOpaque, MechOpaque) must be flagged true and
    // map to a real (non-Invalid, in-range) enumerator.
    //
    // Count_ is hardcoded as a tripwire: bump it (and re-examine the family set
    // below) every time a PipelineId enumerator is added.
    CHECK(static_cast<uint32_t>(PipelineId::Count_) == 25u);
    CHECK(static_cast<uint32_t>(PipelineId::Invalid) == 0u);

    for (int i = 0; i < kRenderPassContractCount; ++i) {
        const RenderPassContract& c = kRenderPassContracts[i];
        const bool hasFamily = (c.id == RenderPassId::StaticPropOpaque ||
                                c.id == RenderPassId::MechOpaque);
        if (!hasFamily) continue;

        // A lane that owns a PipelineId family must be flagged registered.
        CHECK(c.pipelineDescRegistered);

        const uint32_t fam =
            (c.id == RenderPassId::MechOpaque)
                ? static_cast<uint32_t>(PipelineId::MechOpaque)
                : static_cast<uint32_t>(PipelineId::StaticPropOpaque);
        CHECK(fam > static_cast<uint32_t>(PipelineId::Invalid));
        CHECK(fam < static_cast<uint32_t>(PipelineId::Count_));
    }
}

TEST_CASE("RenderPassContract: non-applyPipeline lanes stay unregistered") {
    // Defensive complement -- since FRAMEGRAPH-STATEPACK-SKELETON-1 the lanes
    // that do NOT route through applyPipeline() are exactly Shadow (its three
    // shadow PipelineIds are descriptive-only per PipelineRegistry.h),
    // VegetationCards, and UI. Every one of them must report false.
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        const RenderPassContract& c = kRenderPassContracts[i];
        if (c.id != RenderPassId::Shadow &&
            c.id != RenderPassId::VegetationCards &&
            c.id != RenderPassId::UI) continue;
        CHECK_FALSE(c.pipelineDescRegistered);
    }
}

// ---------------------------------------------------------------------------
// RENDER-PASS-DAG-CONTRACT-1: widened pass enum + frame pass order.
// (RenderCore-only checks. The toRenderPassId mapping is exercised in
// test_render_contract_3, which links render_contract.cpp.)
// ---------------------------------------------------------------------------

TEST_CASE("RenderPassContract: enum/table/order counts are all 11") {
    CHECK(kRenderPassIdCount == 11u);
    CHECK(kRenderPassContractCount == 11);
    CHECK(kFramePassOrderCount == 11);
}

TEST_CASE("RenderPassContract: 6 new lanes resolve to contract rows with current registration") {
    // RENDER-PASS-DAG-CONTRACT-1 added six lanes. FRAMEGRAPH-STATEPACK-SKELETON-1
    // (a6854d69) subsequently routed four of them through applyPipeline():
    //   registered=true:  Water, PostProcess, TerrainDecal, TerrainOverlay
    //   registered=false: VegetationCards, UI
    struct LaneExpect { RenderPassId id; bool registered; };
    const LaneExpect newIds[] = {
        { RenderPassId::Water,           true  },
        { RenderPassId::PostProcess,     true  },
        { RenderPassId::VegetationCards, false },
        { RenderPassId::TerrainDecal,    true  },
        { RenderPassId::TerrainOverlay,  true  },
        { RenderPassId::UI,              false },
    };
    for (const LaneExpect& want : newIds) {
        const RenderPassContract* row = nullptr;
        for (int i = 0; i < kRenderPassContractCount; ++i)
            if (kRenderPassContracts[i].id == want.id) { row = &kRenderPassContracts[i]; break; }
        REQUIRE(row != nullptr);
        CHECK(row->pipelineDescRegistered == want.registered);
    }
}

TEST_CASE("RenderPassContract: kFramePassOrder lists every real id exactly once, no None") {
    int counts[12] = {0};  // index by RenderPassId value (1..11)
    for (int i = 0; i < kFramePassOrderCount; ++i) {
        uint32_t v = static_cast<uint32_t>(kFramePassOrder[i]);
        CHECK(v != static_cast<uint32_t>(RenderPassId::None));
        REQUIRE(v >= 1u);
        REQUIRE(v <= kRenderPassIdCount);
        ++counts[v];
    }
    for (uint32_t v = 1; v <= kRenderPassIdCount; ++v)
        CHECK(counts[v] == 1);
}

TEST_CASE("RenderPassContract: Shadow + PostProcess carry non-default Vulkan metadata") {
    const RenderPassContract* shadow = nullptr;
    const RenderPassContract* post   = nullptr;
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        if (kRenderPassContracts[i].id == RenderPassId::Shadow)      shadow = &kRenderPassContracts[i];
        if (kRenderPassContracts[i].id == RenderPassId::PostProcess) post   = &kRenderPassContracts[i];
    }
    REQUIRE(shadow != nullptr);
    REQUIRE(post != nullptr);
    CHECK((shadow->depthLoadOp == LoadOp::Clear));
    CHECK((shadow->depthFinalLayout == ImageLayout::ShaderReadOnly));
    CHECK((post->colorFinalLayout == ImageLayout::Present));
}

} // TEST_SUITE("RenderCore")

// ---------------------------------------------------------------------------
// PipelineDesc + render_contract -> PipelineDesc adapter
// PIPELINE-DESC-SCAFFOLD-1: GL-free conversion substrate (no production caller
// yet; this proves the adapter lowers a PassStateContract correctly).
// ---------------------------------------------------------------------------

namespace {

// Field-by-field equality. PipelineDesc deliberately has NO operator== (the
// scaffold slice changes the struct by zero bytes); the test owns comparison.
bool pipelineDescEqual(const PipelineDesc& a, const PipelineDesc& b) {
    return a.glProgramName        == b.glProgramName
        && a.blend                == b.blend
        && a.depthTestEnable      == b.depthTestEnable
        && a.depthWriteEnable     == b.depthWriteEnable
        && a.depthFunc            == b.depthFunc
        && a.cullMode             == b.cullMode
        && a.colorAttachments.color0 == b.colorAttachments.color0
        && a.colorAttachments.color1 == b.colorAttachments.color1
        && a.colorAttachments.color2 == b.colorAttachments.color2
        && a.objectIdWriteEnabled == b.objectIdWriteEnabled
        && a.ssboBindingsMask     == b.ssboBindingsMask;
}

// Representative opaque MRT+objectId contract, mirroring kStaticPropState in
// render_contract.cpp (which the GL-free test target cannot link).
render_contract::PassStateContract makeStaticPropContract() {
    render_contract::PassStateContract c{};
    c.requiresDepthTest   = true;
    c.requiresDepthWrite  = true;
    c.blend               = render_contract::PassStateContract::BlendMode::Opaque;
    c.requiresMRT         = true;
    c.attachmentCount     = 3;
    c.attachments         = render_contract::RequiredAttachments{true, true, true};
    c.expectedFBO         = "scene HDR FBO (MRT + objectId)";
    c.restoresStateOnExit = false;
    return c;
}

} // namespace

TEST_SUITE("RenderCore") {

TEST_CASE("PipelineDesc value-init zeroes every field") {
    PipelineDesc d{};
    CHECK(d.glProgramName == 0u);
    CHECK((d.blend == BlendMode::Opaque));            // enum value 0
    CHECK(d.depthTestEnable  == false);
    CHECK(d.depthWriteEnable == false);
    CHECK((d.depthFunc == DepthFunc::LessEqual));     // enum value 0
    CHECK((d.cullMode  == CullMode::None));           // enum value 0
    CHECK(d.colorAttachments.color0 == false);
    CHECK(d.colorAttachments.color1 == false);
    CHECK(d.colorAttachments.color2 == false);
    CHECK(d.objectIdWriteEnabled == false);
    CHECK(d.ssboBindingsMask == 0u);
}

TEST_CASE("PipelineDesc field-equality helper agrees on identical descs") {
    PipelineDesc a = render_contract::pipelineDescFromPassContract(makeStaticPropContract());
    PipelineDesc b = render_contract::pipelineDescFromPassContract(makeStaticPropContract());
    CHECK(pipelineDescEqual(a, b));

    b.depthWriteEnable = !b.depthWriteEnable;          // perturb one field
    CHECK_FALSE(pipelineDescEqual(a, b));
}

TEST_CASE("pipelineDescFromPassContract converts a representative pass contract") {
    render_contract::PassStateContract c = makeStaticPropContract();
    PipelineDesc d = render_contract::pipelineDescFromPassContract(
        c,
        /*glProgramName*/ 42u,
        /*depthFunc*/     DepthFunc::GreaterEqual,
        /*cullMode*/      CullMode::Back,
        /*objectIdWrite*/ true,
        /*ssboMask*/      (1u << 16));   // BASE_INSTANCE slot

    // Derived from the contract.
    CHECK(d.depthTestEnable  == true);
    CHECK(d.depthWriteEnable == true);
    CHECK((d.blend == BlendMode::Opaque));
    CHECK(d.colorAttachments.color0 == true);
    CHECK(d.colorAttachments.color1 == true);
    CHECK(d.colorAttachments.color2 == true);

    // Passed through explicitly (PassStateContract carries none of these).
    CHECK(d.glProgramName == 42u);
    CHECK((d.depthFunc == DepthFunc::GreaterEqual));
    CHECK((d.cullMode  == CullMode::Back));
    CHECK(d.objectIdWriteEnabled == true);
    CHECK(d.ssboBindingsMask == (1u << 16));
}

TEST_CASE("pipelineDescFromPassContract applies documented MC2 defaults") {
    // Minimal contract; all explicit args omitted -> defaults must hold.
    render_contract::PassStateContract c{};
    c.blend       = render_contract::PassStateContract::BlendMode::Opaque;
    c.attachments = render_contract::RequiredAttachments{true, false, false};

    PipelineDesc d = render_contract::pipelineDescFromPassContract(c);
    CHECK(d.glProgramName == 0u);                       // unregistered
    CHECK((d.depthFunc == DepthFunc::GreaterEqual));     // reverse-Z default
    CHECK((d.cullMode  == CullMode::Back));              // opaque-geometry default
    CHECK(d.objectIdWriteEnabled == false);
    CHECK(d.ssboBindingsMask == 0u);
}

TEST_CASE("pipelineDescFromPassContract maps every blend mode") {
    using BM = render_contract::PassStateContract::BlendMode;
    render_contract::PassStateContract c{};
    c.attachments = render_contract::RequiredAttachments{true, false, false};

    c.blend = BM::Opaque;
    CHECK((render_contract::pipelineDescFromPassContract(c).blend == BlendMode::Opaque));
    c.blend = BM::AlphaBlend;
    CHECK((render_contract::pipelineDescFromPassContract(c).blend == BlendMode::AlphaBlend));
    c.blend = BM::AlphaTest;
    CHECK((render_contract::pipelineDescFromPassContract(c).blend == BlendMode::AlphaTest));
    c.blend = BM::Additive;
    CHECK((render_contract::pipelineDescFromPassContract(c).blend == BlendMode::Additive));
}

TEST_CASE("pipelineDescFromPassContract preserves attachment/draw-buffer contract") {
    // Each row is an actual contract attachment pattern from render_contract.cpp.
    struct Row { bool c0, c1, c2; } rows[] = {
        {false, false, false},   // ShadowCaster: depth-only FBO
        {true,  false, false},   // PostProcess / UI: single attachment
        {true,  true,  false},   // TerrainBase / Water: MRT, no objectId
        {true,  true,  true},    // StaticProp / OpaqueObject: MRT + objectId
    };
    for (const Row& r : rows) {
        render_contract::PassStateContract c{};
        c.attachments = render_contract::RequiredAttachments{r.c0, r.c1, r.c2};
        PipelineDesc d = render_contract::pipelineDescFromPassContract(c);
        CHECK(d.colorAttachments.color0 == r.c0);
        CHECK(d.colorAttachments.color1 == r.c1);
        CHECK(d.colorAttachments.color2 == r.c2);
    }
}

TEST_CASE("pipelineDescFromPassContract carries depth test/write independently") {
    render_contract::PassStateContract c{};
    c.attachments = render_contract::RequiredAttachments{true, true, false};

    // AlphaObject pattern: depth test ON, depth write OFF.
    c.requiresDepthTest  = true;
    c.requiresDepthWrite = false;
    PipelineDesc d = render_contract::pipelineDescFromPassContract(c);
    CHECK(d.depthTestEnable  == true);
    CHECK(d.depthWriteEnable == false);

    // PostProcess pattern: both OFF.
    c.requiresDepthTest  = false;
    c.requiresDepthWrite = false;
    d = render_contract::pipelineDescFromPassContract(c);
    CHECK(d.depthTestEnable  == false);
    CHECK(d.depthWriteEnable == false);
}

TEST_CASE("PipelineDesc stays within its hot-path size budget") {
    // Mirrors the static_assert in PipelineDesc.h; a runtime guard so a struct
    // growth shows up as a failing test, not only a compile error elsewhere.
    CHECK(sizeof(PipelineDesc) <= 20u);
}

// ---------------------------------------------------------------------------
// MechVisualState — GAMEADAPTERS-VISUAL-STATE-BRIDGE-1
// ---------------------------------------------------------------------------
TEST_CASE("MechVisualState defaults to safe-neutral") {
    MechVisualState s;
    CHECK(s.heat01 == 0.0f);
    CHECK(s.damage01 == 0.0f);
    CHECK(s.flags == 0u);
}

TEST_CASE("MechVisualState is a 12-byte trivially-copyable POD") {
    CHECK(sizeof(MechVisualState) == 12u);
    CHECK(std::is_trivially_copyable<MechVisualState>::value);
}

TEST_CASE("sanitizeMechVisual01 clamps to [0,1] and rejects non-finite") {
    CHECK(sanitizeMechVisual01(0.5f)  == doctest::Approx(0.5f));
    CHECK(sanitizeMechVisual01(-1.0f) == 0.0f);   // below range
    CHECK(sanitizeMechVisual01(2.0f)  == 1.0f);   // above range
    CHECK(sanitizeMechVisual01(0.0f)  == 0.0f);
    CHECK(sanitizeMechVisual01(1.0f)  == 1.0f);
    // getStatusRating() divides by maxArmor with no zero guard -> NaN/Inf must
    // map to the safe-neutral 0.0, never propagate into the render path.
    CHECK(sanitizeMechVisual01(NAN)       == 0.0f);
    CHECK(sanitizeMechVisual01(INFINITY)  == 1.0f);   // +Inf >= 1 -> clamps to 1
    CHECK(sanitizeMechVisual01(-INFINITY) == 0.0f);
}

TEST_CASE("packMechRelation packs relation into bits [4:5] without disturbing others") {
    uint32_t f = kMechVisualFlag_Selected | kMechVisualFlag_Destroyed;
    f = packMechRelation(f, 2u);  // enemy
    // existing flags preserved
    CHECK((f & kMechVisualFlag_Selected) != 0u);
    CHECK((f & kMechVisualFlag_Destroyed) != 0u);
    // relation field reads back as 2
    CHECK(((f & kMechVisualFlag_RelationMask) >> kMechVisualFlag_RelationShift) == 2u);
    // re-packing a different relation overwrites cleanly (no accumulation)
    f = packMechRelation(f, 0u);  // own
    CHECK(((f & kMechVisualFlag_RelationMask) >> kMechVisualFlag_RelationShift) == 0u);
    CHECK((f & kMechVisualFlag_Selected) != 0u);  // still preserved
}

} // TEST_SUITE("RenderCore")
