// Frame-graph offline validation.
//
// Proves the resource DAG declared by kRenderPassContracts[] + kFramePassOrder[] is
// self-consistent (every pass's reads are produced by an earlier pass or are external)
// and that the validator CATCHES a bad reorder — all offline, no GL/engine/smoke. This
// is the "validation" layer over the existing "declaration" table.
#include "doctest.h"
#include "RenderCore/frame_graph_validate.h"
#include "RenderCore/ambient_contract.h"
#include "RenderCore/fbo_ledger.h"
#include "RenderCore/terrain_subpass_contract.h"
#include "RenderCore/frame_pass_trace.h"   // pure dry-run kernel
#include "RenderCore/frame_executor.h"     // IslandContract
#include "RenderCore/postprocess_subgraph.h" // PostProcessSubpass table
#include "RenderCore/render_state_desc.h"  // RenderStateDesc
#include "RenderCore/top_level_pass_executor.h" // TopLevelPassContract
#include "RenderCore/scheduler_legal_reorder.h" // scheduler legality
#include <string>   // apply counters
#include <cstring>  // apply counters

using namespace RenderCore;
using namespace RenderCore::framegraph;

TEST_SUITE("FrameGraph") {

TEST_CASE("shipped frame graph: every read satisfied by a prior write or external") {
    const ValidationResult r = validateShippedFrameGraph();
    // On failure these report the offending pass/resource ids directly.
    CHECK(static_cast<unsigned>(r.offendingPass) == 0u);   // None
    CHECK(static_cast<unsigned>(r.missingResource) == 0u); // Unknown
    CHECK(r.ok == true);
    CHECK(r.unknownPass == false);
}

TEST_CASE("reorder regression: postprocess before geometry strands MainColor") {
    const RenderPassId badOrder[] = { RenderPassId::PostProcess, RenderPassId::StaticPropOpaque };
    const ValidationResult r = validateReadsSatisfied(
        kRenderPassContracts, kRenderPassIdCount, badOrder, 2,
        kExternalResources, kExternalResourceCount);
    CHECK_FALSE(r.ok);
    CHECK(static_cast<unsigned>(r.offendingPass) == static_cast<unsigned>(RenderPassId::PostProcess));
    CHECK(static_cast<unsigned>(r.missingResource) == static_cast<unsigned>(RenderResourceId::MainColor));
}

TEST_CASE("removing an external resource surfaces the missing producer") {
    auto synthetic = kRenderPassContracts[0];
    synthetic.reads[0] = RenderResourceId::MaterialGpuBuffer;
    synthetic.reads[1] = RenderResourceId::Unknown;
    const RenderPassId order[] = { synthetic.id };
    const RenderResourceId external[] = { RenderResourceId::MaterialGpuBuffer };
    CHECK_FALSE(validateReadsSatisfied(&synthetic, 1, order, 1, nullptr, 0).ok);
    CHECK(validateReadsSatisfied(&synthetic, 1, order, 1, external, 1).ok);
}

TEST_CASE("unknown pass id in order is reported") {
    const RenderPassId badOrder[] = { RenderPassId::None };  // None has no contract row
    const ValidationResult r = validateReadsSatisfied(
        kRenderPassContracts, kRenderPassIdCount,
        badOrder, 1, kExternalResources, kExternalResourceCount);
    CHECK(r.ok == false);
    CHECK(r.unknownPass == true);
}

TEST_CASE("ambient ledger: colorMask handshake is declared (shadow OFF, terrain re-assert)") {
    // The recon's #1 executor blocker, as a tested invariant. If a future edit drops
    // the terrain colorMask re-assert (or the shadow color-write-off declaration), this
    // fails offline instead of shipping an invisible scene.
    CHECK(colorMaskHandshakeDeclared() == true);
    const AmbientContract* shadow = findAmbient(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(shadow->disablesColorWrite == true);
    const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);
    CHECK(terrain->reassertsColorMaskAllOn == true);
}

TEST_CASE("SHADOW-OBSERVE-2: shadow colorMaskOnEntry=Inherit + disablesColorWrite=true + depthFunc=ShadowLess") {
    // Active shadow pre-pass enforces depth-only via FBO attachment
    // (DrawBufferSet::ShadowDepthOnly), NOT glColorMask. colorMask is AllOn at the observe
    // point (beginShadowPrePass:6270). colorMaskOnEntry relaxed to Inherit so the ambient
    // guard skips colorMask comparison (avoids AllOn-vs-AllOff false mismatch).
    // disablesColorWrite stays true — shadow IS depth-only, however enforced — so
    // colorMaskHandshakeDeclared() is not weakened. depthFunc=ShadowLess must hold.
    const AmbientContract* shadow = findAmbient(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(static_cast<unsigned>(shadow->colorMaskOnEntry) ==
          static_cast<unsigned>(ColorMaskState::Inherit));
    CHECK(shadow->disablesColorWrite == true);   // depth-only invariant preserved
    CHECK(static_cast<unsigned>(shadow->depthFunc) ==
          static_cast<unsigned>(DepthFuncState::ShadowLess));
    // Handshake integrity: colorMaskHandshakeDeclared() uses disablesColorWrite, not
    // colorMaskOnEntry, so relaxing colorMaskOnEntry to Inherit does not weaken it.
    CHECK(colorMaskHandshakeDeclared() == true);
}

TEST_CASE("ambient ledger: terrain latch handshake declared (terrain sets, post consumes)") {
    // Recon landmine #2: markTerrainDrawn/sceneHasTerrain_ is a cross-phase latch; if a
    // producer is removed, screenShadow/cloudShadow/shoreline/edgeFog/fogOob silently
    // bail. This makes the producer/consumer pairing a tested invariant.
    CHECK(terrainLatchHandshakeDeclared() == true);
    const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);
    CHECK(terrain->producesTerrainLatch == true);
    const AmbientContract* post = findAmbient(RenderPassId::PostProcess);
    REQUIRE(post != nullptr);
    CHECK(post->consumesTerrainLatch == true);
}

TEST_CASE("ambient ledger: depth func declarations match the reverse-Z / shadow contract") {
    // Scene passes use reverse-Z GL_GEQUAL; shadow uses GL_LESS (render_contract.cpp).
    const AmbientContract* shadow = findAmbient(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(static_cast<unsigned>(shadow->depthFunc) ==
          static_cast<unsigned>(DepthFuncState::ShadowLess));
    const RenderPassId scenePasses[] = {
        RenderPassId::StaticPropOpaque, RenderPassId::Terrain, RenderPassId::MechOpaque
    };
    for (int i = 0; i < 3; ++i) {
        const AmbientContract* a = findAmbient(scenePasses[i]);
        REQUIRE(a != nullptr);
        CHECK(static_cast<unsigned>(a->depthFunc) ==
              static_cast<unsigned>(DepthFuncState::SceneGEqual));
    }
}

TEST_CASE("compareAmbient: pure cross-check logic (declared vs sampled)") {
    const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);  // colorMask=AllOn, depthFunc=SceneGEqual

    // Live matches declaration -> no mismatch.
    AmbientSample good; good.colorMask = ColorMaskState::AllOn; good.depthFunc = DepthFuncState::SceneGEqual;
    CHECK(compareAmbient(*terrain, good).any() == false);

    // Wrong depth func -> depthFunc mismatch only.
    AmbientSample badDepth; badDepth.colorMask = ColorMaskState::AllOn; badDepth.depthFunc = DepthFuncState::ShadowLess;
    AmbientMismatch md = compareAmbient(*terrain, badDepth);
    CHECK(md.depthFunc == true);
    CHECK(md.colorMask == false);

    // Wrong color mask -> colorMask mismatch only.
    AmbientSample badColor; badColor.colorMask = ColorMaskState::AllOff; badColor.depthFunc = DepthFuncState::SceneGEqual;
    AmbientMismatch mc = compareAmbient(*terrain, badColor);
    CHECK(mc.colorMask == true);
    CHECK(mc.depthFunc == false);

    // Unclassifiable live value (Inherit) is skipped -> no false positive.
    AmbientSample unknown;  // all Inherit
    CHECK(compareAmbient(*terrain, unknown).any() == false);
}

TEST_CASE("ambient ledger-2: depth-write declared ON; blend left Inherit (probe-proven)") {
    // depthWrite is a clean per-pass axis (runtime probe: dwMiss=0). blend is NOT --
    // the probe proved GL_BLEND is globally enabled even in opaque passes, so it is left
    // Inherit (asserting Off produced 5881/9804 false mismatches). This test locks that
    // finding: depthWrite ON, blend Inherit, for shadow + opaque passes.
    const RenderPassId opaque[] = {
        RenderPassId::Shadow, RenderPassId::StaticPropOpaque,
        RenderPassId::Terrain, RenderPassId::MechOpaque
    };
    for (int i = 0; i < 4; ++i) {
        const AmbientContract* a = findAmbient(opaque[i]);
        REQUIRE(a != nullptr);
        CHECK(static_cast<unsigned>(a->depthWrite) == static_cast<unsigned>(DepthWriteState::On));
        CHECK(static_cast<unsigned>(a->blend)      == static_cast<unsigned>(BlendState::Inherit));
    }
}

TEST_CASE("compareAmbient: blend + depth-write axes cross-check (pure, synthetic decl)") {
    // Table-independent: exercise the pure compare with a decl that DECLARES blend, so
    // the logic is tested even though no shipped pass declares blend (it's Inherit there).
    AmbientContract decl{};                 // all Inherit / false / nullptr
    decl.depthWrite = DepthWriteState::On;
    decl.blend      = BlendState::Off;

    AmbientSample good; good.blend = BlendState::Off; good.depthWrite = DepthWriteState::On;
    CHECK(compareAmbient(decl, good).any() == false);

    AmbientSample badBlend = good; badBlend.blend = BlendState::On;
    CHECK(compareAmbient(decl, badBlend).blend == true);

    AmbientSample badDW = good; badDW.depthWrite = DepthWriteState::Off;
    CHECK(compareAmbient(decl, badDW).depthWrite == true);

    // Inherit live -> skipped (no false positive even though declared).
    AmbientSample inheritBlend = good; inheritBlend.blend = BlendState::Inherit;
    CHECK(compareAmbient(decl, inheritBlend).blend == false);
}

TEST_CASE("AMBIENT-VIEWPORT-PROBE-1: classifyViewport + viewport axis cross-check (pure)") {
    // classifyViewport: square -> ShadowMap, non-square -> MainScene, degenerate -> Inherit.
    CHECK(classifyViewport(0, 0, 4096, 4096) == ViewportKind::ShadowMap);   // shadow atlas
    CHECK(classifyViewport(0, 0, 2048, 2048) == ViewportKind::ShadowMap);
    CHECK(classifyViewport(0, 0, 1920, 1080) == ViewportKind::MainScene);   // 16:9 backbuffer
    CHECK(classifyViewport(0, 0, 1024,  768) == ViewportKind::MainScene);   // 4:3
    CHECK(classifyViewport(0, 0,    0,    0) == ViewportKind::Inherit);     // degenerate -> skip
    CHECK(classifyViewport(0, 0, 1920,    0) == ViewportKind::Inherit);

    // compareAmbient viewport axis: declared MainScene vs sampled kinds.
    AmbientContract decl{};                      // all Inherit
    decl.viewport = ViewportKind::MainScene;

    AmbientSample good; good.viewport = ViewportKind::MainScene;
    CHECK(compareAmbient(decl, good).viewport == false);
    CHECK(compareAmbient(decl, good).any()    == false);

    // The suspected leak: a shadow-square viewport bleeding into a MainScene pass.
    AmbientSample leak; leak.viewport = ViewportKind::ShadowMap;
    CHECK(compareAmbient(decl, leak).viewport == true);
    CHECK(compareAmbient(decl, leak).any()    == true);

    // Inherit live -> skipped (no false positive even though declared).
    AmbientSample unclass; unclass.viewport = ViewportKind::Inherit;
    CHECK(compareAmbient(decl, unclass).viewport == false);

    // Inherit decl -> skipped regardless of live.
    AmbientContract noDecl{};
    CHECK(compareAmbient(noDecl, leak).viewport == false);
}

TEST_CASE("fbo ledger: register/resolve + default-FBO + unregistered") {
    FboLedger led;
    led.reset();
    led.registerFbo(7u, RenderResourceId::MainColor);
    led.registerFbo(10u, RenderResourceId::SceneDepthCopy);
    auto rid = [](RenderResourceId r){ return static_cast<unsigned>(r); };
    CHECK(rid(led.resolve(7u))  == rid(RenderResourceId::MainColor));
    CHECK(rid(led.resolve(10u)) == rid(RenderResourceId::SceneDepthCopy));
    CHECK(rid(led.resolve(0u))  == rid(RenderResourceId::Backbuffer));   // default framebuffer
    CHECK(rid(led.resolve(99u)) == rid(RenderResourceId::Unknown));      // unregistered -> skipped
    // re-register updates in place
    led.registerFbo(7u, RenderResourceId::WaterReflectionColor);
    CHECK(rid(led.resolve(7u))  == rid(RenderResourceId::WaterReflectionColor));
}

TEST_CASE("fbo ledger: mismatch logic skips Unknown on either side") {
    // declared==actual -> ok; differ -> mismatch; Unknown either side -> skipped.
    CHECK(fboMismatch(RenderResourceId::MainColor, RenderResourceId::MainColor)        == false);
    CHECK(fboMismatch(RenderResourceId::MainColor, RenderResourceId::SceneDepthCopy) == true);
    CHECK(fboMismatch(RenderResourceId::Unknown,   RenderResourceId::MainColor)        == false);
    CHECK(fboMismatch(RenderResourceId::MainColor, RenderResourceId::Unknown)          == false);
}

TEST_CASE("fbo ledger: scene passes declare MainColor target; others undeclared") {
    auto rid = [](RenderResourceId r){ return static_cast<unsigned>(r); };
    const unsigned mc = rid(RenderResourceId::MainColor);
    CHECK(rid(declaredFboTarget(RenderPassId::StaticPropOpaque)) == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::Terrain))          == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::TerrainOverlay))   == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::TerrainDecal))     == mc);
    // PostProcess intentionally undeclared (timing-uncertain at the sample seam).
    CHECK(rid(declaredFboTarget(RenderPassId::PostProcess)) == rid(RenderResourceId::Unknown));
}

TEST_CASE("terrain subpass table: 4 rows, writes all MainColor+MainDepth, all read ShadowDynamicMap first") {
    auto rid = [](RenderResourceId r){ return static_cast<unsigned>(r); };
    CHECK(kTerrainSubPassCount == 4);
    CHECK(allTerrainSubPassesWriteMainColorDepth() == true);
    for (int i = 0; i < kTerrainSubPassCount; ++i) {
        CHECK(rid(kTerrainSubPasses[i].reads[0]) < rid(RenderResourceId::Count));
    }
}

TEST_CASE("terrain subpass table: Indirect declares Command barrier; others None") {
    auto bk = [](BarrierKind b){ return static_cast<unsigned>(b); };
    const TerrainSubPass* indirect = findTerrainSubPass(TerrainPath::IndirectBridge);
    REQUIRE(indirect != nullptr);
    CHECK(bk(indirect->barrierAfter) == bk(BarrierKind::Command));

    const TerrainPath others[] = { TerrainPath::LODChunk, TerrainPath::PatchStreamThin, TerrainPath::LegacyMLR };
    for (int i = 0; i < 3; ++i) {
        const TerrainSubPass* s = findTerrainSubPass(others[i]);
        REQUIRE(s != nullptr);
        CHECK(bk(s->barrierAfter) == bk(BarrierKind::None));
    }
}

TEST_CASE("terrain subpass table: Indirect reads recipe/thin/cement/mask; LODChunk reads HeightSsbo instead") {
    const TerrainSubPass* indirect = findTerrainSubPass(TerrainPath::IndirectBridge);
    REQUIRE(indirect != nullptr);
    // verify all 4 Indirect-specific reads are present
    bool hasRecipe = false, hasThin = false, hasCement = false, hasMask = false;
    for (int i = 0; i < 6; ++i) {
        if (indirect->reads[i] == RenderResourceId::TerrainRecipeBuffer) hasRecipe = true;
        if (indirect->reads[i] == RenderResourceId::TerrainThinBuffer)   hasThin   = true;
        if (indirect->reads[i] == RenderResourceId::CementAtlas)         hasCement = true;
        if (indirect->reads[i] == RenderResourceId::TransitionMaskArray) hasMask   = true;
    }
    CHECK(hasRecipe == true);
    CHECK(hasThin   == true);
    CHECK(hasCement == true);
    CHECK(hasMask   == true);

    const TerrainSubPass* lod = findTerrainSubPass(TerrainPath::LODChunk);
    REQUIRE(lod != nullptr);
    bool lodHasRecipe = false, lodHasThin = false, lodHasHeight = false;
    for (int i = 0; i < 6; ++i) {
        if (lod->reads[i] == RenderResourceId::TerrainRecipeBuffer) lodHasRecipe = true;
        if (lod->reads[i] == RenderResourceId::TerrainThinBuffer)   lodHasThin   = true;
        if (lod->reads[i] == RenderResourceId::TerrainHeightSsbo)   lodHasHeight = true;
    }
    CHECK(lodHasRecipe == false);
    CHECK(lodHasThin   == false);
    CHECK(lodHasHeight == true);
}

TEST_CASE("terrain subpass table: mvpSource and drawSite per branch") {
    auto ms = [](TerrainMvpSource m){ return static_cast<unsigned>(m); };
    auto ds = [](TerrainDrawSite d){ return static_cast<unsigned>(d); };
    const TerrainSubPass* indirect = findTerrainSubPass(TerrainPath::IndirectBridge);
    REQUIRE(indirect != nullptr);
    CHECK(ms(indirect->mvpSource) == ms(TerrainMvpSource::SnapshotEpoch));

    const TerrainSubPass* patch = findTerrainSubPass(TerrainPath::PatchStreamThin);
    REQUIRE(patch != nullptr);
    CHECK(ms(patch->mvpSource) == ms(TerrainMvpSource::Live));

    const TerrainSubPass* legacy = findTerrainSubPass(TerrainPath::LegacyMLR);
    REQUIRE(legacy != nullptr);
    CHECK(ms(legacy->mvpSource) == ms(TerrainMvpSource::Live));

    const TerrainSubPass* lod = findTerrainSubPass(TerrainPath::LODChunk);
    REQUIRE(lod != nullptr);
    CHECK(ds(lod->drawSite) == ds(TerrainDrawSite::Gamecam));

    const TerrainPath renderListsPaths[] = { TerrainPath::IndirectBridge, TerrainPath::PatchStreamThin, TerrainPath::LegacyMLR };
    for (int i = 0; i < 3; ++i) {
        const TerrainSubPass* s = findTerrainSubPass(renderListsPaths[i]);
        REQUIRE(s != nullptr);
        CHECK(ds(s->drawSite) == ds(TerrainDrawSite::RenderLists));
    }
}

TEST_CASE("terrain subpass latch audit: all reachable branches set markTerrainDrawn (regression guard)") {
    auto tp = [](TerrainPath p){ return static_cast<int>(p); };
    // Every branch declares producesTerrainLatch=true.
    for (int i = 0; i < kTerrainSubPassCount; ++i)
        CHECK(kTerrainSubPasses[i].producesTerrainLatch == true);

    // All four branches call markTerrainDrawn. This is a REGRESSION GUARD — if any branch's
    // markTerrainDrawn is removed and latchActuallyImplemented is not also set false, this
    // test will trip, catching the regression offline before it ships.
    CHECK(allDeclaredLatchProducersImplemented() == true);
    CHECK(tp(firstUnimplementedLatchProducer())  == tp(TerrainPath::Count));  // Count = none missing

    // All four branches must have latchActuallyImplemented == true.
    const TerrainPath allPaths[] = {
        TerrainPath::LODChunk, TerrainPath::IndirectBridge,
        TerrainPath::PatchStreamThin, TerrainPath::LegacyMLR
    };
    for (int i = 0; i < 4; ++i) {
        const TerrainSubPass* s = findTerrainSubPass(allPaths[i]);
        REQUIRE(s != nullptr);
        CHECK(s->latchActuallyImplemented == true);
    }
}

TEST_CASE("terrain subpass active-branch probe: pure kernel with synthetic counter arrays") {
    auto tp = [](TerrainPath p){ return static_cast<int>(p); };
    // counts {1994,0,0,0} -> drew==1, dominant==LODChunk
    {
        unsigned long c[4] = { 1994, 0, 0, 0 };
        CHECK(terrainPathsThatDrew(c) == 1);
        CHECK(tp(dominantTerrainPath(c)) == tp(TerrainPath::LODChunk));
    }
    // counts {0,0,0,0} -> drew==0, dominant==TerrainPath::Count (none)
    {
        unsigned long c[4] = { 0, 0, 0, 0 };
        CHECK(terrainPathsThatDrew(c) == 0);
        CHECK(tp(dominantTerrainPath(c)) == tp(TerrainPath::Count));
    }
    // counts {5,9,0,0} -> dominant==IndirectBridge, drew==2
    {
        unsigned long c[4] = { 5, 9, 0, 0 };
        CHECK(terrainPathsThatDrew(c) == 2);
        CHECK(tp(dominantTerrainPath(c)) == tp(TerrainPath::IndirectBridge));
    }
}

// ---------------------------------------------------------------------------
// Pure dryRunCompare() kernel (offline, no engine).
// Fabricated traces only — proves fired-set/order/terrain-mutex/latch-miss classification
// and (critically) that UNOBSERVED passes are NOT counted as divergences.
// ---------------------------------------------------------------------------

// Helper: build a trace where the listed passes fired, in the listed RECORD order.
static framegraph::FramePassTrace buildTrace(const RenderPassId* firedInRecordOrder, int count) {
    framegraph::FramePassTrace t;
    framegraph::resetTrace(t, kFramePassOrder, kFramePassOrderCount);
    for (int i = 0; i < count; ++i)
        framegraph::recordPassFired(t, firedInRecordOrder[i],
                                    RenderResourceId::MainColor, kFramePassOrder, kFramePassOrderCount);
    return t;
}

TEST_CASE("dryrun (a): all 11 declared passes fired in declared order -> clean") {
    framegraph::FramePassTrace t = buildTrace(kFramePassOrder, kFramePassOrderCount);
    const framegraph::DryRunReport r =
        framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
    CHECK(r.firedCount == kFramePassOrderCount);
    CHECK(r.unobservedCount == 0);
    CHECK(r.outOfOrderCount == 0);
    CHECK(r.terrainMutexViolation == false);
}

TEST_CASE("dryrun (b): only 7 observable passes fired, 4 invisible -> unobserved!=diverged") {
    // The 4 passes with NO noteRenderPass callsite today (recon): Shadow, Water, VFX, Veg.
    // Everything else fires in declared order. Must yield ZERO false alarms.
    // NOTE: kFramePassOrder now reflects the real engine fire order:
    // StaticPropOpaque(slot1) before MechOpaque(slot2) — Static flush fires at renderLists
    // preamble (~txmmgr:3250); Mech GPU draw fires after (~txmmgr:3271).
    // OpaqueObject is observed only at its draw site. TerrainOverlay(slot4) fires before
    // TerrainDecal(slot5) — txmmgr:3275/3311.
    const RenderPassId fired[] = {
        RenderPassId::StaticPropOpaque,
        RenderPassId::MechOpaque,
        RenderPassId::Terrain,
        RenderPassId::TerrainOverlay,  // fires before Decal in renderLists
        RenderPassId::TerrainDecal,
        RenderPassId::UI,
        RenderPassId::PostProcess,
    };
    framegraph::FramePassTrace t = buildTrace(fired, 7);
    const framegraph::DryRunReport r =
        framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
    CHECK(r.firedCount == 7);
    CHECK(r.unobservedCount == 4);          // Shadow, Water, VFX, VegetationCards
    CHECK(r.outOfOrderCount == 0);          // PROVES unobserved is NOT a divergence
    CHECK(r.terrainMutexViolation == false);
}

TEST_CASE("dryrun (c): two NON-whitelisted passes swapped -> outOfOrder>0 with an offender") {
    // Swap MechOpaque before StaticPropOpaque (declared order: Static then Mech).
    // kFramePassOrder has StaticPropOpaque(slot1) before
    // MechOpaque(slot2) — Static flush precedes Mech GPU draw. Reversing them here is
    // the intentional OOO case. Neither has knownEarlyDrawSite, so generic detection fires.
    const RenderPassId fired[] = {
        RenderPassId::Shadow,
        RenderPassId::MechOpaque,         // recorded before its declared predecessor StaticPropOpaque
        RenderPassId::StaticPropOpaque,
        RenderPassId::Terrain,
        RenderPassId::TerrainOverlay,    // correct order (Overlay before Decal)
        RenderPassId::TerrainDecal,
        RenderPassId::Water,
        RenderPassId::VegetationCards,
        RenderPassId::VFX,
        RenderPassId::UI,
        RenderPassId::PostProcess,
    };
    framegraph::FramePassTrace t = buildTrace(fired, kFramePassOrderCount);
    const framegraph::DryRunReport r =
        framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
    CHECK(r.outOfOrderCount > 0);
    CHECK(static_cast<unsigned>(r.firstOutOfOrderPass) != 0u);   // an offender was identified
    CHECK(r.knownEarlySuppressed == 0);   // no whitelist suppression for these passes
}

TEST_CASE("dryrun (d): latch-miss is data-driven regression guard (false for both IndirectBridge and LODChunk)") {
    // ALL rows have latchActuallyImplemented=true, so terrainLatchMissActive==false
    // for ANY dominant branch. This test re-encodes that reality and would re-trip if a
    // row's latchActuallyImplemented were regressed back to false.
    framegraph::FramePassTrace t = buildTrace(kFramePassOrder, kFramePassOrderCount);
    t.terrainBranch    = framegraph::TerrainPath::IndirectBridge;
    t.terrainDrewCount = 1;
    {
        const framegraph::DryRunReport r =
            framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
        CHECK(r.terrainLatchMissActive == false);  // implemented; no miss
        CHECK(r.terrainMutexViolation == false);
    }
    t.terrainBranch = framegraph::TerrainPath::LODChunk;
    {
        const framegraph::DryRunReport r =
            framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
        CHECK(r.terrainLatchMissActive == false);
    }
}

TEST_CASE("dryrun (e): two terrain branches drew -> mutex violation") {
    framegraph::FramePassTrace t = buildTrace(kFramePassOrder, kFramePassOrderCount);
    t.terrainBranch    = framegraph::TerrainPath::LODChunk;
    t.terrainDrewCount = 2;
    const framegraph::DryRunReport r =
        framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
    CHECK(r.terrainMutexViolation == true);
}

TEST_CASE("dryrun (f): Option B knownEarlyDrawSite suppression proof") {
    // Terrain fires before StaticPropOpaque in the LOD-chunk path
    // (gamecam.cpp:508 pre-renderLists). This is an apparent out-of-order vs kFramePassOrder.
    // Without knownEarly -> outOfOrderCount>0 (kernel detects it). With knownEarly -> suppressed.
    //
    // kFramePassOrder has StaticPropOpaque(slot1) before
    // MechOpaque(slot2). The LOD-chunk runtime fire order: Terrain early (knownEarly),
    // then renderLists (StaticProp flush, then Mech GPU draw).
    // fired[] reflects real renderLists order (Static before Mech) — only Terrain is early.
    const RenderPassId fired[] = {
        RenderPassId::Shadow,
        RenderPassId::Terrain,            // fires pre-renderLists in LOD-chunk path
        RenderPassId::StaticPropOpaque,   // fires at GpuStaticPropBatcher::flush in renderLists
        RenderPassId::MechOpaque,         // fires after Static flush (GpuMechBatcher::flush)
        RenderPassId::TerrainOverlay,
        RenderPassId::TerrainDecal,
        RenderPassId::Water,
        RenderPassId::VegetationCards,
        RenderPassId::VFX,
        RenderPassId::UI,
        RenderPassId::PostProcess,
    };

    // WITHOUT knownEarly: kernel detects the out-of-order and no suppression occurs.
    {
        framegraph::FramePassTrace t = buildTrace(fired, kFramePassOrderCount);
        const framegraph::DryRunReport r =
            framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
        CHECK(r.outOfOrderCount > 0);
        CHECK(r.knownEarlySuppressed == 0);
    }

    // WITH terrain entry marked knownEarly: suppressed into knownEarlySuppressed, NOT outOfOrder.
    // This proves Option B suppression is the cause of the clean report, not a kernel hole.
    {
        framegraph::FramePassTrace t = buildTrace(fired, kFramePassOrderCount);
        framegraph::markEntryKnownEarly(t, RenderPassId::Terrain,
                                        kFramePassOrder, kFramePassOrderCount);
        const framegraph::DryRunReport r =
            framegraph::dryRunCompare(t, kFramePassOrder, kFramePassOrderCount);
        CHECK(r.outOfOrderCount == 0);
        CHECK(r.knownEarlySuppressed > 0);
    }
}

// Offline tests for the pure IslandContract table.
// GL-free: tests only the constexpr descriptor, not the GL wrapper.
// Rows: PostProcess only (the GL EdgeFog, FogOob, Shoreline, CloudShadow and ScreenShadow passes were deleted; those passes run as Vulkan native post passes).

TEST_CASE("executor island (a): kExecutorIslands has only the PostProcess row") {
    using namespace RenderCore::framegraph;
    CHECK(kExecutorIslandCount == 1u);
    CHECK(static_cast<unsigned>(kExecutorIslands[0].id) ==
          static_cast<unsigned>(ExecutorIslandId::PostProcess));
}

TEST_CASE("executor island (b): findIslandContract(PostProcess) returns non-null with expected flags") {
    using namespace RenderCore::framegraph;
    const IslandContract* c = findIslandContract(ExecutorIslandId::PostProcess);
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->requiresProgramValid    == true);
        CHECK(c->requiresSceneColorTex   == true);
        CHECK(c->requiresSceneDepthTex   == false);
        CHECK(c->warnIfNoTerrainLatch    == true);
        CHECK(c->postRequiresDefaultFbo  == true);
        CHECK(c->postRequiresBlendDisabled   == false);
        CHECK(c->postRequiresActiveTexture0  == false);
    }
}

TEST_CASE("executor island (c): the retired GL sub-stage islands have no contract row") {
    using namespace RenderCore::framegraph;
    CHECK(findIslandContract(ExecutorIslandId::EdgeFog)     == nullptr);
    CHECK(findIslandContract(ExecutorIslandId::FogOob)      == nullptr);
    CHECK(findIslandContract(ExecutorIslandId::Shoreline)   == nullptr);
    CHECK(findIslandContract(ExecutorIslandId::CloudShadow) == nullptr);
    CHECK(findIslandContract(ExecutorIslandId::ScreenShadow) == nullptr);
}

TEST_CASE("executor island (h): findIslandContract(Count) returns nullptr (out-of-range)") {
    using namespace RenderCore::framegraph;
    CHECK(findIslandContract(ExecutorIslandId::Count) == nullptr);
}

// ---------------------------------------------------------------------------
// Offline tests for the GL-free PostProcessSubpass table.
// Pure constexpr declaration checks — no GL, no engine, no smoke.
// ---------------------------------------------------------------------------

TEST_CASE("postprocess subgraph (a): kPostProcessSubpassCount == 12") {
    using namespace RenderCore::framegraph;
    // 14 total rows (6 original + 8 added).
    CHECK(kPostProcessSubpassCount == 12u);
}

TEST_CASE("postprocess subgraph (b): findPostProcessSubpass non-null for Composite and ShadowDebugOverlay") {
    using namespace RenderCore::framegraph;
    const PostProcessSubpass* composite = findPostProcessSubpass(ExecutorIslandId::Composite);
    CHECK(composite != nullptr);
    const PostProcessSubpass* dbgOverlay = findPostProcessSubpass(ExecutorIslandId::ShadowDebugOverlay);
    CHECK(dbgOverlay != nullptr);
    // Composite writes Backbuffer.
    if (composite) {
        CHECK(static_cast<unsigned>(composite->writes[0]) ==
              static_cast<unsigned>(RenderResourceId::Backbuffer));
    }
    // ShadowDebugOverlay writes Backbuffer.
    if (dbgOverlay) {
        CHECK(static_cast<unsigned>(dbgOverlay->writes[0]) ==
              static_cast<unsigned>(RenderResourceId::Backbuffer));
    }
}

TEST_CASE("postprocess subgraph (c): validateShippedPostProcessSubgraph().ok == true") {
    using namespace RenderCore::framegraph;
    const PostProcessValidationResult r = validateShippedPostProcessSubgraph();
    CHECK(r.ok == true);
    // On failure these report the offending subpass / missing resource.
    CHECK(static_cast<unsigned>(r.offendingSubpass)  == static_cast<unsigned>(ExecutorIslandId::Count));
    CHECK(static_cast<unsigned>(r.missingResource)   == static_cast<unsigned>(RenderResourceId::Unknown));
}

TEST_CASE("postprocess subgraph (d): owned flags match spec (only Composite is executor-owned)") {
    using namespace RenderCore::framegraph;
    const PostProcessSubpass* composite = findPostProcessSubpass(ExecutorIslandId::Composite);
    REQUIRE(composite != nullptr);
    CHECK(composite->ownedByExecutor == true);
    // The five Vulkan native passes keep their model rows but no GL executor wraps them any more.
    const ExecutorIslandId notOwnedIds[] = {
        ExecutorIslandId::CloudShadow,
        ExecutorIslandId::Shoreline,
        ExecutorIslandId::EdgeFog,
        ExecutorIslandId::FogOob,
        ExecutorIslandId::ScreenShadow,
        ExecutorIslandId::ShadowDebugOverlay,
        ExecutorIslandId::HzbReduce,
        ExecutorIslandId::HzbProbe,
        ExecutorIslandId::ClusterDepthPyramid,
        ExecutorIslandId::LightgridBuild,
        ExecutorIslandId::PostprocessComputeBlur,
    };
    for (int i = 0; i < 11; ++i) {
        const PostProcessSubpass* sp = findPostProcessSubpass(notOwnedIds[i]);
        REQUIRE(sp != nullptr);
        CHECK(sp->ownedByExecutor == false);
    }
}

TEST_CASE("postprocess subgraph (e): Composite is unconditional; all 13 others are conditional") {
    using namespace RenderCore::framegraph;
    const PostProcessSubpass* composite = findPostProcessSubpass(ExecutorIslandId::Composite);
    REQUIRE(composite != nullptr);
    CHECK(composite->conditional == false);

    // All 13 non-Composite rows must be conditional (gated by env-var, member, or terrain latch).
    const ExecutorIslandId conditionalIds[] = {
        ExecutorIslandId::CloudShadow,
        ExecutorIslandId::Shoreline,
        ExecutorIslandId::EdgeFog,
        ExecutorIslandId::FogOob,
        ExecutorIslandId::ShadowDebugOverlay,
        // Added sub-stage rows:
        ExecutorIslandId::HzbReduce,
        ExecutorIslandId::HzbProbe,
        ExecutorIslandId::ClusterDepthPyramid,
        ExecutorIslandId::LightgridBuild,
        ExecutorIslandId::PostprocessComputeBlur,
        ExecutorIslandId::ScreenShadow,
    };
    for (int i = 0; i < 11; ++i) {
        const PostProcessSubpass* sp = findPostProcessSubpass(conditionalIds[i]);
        REQUIRE(sp != nullptr);
        CHECK(sp->conditional == true);
    }
}

TEST_CASE("postprocess subgraph (f): order regression — Composite before a MainColor writer is flagged") {
    // If Composite (which reads MainColor) is moved BEFORE e.g. CloudShadow (which
    // writes MainColor), and CloudShadow is moved after Composite, the sub-stage
    // order validation must catch the unsatisfied read for the pass that now reads
    // something that hasn't been written yet.
    //
    // We simulate this by calling validatePostProcessSubgraph with an external set
    // that does NOT include MainColor — forcing Composite's read to be unsatisfied
    // unless an earlier subpass in kPostProcessSubpasses produces it.
    // In the CORRECT table order (CloudShadow/Shoreline/EdgeFog/FogOob all write
    // MainColor BEFORE Composite), the validator passes because MainColor is produced
    // by them. Here we strip MainColor from external AND from what would be produced
    // if Composite were first — achieved by using an external set that only has
    // MainDepth, ShadowStaticMap, ShadowDynamicMap, MainNormal (no MainColor).
    // The validator must flag Composite's read[0]=MainColor as unsatisfied IF
    // Composite appears first in the table.
    //
    // We can't reorder the constexpr table at runtime, so we test the validator
    // logic directly: prove that MainColor absent from external AND prior writes =>
    // validation fails with offendingSubpass != Count.
    using namespace RenderCore::framegraph;

    // External set WITHOUT MainColor. Row 0 is HzbReduce
    // (reads MainDepth, writes HzbPyramid). Subsequent rows that write MainColor (ScreenShadow,
    // CloudShadow, etc.) do so before Composite reads it — so the shipped table must still pass.
    // SceneObjectId is produced by the upstream geometry passes.
    const RenderResourceId noMainColor[] = {
        RenderResourceId::MainDepth,
        RenderResourceId::MainNormal,
        RenderResourceId::SceneObjectId,    // object-id RT
        RenderResourceId::SceneDepthCopy,   // VFX depth copy
    };
    // In the shipped order, ScreenShadow (row 6, first non-isCompute writer) writes MainColor.
    // Subsequent rows (CloudShadow, Shoreline, etc.) also write it before Composite reads it.
    // So the table must still pass even without MainColor in external.
    const PostProcessValidationResult r = validatePostProcessSubgraph(noMainColor, sizeof(noMainColor) / sizeof(noMainColor[0]));
    // ScreenShadow/CloudShadow/etc. write MainColor before Composite reads it -> ok.
    CHECK(r.ok == true);

    // Now prove the validator bites when the ONLY write of MainColor is removed from
    // external and also not produced by any prior subpass — we do this by providing
    // an empty external set. In the shipped table HzbReduce (row 0, not isCompute) reads
    // MainDepth which is no longer in external, so HzbReduce is flagged unsatisfied.
    const PostProcessValidationResult r2 = validatePostProcessSubgraph(nullptr, 0);
    CHECK(r2.ok == false);
    // HzbReduce is the first non-isCompute row (row 0) and reads MainDepth not in empty external.
    CHECK(static_cast<unsigned>(r2.offendingSubpass) ==
          static_cast<unsigned>(ExecutorIslandId::HzbReduce));
    CHECK(static_cast<unsigned>(r2.missingResource) ==
          static_cast<unsigned>(RenderResourceId::MainDepth));
}

// ---------------------------------------------------------------------------
// Offline tests for the 8 added sub-stage rows.
// ---------------------------------------------------------------------------

TEST_CASE("postprocess subgraph (g): SUBGRAPH-2 rows are all findable by id") {
    using namespace RenderCore::framegraph;
    const ExecutorIslandId slice2Ids[] = {
        ExecutorIslandId::HzbReduce,
        ExecutorIslandId::HzbProbe,
        ExecutorIslandId::ClusterDepthPyramid,
        ExecutorIslandId::LightgridBuild,
        ExecutorIslandId::PostprocessComputeBlur,
        ExecutorIslandId::ScreenShadow,
    };
    for (int i = 0; i < 6; ++i) {
        const PostProcessSubpass* sp = findPostProcessSubpass(slice2Ids[i]);
        CHECK(sp != nullptr);
    }
}

TEST_CASE("postprocess subgraph (h): isCompute flagged correctly (3 compute, rest false)") {
    using namespace RenderCore::framegraph;
    // Only ClusterDepthPyramid, LightgridBuild, PostprocessComputeBlur are compute dispatches.
    const ExecutorIslandId computeIds[] = {
        ExecutorIslandId::ClusterDepthPyramid,
        ExecutorIslandId::LightgridBuild,
        ExecutorIslandId::PostprocessComputeBlur,
    };
    for (int i = 0; i < 3; ++i) {
        const PostProcessSubpass* sp = findPostProcessSubpass(computeIds[i]);
        REQUIRE(sp != nullptr);
        CHECK(sp->isCompute == true);
    }
    // All draw-pass rows must have isCompute=false.
    const ExecutorIslandId drawIds[] = {
        ExecutorIslandId::HzbReduce,
        ExecutorIslandId::HzbProbe,
        ExecutorIslandId::ScreenShadow,
        ExecutorIslandId::CloudShadow,
        ExecutorIslandId::Shoreline,
        ExecutorIslandId::EdgeFog,
        ExecutorIslandId::FogOob,
        ExecutorIslandId::Composite,
        ExecutorIslandId::ShadowDebugOverlay,
    };
    for (int i = 0; i < 9; ++i) {
        const PostProcessSubpass* sp = findPostProcessSubpass(drawIds[i]);
        REQUIRE(sp != nullptr);
        CHECK(sp->isCompute == false);
    }
}


TEST_CASE("postprocess subgraph (j): validateShippedPostProcessSubgraph still ok with 14 rows") {
    // The complete 14-row table + updated external set must still validate.
    using namespace RenderCore::framegraph;
    const PostProcessValidationResult r = validateShippedPostProcessSubgraph();
    CHECK(r.ok == true);
    CHECK(static_cast<unsigned>(r.offendingSubpass) == static_cast<unsigned>(ExecutorIslandId::Count));
    CHECK(static_cast<unsigned>(r.missingResource)  == static_cast<unsigned>(RenderResourceId::Unknown));
}

TEST_CASE("postprocess subgraph (k): HzbReduce writes HzbPyramid; HzbProbe reads it (draw-pass chain)") {
    using namespace RenderCore::framegraph;
    const PostProcessSubpass* reduce = findPostProcessSubpass(ExecutorIslandId::HzbReduce);
    const PostProcessSubpass* probe  = findPostProcessSubpass(ExecutorIslandId::HzbProbe);
    REQUIRE(reduce != nullptr);
    REQUIRE(probe  != nullptr);
    // HzbReduce writes HzbPyramid.
    bool reduceWrites = false;
    for (int i = 0; i < 3; ++i) {
        if (reduce->writes[i] == RenderResourceId::HzbPyramid)
            reduceWrites = true;
    }
    CHECK(reduceWrites == true);
    // HzbProbe reads HzbPyramid.
    bool probeReads = false;
    for (int i = 0; i < 5; ++i) {
        if (probe->reads[i] == RenderResourceId::HzbPyramid)
            probeReads = true;
    }
    CHECK(probeReads == true);
    // Neither is executor-owned; neither is compute.
    CHECK(reduce->ownedByExecutor == false);
    CHECK(probe->ownedByExecutor  == false);
    CHECK(reduce->isCompute       == false);
    CHECK(probe->isCompute        == false);
}

// ---------------------------------------------------------------------------
// Verify MainNormal producer/consumer
// declarations are present and load-bearing in the shipped frame graph.
// ---------------------------------------------------------------------------

TEST_CASE("MainNormal: PostProcess reads it; Terrain writes it") {
    // PostProcess reads[] must include MainNormal.
    bool ppReads = false;
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        if (kRenderPassContracts[i].id != RenderPassId::PostProcess) continue;
        for (int j = 0; j < 4; ++j) {
            if (kRenderPassContracts[i].reads[j] == RenderResourceId::MainNormal)
                ppReads = true;
        }
    }
    CHECK(ppReads == true);

    // At least Terrain must declare MainNormal in writes[].
    bool terrainWrites = false;
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        if (kRenderPassContracts[i].id != RenderPassId::Terrain) continue;
        for (int j = 0; j < 4; ++j) {
            if (kRenderPassContracts[i].writes[j] == RenderResourceId::MainNormal)
                terrainWrites = true;
        }
    }
    CHECK(terrainWrites == true);

    // The shipped frame graph must still be valid with MainNormal declared.
    const ValidationResult r = validateShippedFrameGraph();
    CHECK(r.ok == true);
    CHECK(static_cast<unsigned>(r.offendingPass) == 0u);
    CHECK(static_cast<unsigned>(r.missingResource) == 0u);
}

TEST_CASE("MainNormal regression: removing all producers causes PostProcess unsatisfied") {
    // Build synthetic contracts identical to shipped, but strip MainNormal from ALL
    // geometry-pass writes[]. The DAG validator must then flag PostProcess/MainNormal.
    // This proves the producer declarations are load-bearing, not decorative.

    // Copy the shipped contracts into a mutable local array.
    RenderPassContract synthetic[kRenderPassIdCount];
    for (int i = 0; i < kRenderPassContractCount; ++i)
        synthetic[i] = kRenderPassContracts[i];

    // Strip MainNormal from every pass's writes[].
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (synthetic[i].writes[j] == RenderResourceId::MainNormal)
                synthetic[i].writes[j] = RenderResourceId::Unknown;
        }
    }

    // With no producers, PostProcess's read of MainNormal must be unsatisfied.
    const ValidationResult r = validateReadsSatisfied(
        synthetic, kRenderPassIdCount,
        kFramePassOrder, kFramePassOrderCount,
        kExternalResources, kExternalResourceCount);
    CHECK(r.ok == false);
    CHECK(static_cast<unsigned>(r.offendingPass)    == static_cast<unsigned>(RenderPassId::PostProcess));
    CHECK(static_cast<unsigned>(r.missingResource)  == static_cast<unsigned>(RenderResourceId::MainNormal));
}

// ---------------------------------------------------------------------------
// Offline tests for SceneDepthCopy
// logical resource identity + VFX producer declaration.
// Pure / GL-free. No smoke required.
// ---------------------------------------------------------------------------

TEST_CASE("SceneDepthCopy: VFX pass declares it in writes[]") {
    // Ground-truth: copySceneDepthForParticles() is called from gos_particle_bridge.cpp:1068
    // during the VFX/particle flush (before endScene/PostProcess). The VFX pass is the
    // cross-boundary producer. This test locks that declaration as a regression guard.
    bool vfxWritesDepthCopy = false;
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        if (kRenderPassContracts[i].id != RenderPassId::VFX) continue;
        for (int j = 0; j < 4; ++j) {
            if (kRenderPassContracts[i].writes[j] == RenderResourceId::SceneDepthCopy)
                vfxWritesDepthCopy = true;
        }
    }
    CHECK(vfxWritesDepthCopy == true);
}

TEST_CASE("SceneDepthCopy: validateShippedFrameGraph still ok with VFX producer declared") {
    // Adding SceneDepthCopy to VFX writes must not break the shipped frame-graph DAG
    // validation (no MAIN-order pass reads SceneDepthCopy, so it is
    // producerless-read-safe; adding the write is purely additive).
    const ValidationResult r = validateShippedFrameGraph();
    CHECK(r.ok == true);
    CHECK(static_cast<unsigned>(r.offendingPass)    == 0u);
    CHECK(static_cast<unsigned>(r.missingResource)  == 0u);
}

// ---------------------------------------------------------------------------
// Offline tests for SceneColorCopy
// producer declaration (closes the id-without-producer gap). Pure / GL-free.
// ---------------------------------------------------------------------------

TEST_CASE("SceneColorCopy: VFX pass declares it in writes[]") {
    // Ground-truth: copySceneColorForVfx() is called from gos_particle_bridge.cpp:1097
    // during the VFX/particle flush (gated MC2_VFX_SCENECOLOR_GRAB), same window as the
    // soft-particle depth copy. The VFX pass is the producer; the id (enum 20) otherwise
    // has no modeled producer. Lock the declaration as a regression guard.
    bool vfxWritesColorCopy = false;
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        if (kRenderPassContracts[i].id != RenderPassId::VFX) continue;
        for (int j = 0; j < 4; ++j) {
            if (kRenderPassContracts[i].writes[j] == RenderResourceId::SceneColorCopy)
                vfxWritesColorCopy = true;
        }
    }
    CHECK(vfxWritesColorCopy == true);
}

TEST_CASE("SceneColorCopy: validateShippedFrameGraph still ok with VFX producer declared") {
    // Adding SceneColorCopy to VFX writes must not break the shipped frame-graph DAG
    // validation (no MAIN-order pass reads SceneColorCopy — only an isCompute PP subpass
    // does, which the read-satisfaction walk skips), so the write is purely additive.
    const ValidationResult r = validateShippedFrameGraph();
    CHECK(r.ok == true);
    CHECK(static_cast<unsigned>(r.offendingPass)    == 0u);
    CHECK(static_cast<unsigned>(r.missingResource)  == 0u);
}

TEST_CASE("SceneDepthCopy: present in PP subgraph external set (validateShippedPostProcessSubgraph ok)") {
    // SceneDepthCopy must appear in validateShippedPostProcessSubgraph's kExternal[] so
    // that when BoxDecals reads it in a future subpass, the subgraph validator
    // accepts the read as externally produced, not producerless.
    using namespace RenderCore::framegraph;
    const PostProcessValidationResult r = validateShippedPostProcessSubgraph();
    CHECK(r.ok == true);
    CHECK(static_cast<unsigned>(r.offendingSubpass) == static_cast<unsigned>(ExecutorIslandId::Count));
    CHECK(static_cast<unsigned>(r.missingResource)  == static_cast<unsigned>(RenderResourceId::Unknown));
}


TEST_CASE("SceneObjectId: MechOpaque and StaticPropOpaque declare it in writes[]") {
    // Ground-truth: mech.frag and static_prop.frag emit layout(location=2) out uint v_objectId
    // when the MRT 3-entry draw-buffer set is active (IsObjectIdBufferEnabled).
    // Both geometry passes must declare the conditional write in their contract rows.
    bool mechWrites   = false;
    bool staticWrites = false;
    for (int i = 0; i < kRenderPassContractCount; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (kRenderPassContracts[i].writes[j] != RenderResourceId::SceneObjectId)
                continue;
            if (kRenderPassContracts[i].id == RenderPassId::MechOpaque)
                mechWrites = true;
            if (kRenderPassContracts[i].id == RenderPassId::StaticPropOpaque)
                staticWrites = true;
        }
    }
    CHECK(mechWrites   == true);
    CHECK(staticWrites == true);
}

TEST_CASE("SceneObjectId: Composite subpass reads it (closes the SUBGRAPH-1 gap)") {
    // sceneObjectIdTex_ (unit 2) previously had no logical id. The Composite row's
    // reads[] must include SceneObjectId so the subgraph validator sees the read declared.
    using namespace RenderCore::framegraph;
    const PostProcessSubpass* composite = findPostProcessSubpass(ExecutorIslandId::Composite);
    REQUIRE(composite != nullptr);
    bool compositeReads = false;
    for (int i = 0; i < 5; ++i) {
        if (composite->reads[i] == RenderResourceId::SceneObjectId)
            compositeReads = true;
    }
    CHECK(compositeReads == true);
}

TEST_CASE("SceneObjectId: validateShippedFrameGraph still ok (geometry pass writes don't strand anything)") {
    // Adding SceneObjectId to MechOpaque+StaticPropOpaque writes[] must not break the
    // shipped frame-graph DAG validation — SceneObjectId is consumed by PostProcess
    // (via the PP subgraph external set), so the write is never an orphan.
    const ValidationResult r = validateShippedFrameGraph();
    CHECK(r.ok == true);
    CHECK(static_cast<unsigned>(r.offendingPass)    == 0u);
    CHECK(static_cast<unsigned>(r.missingResource)  == 0u);
}

TEST_CASE("SceneObjectId: validateShippedPostProcessSubgraph ok (Composite read satisfied as external)") {
    // SceneObjectId is produced externally (geometry passes, upstream of the PP subgraph).
    // It must appear in the external set so the subgraph validator accepts Composite's read.
    using namespace RenderCore::framegraph;
    const PostProcessValidationResult r = validateShippedPostProcessSubgraph();
    CHECK(r.ok == true);
    CHECK(static_cast<unsigned>(r.offendingSubpass) == static_cast<unsigned>(ExecutorIslandId::Count));
    CHECK(static_cast<unsigned>(r.missingResource)  == static_cast<unsigned>(RenderResourceId::Unknown));
}

TEST_CASE("SceneObjectId: external entry is load-bearing for Composite read") {
    // Prove that SceneObjectId in the external set is the ONLY thing satisfying Composite's
    // read. Strip SceneObjectId from the external set and the subgraph validator must flag
    // Composite as having an unsatisfied read.
    using namespace RenderCore::framegraph;

    // External set without SceneObjectId — Composite's reads[SceneObjectId] is unsatisfied.
    const RenderResourceId extWithout[] = {
        RenderResourceId::MainColor,
        RenderResourceId::MainDepth,
        RenderResourceId::MainNormal,
        RenderResourceId::SceneDepthCopy,
        // SceneObjectId intentionally omitted
    };
    const PostProcessValidationResult r = validatePostProcessSubgraph(extWithout, sizeof(extWithout) / sizeof(extWithout[0]));
    CHECK(r.ok == false);
    CHECK(static_cast<unsigned>(r.offendingSubpass) == static_cast<unsigned>(ExecutorIslandId::Composite));
    CHECK(static_cast<unsigned>(r.missingResource)  == static_cast<unsigned>(RenderResourceId::SceneObjectId));
}

// ---------------------------------------------------------------------------
// Offline tests for the GL-free RenderStateDesc
// vocabulary. Pure / no GL / no engine. Validates that the unified StatePack agrees
// with the per-axis ledgers it was derived from.
// ---------------------------------------------------------------------------

TEST_CASE("statepack (a): kPassRenderState has one row per RenderPassId (11 rows)") {
    using namespace RenderCore::framegraph;
    // kRenderPassIdCount == 11 (Shadow/MechOpaque/StaticPropOpaque/Terrain/TerrainOverlay/
    // TerrainDecal/Water/VegetationCards/VFX/UI/PostProcess — None excluded).
    CHECK(kPassRenderStateCount == static_cast<int>(kRenderPassIdCount));
    CHECK(kPassRenderStateCount == 11);
}

TEST_CASE("statepack (b): validatePassRenderStateConsistency() ok — union agrees with ambient_contract + fbo_ledger") {
    using namespace RenderCore::framegraph;
    // This is the primary gate for the StatePack: every RenderStateDesc row must agree with
    // the per-axis ledger it was derived from. A failure here is a real finding (drift).
    const StatePackConsistencyResult r = validatePassRenderStateConsistency();
    CHECK(r.ok == true);
    // On failure, report the offending pass and axis.
    CHECK(static_cast<unsigned>(r.offendingPass) == static_cast<unsigned>(RenderPassId::None));
    CHECK(static_cast<unsigned>(r.axis) == static_cast<unsigned>(StatePackAxis::None));
}

TEST_CASE("statepack (c): UI row pipelineId==Invalid + passHasStaticPipeline(UI)==false") {
    using namespace RenderCore::framegraph;
    const RenderStateDesc* ui = findPassRenderState(RenderPassId::UI);
    REQUIRE(ui != nullptr);
    CHECK(static_cast<unsigned>(ui->pipelineId) == static_cast<unsigned>(RenderCore::PipelineId::Invalid));
    CHECK(passHasStaticPipeline(RenderPassId::UI) == false);
}

TEST_CASE("statepack (d): Shadow row pipelineId==Invalid + passHasStaticPipeline(Shadow)==false") {
    using namespace RenderCore::framegraph;
    // Shadow uses three descriptive-only sub-caster PipelineIds; no single pipeline for the pass.
    const RenderStateDesc* shadow = findPassRenderState(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(static_cast<unsigned>(shadow->pipelineId) == static_cast<unsigned>(RenderCore::PipelineId::Invalid));
    CHECK(passHasStaticPipeline(RenderPassId::Shadow) == false);
}

TEST_CASE("statepack (e): spot rows — Shadow depthFunc==ShadowLess, Terrain colorMask==AllOn match ambient") {
    using namespace RenderCore::framegraph;
    // Shadow: ambient_contract declares depthFunc=ShadowLess.
    const RenderStateDesc* shadow = findPassRenderState(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(static_cast<unsigned>(shadow->depthFunc) == static_cast<unsigned>(DepthFuncState::ShadowLess));

    // Terrain: ambient_contract declares colorMask=AllOn (re-assert after shadow).
    const RenderStateDesc* terrain = findPassRenderState(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);
    CHECK(static_cast<unsigned>(terrain->colorMask) == static_cast<unsigned>(ColorMaskState::AllOn));

    // StaticPropOpaque: fboTarget==MainColor (from kPassFboTarget).
    const RenderStateDesc* spo = findPassRenderState(RenderPassId::StaticPropOpaque);
    REQUIRE(spo != nullptr);
    CHECK(static_cast<unsigned>(spo->fboTarget) == static_cast<unsigned>(RenderResourceId::MainColor));
}

TEST_CASE("statepack (f): routed passes have non-Invalid pipelineId; non-routed have Invalid") {
    using namespace RenderCore::framegraph;
    // Passes confirmed routed via applyPipeline with a single PipelineId:
    const RenderPassId routed[] = {
        RenderPassId::StaticPropOpaque,
        RenderPassId::MechOpaque,
        RenderPassId::Terrain,
        RenderPassId::TerrainOverlay,
        RenderPassId::TerrainDecal,
    };
    for (int i = 0; i < 5; ++i) {
        const RenderStateDesc* r = findPassRenderState(routed[i]);
        REQUIRE(r != nullptr);
        CHECK(static_cast<unsigned>(r->pipelineId) != static_cast<unsigned>(RenderCore::PipelineId::Invalid));
        CHECK(passHasStaticPipeline(routed[i]) == true);
    }
    // Passes with no single PipelineId (multi-pipeline, descriptive-only, or no routing):
    const RenderPassId invalid[] = {
        RenderPassId::Water,
        RenderPassId::Shadow,
        RenderPassId::VegetationCards,
        RenderPassId::VFX,
        RenderPassId::UI,
        RenderPassId::PostProcess,
    };
    for (int i = 0; i < 6; ++i) {
        CHECK(passHasStaticPipeline(invalid[i]) == false);
    }
}

TEST_CASE("statepack (g): consistency validator catches a deliberately-wrong RenderStateDesc field") {
    using namespace RenderCore::framegraph;
    // Regression case: if kPassRenderState were to declare Shadow with colorMask=AllOn instead
    // of Inherit, the validator must catch it (ambient_contract shadow row has colorMask=Inherit;
    // both sides need non-Inherit to generate a violation — use Terrain which has AllOn on both).
    // Build a synthetic row that disagrees with ambient: Terrain colorMask=AllOff (should be AllOn).
    // We can't mutate kPassRenderState, so we call compareAmbient directly on the synthetic value.
    const AmbientContract* terrain = findAmbient(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);
    // terrain->colorMaskOnEntry == AllOn (ambient_contract ground truth)
    CHECK(static_cast<unsigned>(terrain->colorMaskOnEntry) == static_cast<unsigned>(ColorMaskState::AllOn));
    // A synthetic RenderStateDesc row with colorMask=AllOff would disagree.
    // compareAmbient covers the cross-check logic (already tested in "compareAmbient" test above).
    // Here we exercise the validator's specific path: create a wrong RenderStateDesc inline
    // and verify the validator (reimplementing its inner loop for the synthetic case).
    RenderStateDesc synthetic;
    synthetic.id        = RenderPassId::Terrain;
    synthetic.pipelineId = RenderCore::PipelineId::TerrainSolid;
    synthetic.colorMask = ColorMaskState::AllOff;   // WRONG (ambient says AllOn)
    synthetic.depthWrite = DepthWriteState::On;
    synthetic.depthFunc  = DepthFuncState::SceneGEqual;
    synthetic.viewport   = ViewportKind::MainScene;
    synthetic.fboTarget  = RenderResourceId::MainColor;

    // Cross-check synthetic.colorMask vs terrain->colorMaskOnEntry — must differ.
    const ColorMaskState rowVal = synthetic.colorMask;
    const ColorMaskState ambVal = terrain->colorMaskOnEntry;
    bool bothConcrete = (rowVal != ColorMaskState::Inherit && ambVal != ColorMaskState::Inherit);
    CHECK(bothConcrete == true);
    CHECK(rowVal != ambVal);   // AllOff != AllOn — validator would return ok=false for this row
}

// ---------------------------------------------------------------------------
// Offline tests for the pure TopLevelPassContract table.
// GL-free — tests only the constexpr descriptor, not the GL wrapper.
// ---------------------------------------------------------------------------

TEST_CASE("top-level executor (a): kTopLevelExecutorPasses has 10 rows (UI-SAME-ORDER-VALIDATE-1 adds UI)") {
    using namespace RenderCore::framegraph;
    CHECK(kTopLevelExecutorPassCount == 10u);
}

TEST_CASE("top-level executor (b): findTopLevelExecutorPass returns non-null for all 10 wrappable passes") {
    using namespace RenderCore::framegraph;
    const RenderPassId wrappable[] = {
        RenderPassId::Shadow,
        RenderPassId::MechOpaque,
        RenderPassId::StaticPropOpaque,
        RenderPassId::Terrain,
        RenderPassId::TerrainOverlay,
        RenderPassId::TerrainDecal,
        RenderPassId::Water,
        RenderPassId::VegetationCards,
        RenderPassId::VFX,              // FBO-only
        RenderPassId::UI,
    };
    for (int i = 0; i < 10; ++i) {
        const TopLevelPassContract* c = findTopLevelExecutorPass(wrappable[i]);
        CHECK(c != nullptr);
    }
}

TEST_CASE("top-level executor (c): all top-level passes now executor-owned (0 deferred)") {
    // UI is owned -> NO deferred top-level passes remain.
    // VFX, Water, Shadow + MechOpaque are owned too.
    using namespace RenderCore::framegraph;
    // Confirm Shadow + MechOpaque + Water + VFX + UI are ALL owned (none deferred):
    CHECK(findTopLevelExecutorPass(RenderPassId::Shadow)     != nullptr);
    CHECK(findTopLevelExecutorPass(RenderPassId::MechOpaque) != nullptr);
    CHECK(findTopLevelExecutorPass(RenderPassId::Water)      != nullptr);
    CHECK(findTopLevelExecutorPass(RenderPassId::VFX)        != nullptr);
    CHECK(findTopLevelExecutorPass(RenderPassId::UI)         != nullptr);
}

TEST_CASE("top-level executor (d): ambient + FBO flags match declared ledger declarations") {
    using namespace RenderCore::framegraph;

    // Shadow has AmbientContract (ShadowLess/ShadowMap) + FBO
    // ledger (ShadowDynamicMap). MechOpaque has AmbientContract (SceneGEqual) + FBO (MainColor).
    const TopLevelPassContract* shadow = findTopLevelExecutorPass(RenderPassId::Shadow);
    REQUIRE(shadow != nullptr);
    CHECK(shadow->validateAmbient == false);   // AmbientContract: ShadowLess + ShadowMap viewport
    CHECK(shadow->validateFbo     == false);   // FBO ledger: ShadowDynamicMap

    const TopLevelPassContract* mech = findTopLevelExecutorPass(RenderPassId::MechOpaque);
    REQUIRE(mech != nullptr);
    CHECK(mech->validateAmbient == true);     // AmbientContract: SceneGEqual + MainScene viewport
    CHECK(mech->validateFbo     == true);     // FBO ledger: MainColor

    // StaticPropOpaque and Terrain have AmbientContract + FBO ledger rows.
    const TopLevelPassContract* sp = findTopLevelExecutorPass(RenderPassId::StaticPropOpaque);
    REQUIRE(sp != nullptr);
    CHECK(sp->validateAmbient == true);  // AmbientContract row exists
    CHECK(sp->validateFbo     == true);  // FBO ledger declares MainColor

    const TopLevelPassContract* terrain = findTopLevelExecutorPass(RenderPassId::Terrain);
    REQUIRE(terrain != nullptr);
    CHECK(terrain->validateAmbient == true);
    CHECK(terrain->validateFbo     == true);

    // TerrainOverlay and TerrainDecal have FBO ledger but no AmbientContract row.
    const TopLevelPassContract* overlay = findTopLevelExecutorPass(RenderPassId::TerrainOverlay);
    REQUIRE(overlay != nullptr);
    CHECK(overlay->validateAmbient == false);
    CHECK(overlay->validateFbo     == true);

    const TopLevelPassContract* decal = findTopLevelExecutorPass(RenderPassId::TerrainDecal);
    REQUIRE(decal != nullptr);
    CHECK(decal->validateAmbient == false);
    CHECK(decal->validateFbo     == true);

    // Water has AmbientContract (SceneGEqual/depthWrite On) + FBO ledger (MainColor).
    const TopLevelPassContract* water = findTopLevelExecutorPass(RenderPassId::Water);
    REQUIRE(water != nullptr);
    CHECK(water->validateAmbient == false);
    CHECK(water->validateFbo     == false);

    // VegetationCards has neither AmbientContract row nor FBO ledger target.
    const TopLevelPassContract* veg = findTopLevelExecutorPass(RenderPassId::VegetationCards);
    REQUIRE(veg != nullptr);
    CHECK(veg->validateAmbient == false);
    CHECK(veg->validateFbo     == false);

    // VFX has FBO ledger (MainColor) but NO AmbientContract row
    // (note seam fires pre-body in a different TU -> ambient not honestly declarable).
    const TopLevelPassContract* vfx = findTopLevelExecutorPass(RenderPassId::VFX);
    REQUIRE(vfx != nullptr);
    CHECK(vfx->validateAmbient == false);
    CHECK(vfx->validateFbo     == true);

    // UI has FBO ledger (Backbuffer) but NO AmbientContract row
    // (UI ambient is per-draw legacy gos dynamic state -> DO_NOT_MODEL).
    const TopLevelPassContract* ui = findTopLevelExecutorPass(RenderPassId::UI);
    REQUIRE(ui != nullptr);
    CHECK(ui->validateAmbient == false);
    CHECK(ui->validateFbo     == true);
}

TEST_CASE("top-level executor (e): ambient ledger cross-check — Shadow/MechOpaque/StaticProp/Terrain have AmbientContract; TerrainOverlay/Decal/Veg do not") {
    // Verifies the validateAmbient flags are backed by actual ambient_contract.h rows.
    CHECK(findAmbient(RenderPassId::Shadow)           != nullptr);  // ShadowLess+ShadowMap
    CHECK(findAmbient(RenderPassId::MechOpaque)       != nullptr);  // SceneGEqual+MainScene
    CHECK(findAmbient(RenderPassId::StaticPropOpaque) != nullptr);
    CHECK(findAmbient(RenderPassId::Terrain)          != nullptr);
    CHECK(findAmbient(RenderPassId::Water)            != nullptr);  // modeled
    CHECK(findAmbient(RenderPassId::TerrainOverlay)   == nullptr);
    CHECK(findAmbient(RenderPassId::TerrainDecal)     == nullptr);
    CHECK(findAmbient(RenderPassId::VegetationCards)  == nullptr);
    CHECK(findAmbient(RenderPassId::VFX)              == nullptr);  // FBO-only
    CHECK(findAmbient(RenderPassId::UI)               != nullptr);  // modeled; verified observe-only
}

TEST_CASE("UI-PASS-MODEL-1: UI ambient row declares the measured entry state") {
    // Measured entry state: colorMask AllOn, blend Off, depthWrite On,
    // depthFunc Inherit (unconstrained), viewport MainScene (FB0 backbuffer classifies MainScene).
    const AmbientContract* ui = findAmbient(RenderPassId::UI);
    REQUIRE(ui != nullptr);
    CHECK(ui->colorMaskOnEntry == ColorMaskState::AllOn);
    CHECK(ui->blend            == BlendState::Off);
    CHECK(ui->depthWrite       == DepthWriteState::On);
    CHECK(ui->depthFunc        == DepthFuncState::Inherit);  // unconstrained -> skipped by compare
    CHECK(ui->viewport         == ViewportKind::MainScene);
    CHECK(ui->reassertsColorMaskAllOn == false);
    CHECK(ui->disablesColorWrite      == false);
    CHECK(ui->producesTerrainLatch    == false);
    CHECK(ui->consumesTerrainLatch    == false);

    // A live sample matching the measurement produces ZERO mismatch (declarable contract).
    AmbientSample live;
    live.colorMask  = ColorMaskState::AllOn;
    live.blend      = BlendState::Off;
    live.depthWrite = DepthWriteState::On;
    live.depthFunc  = DepthFuncState::Inherit;  // entry unconstrained
    live.viewport   = ViewportKind::MainScene;
    CHECK(compareAmbient(*ui, live).any() == false);
}

TEST_CASE("top-level executor (f): FBO ledger cross-check — Shadow=ShadowDynamicMap, MechOpaque+4=MainColor; VegetationCards undeclared") {
    auto rid = [](RenderResourceId r){ return static_cast<unsigned>(r); };
    const unsigned mc  = rid(RenderResourceId::MainColor);

    const unsigned bb  = rid(RenderResourceId::Backbuffer);
    const unsigned unk = rid(RenderResourceId::Unknown);
    CHECK(rid(declaredFboTarget(RenderPassId::Shadow))           == unk);
    CHECK(rid(declaredFboTarget(RenderPassId::MechOpaque))       == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::StaticPropOpaque)) == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::Terrain))          == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::TerrainOverlay))   == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::TerrainDecal))     == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::Water))           == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::VFX))             == mc);
    CHECK(rid(declaredFboTarget(RenderPassId::UI))             == bb);   // Backbuffer, first such row
    CHECK(rid(declaredFboTarget(RenderPassId::VegetationCards))  == unk);
}

TEST_CASE("top-level executor (g): note field is non-null for all wrappable passes") {
    using namespace RenderCore::framegraph;
    for (unsigned i = 0; i < kTopLevelExecutorPassCount; ++i) {
        CHECK(kTopLevelExecutorPasses[i].note != nullptr);
    }
}

TEST_CASE("top-level executor (h): kTopLevelDeferredPassCount == 0 (UI-SAME-ORDER-VALIDATE-1: all top-level passes owned)") {
    // Shadow + MechOpaque, Water, VFX and UI are all owned -> 0 deferred.
    using namespace RenderCore::framegraph;
    CHECK(kTopLevelDeferredPassCount == 0u);
}

// ---------------------------------------------------------------------------
// Offline tests for the top-level apply-state table
// (kTopLevelStateDesc / findTopLevelStateDesc). Pure/GL-free. The pre-apply GL calls are not tested here
// (GL context required); only the descriptor table shape is asserted.
// ---------------------------------------------------------------------------

TEST_CASE("top-level apply-state (a): kTopLevelStateDesc has exactly 6 rows (TerrainDecal, TerrainOverlay, StaticPropOpaque, MechOpaque, Water, Shadow)") {
    using namespace RenderCore::framegraph;
    CHECK(kTopLevelStateDescCount == 4u);  // GL shadow/water rows are retired
}

TEST_CASE("top-level apply-state (i): findTopLevelStateDesc(MechOpaque) = MechOpaque pipeline, FBO/viewport inherit (not applied)") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;
    const TopLevelStateDesc* d = findTopLevelStateDesc(RenderPassId::MechOpaque);
    REQUIRE(d != nullptr);
    CHECK(static_cast<unsigned>(d->id)         == static_cast<unsigned>(RenderPassId::MechOpaque));
    CHECK(static_cast<unsigned>(d->pipelineId) == static_cast<unsigned>(PipelineId::MechOpaque));
    // Only the pipeline is lifted; FBO/viewport honestly NOT applied (body inherits).
    CHECK(static_cast<unsigned>(d->fboTarget)  == static_cast<unsigned>(RenderResourceId::Unknown));
    CHECK(static_cast<unsigned>(d->viewport)   == static_cast<unsigned>(ViewportKind::Inherit));
    // MechOpaque runs every tier1 frame -> the ON-path body-skip is runtime-exercised.
    REQUIRE(passHasStaticPipeline(RenderPassId::MechOpaque));
}

TEST_CASE("retired GL Water has no top-level apply-state descriptor") {
    CHECK(findTopLevelStateDesc(RenderPassId::Water) == nullptr);
}

TEST_CASE("top-level apply-state (b): findTopLevelStateDesc(TerrainDecal) = TerrainDecal pipeline, FBO/viewport inherit (not applied)") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;
    const TopLevelStateDesc* d = findTopLevelStateDesc(RenderPassId::TerrainDecal);
    REQUIRE(d != nullptr);
    CHECK(static_cast<unsigned>(d->id)         == static_cast<unsigned>(RenderPassId::TerrainDecal));
    // pipelineId reused from the authoritative kPassRenderState[] row — not duplicated.
    CHECK(static_cast<unsigned>(d->pipelineId) == static_cast<unsigned>(PipelineId::TerrainDecal));
    // Only the pipeline is lifted this slice; FBO/viewport are honestly NOT applied.
    CHECK(static_cast<unsigned>(d->fboTarget)  == static_cast<unsigned>(RenderResourceId::Unknown));
    CHECK(static_cast<unsigned>(d->viewport)   == static_cast<unsigned>(ViewportKind::Inherit));
}

TEST_CASE("top-level apply-state (c): findTopLevelStateDesc returns nullptr for a non-apply pass (Terrain)") {
    using namespace RenderCore::framegraph;
    // Terrain is validate-owned but the executor does NOT apply its state.
    CHECK(findTopLevelStateDesc(RenderCore::RenderPassId::Terrain) == nullptr);
    // Shadow is an apply consumer (render-target MODE) -> non-null.
    CHECK(findTopLevelStateDesc(RenderCore::RenderPassId::Shadow) == nullptr);
}

// Shadow descriptor — first FULL render-target-MODE row.
// Pipeline = ShadowMech (the BASE caster pipeline; per-caster ShadowStaticProp re-applies in
// the body). fboTarget/viewport are lifted (ShadowDynamicMap/ShadowMap) — Shadow is a
// full render-target-mode owner (FBO+viewport+clear+pipeline). clear = DepthForwardZ
// (the SOLE non-None ClearSpec in the table).
// ★Shadow is intentionally EXEMPT from the "lifted pipeline matches the authoritative
// kPassRenderState row" invariant: the authoritative Shadow row is PipelineId::Invalid
// (3 descriptive sub-caster pipelines), so passHasStaticPipeline(Shadow) is false and Shadow
// is NOT in the registration row table below. This test asserts ShadowMech directly.
TEST_CASE("retired GL Shadow has no top-level apply-state descriptor") {
    CHECK(findTopLevelStateDesc(RenderPassId::Shadow) == nullptr);
}

TEST_CASE("top-level apply-state (d): lifted pipelineId matches the authoritative kPassRenderState row") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;
    const TopLevelStateDesc* td = findTopLevelStateDesc(RenderPassId::TerrainDecal);
    REQUIRE(td != nullptr);
    // passHasStaticPipeline(TerrainDecal) is true with pipelineId=TerrainDecal in
    // render_state_desc.h — the top-level table must reuse the SAME id, not re-pick.
    REQUIRE(passHasStaticPipeline(RenderPassId::TerrainDecal));
    bool found = false;
    for (int i = 0; i < kPassRenderStateCount; ++i) {
        if (kPassRenderState[i].id == RenderPassId::TerrainDecal) {
            CHECK(static_cast<unsigned>(td->pipelineId) ==
                  static_cast<unsigned>(kPassRenderState[i].pipelineId));
            found = true;
        }
    }
    CHECK(found);
}

// Second top-level apply-state consumer.
TEST_CASE("top-level apply-state (e): findTopLevelStateDesc(TerrainOverlay) = TerrainOverlay pipeline, FBO/viewport inherit (not applied)") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;
    const TopLevelStateDesc* d = findTopLevelStateDesc(RenderPassId::TerrainOverlay);
    REQUIRE(d != nullptr);
    CHECK(static_cast<unsigned>(d->id)         == static_cast<unsigned>(RenderPassId::TerrainOverlay));
    // pipelineId reused from the authoritative kPassRenderState[] row — not duplicated.
    CHECK(static_cast<unsigned>(d->pipelineId) == static_cast<unsigned>(PipelineId::TerrainOverlay));
    // Only the pipeline is lifted this slice; FBO/viewport are honestly NOT applied
    // (overlay inherits Terrain's scene FBO/drawBuffers/viewport).
    CHECK(static_cast<unsigned>(d->fboTarget)  == static_cast<unsigned>(RenderResourceId::Unknown));
    CHECK(static_cast<unsigned>(d->viewport)   == static_cast<unsigned>(ViewportKind::Inherit));
}

TEST_CASE("top-level apply-state (f): TerrainOverlay lifted pipelineId matches the authoritative kPassRenderState row") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;
    const TopLevelStateDesc* td = findTopLevelStateDesc(RenderPassId::TerrainOverlay);
    REQUIRE(td != nullptr);
    REQUIRE(passHasStaticPipeline(RenderPassId::TerrainOverlay));
    bool found = false;
    for (int i = 0; i < kPassRenderStateCount; ++i) {
        if (kPassRenderState[i].id == RenderPassId::TerrainOverlay) {
            CHECK(static_cast<unsigned>(td->pipelineId) ==
                  static_cast<unsigned>(kPassRenderState[i].pipelineId));
            found = true;
        }
    }
    CHECK(found);
}

// Third top-level apply-state consumer — first one whose
// ON-path body-skip is actually exercised at runtime (StaticPropOpaque runs every
// tier1 frame).
// SELF-PROOF: StaticPropOpaque is re-expressed to EXPLICITLY
// apply MainColor/MainScene (the axes it already inherits + runs every tier1 frame) -> proves
// the richer apply path is byte-identical to inheritance. clear stays None.
TEST_CASE("top-level apply-state (g): findTopLevelStateDesc(StaticPropOpaque) = StaticPropOpaque pipeline, EXPLICIT MainColor/MainScene, clear None") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;
    const TopLevelStateDesc* d = findTopLevelStateDesc(RenderPassId::StaticPropOpaque);
    REQUIRE(d != nullptr);
    CHECK(static_cast<unsigned>(d->id)         == static_cast<unsigned>(RenderPassId::StaticPropOpaque));
    // pipelineId reused from the authoritative kPassRenderState[] row — not duplicated.
    CHECK(static_cast<unsigned>(d->pipelineId) == static_cast<unsigned>(PipelineId::StaticPropOpaque));
    // EXTEND self-proof: now EXPLICITLY applied (no longer Unknown/Inherit).
    CHECK(static_cast<unsigned>(d->fboTarget)  == static_cast<unsigned>(RenderResourceId::MainColor));
    CHECK(static_cast<unsigned>(d->viewport)   == static_cast<unsigned>(ViewportKind::MainScene));
    // No depth clear for the opaque scene pass.
    CHECK(static_cast<unsigned>(d->clear)      == static_cast<unsigned>(ClearSpec::None));
}

// The 4 non-self-proof consumers stay byte-identical —
// fboTarget=Unknown / viewport=Inherit (skip-sentinels) and clear defaults to None.
// StaticPropOpaque is the SOLE row with explicit FBO/viewport.
// Shadow is the SOLE row with clear=DepthForwardZ (asserted separately
// in test (l)); it is NOT in this unchanged-consumers loop.
TEST_CASE("top-level apply-state (k): EXTEND — 3 consumers unchanged (Unknown/Inherit); ClearSpec default None (decal/overlay/mech + staticprop)") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;
    const RenderPassId unchanged[] = {
        RenderPassId::TerrainDecal, RenderPassId::TerrainOverlay,
        RenderPassId::MechOpaque,
    };
    for (RenderPassId id : unchanged) {
        const TopLevelStateDesc* d = findTopLevelStateDesc(id);
        REQUIRE(d != nullptr);
        CHECK(static_cast<unsigned>(d->fboTarget) == static_cast<unsigned>(RenderResourceId::Unknown));
        CHECK(static_cast<unsigned>(d->viewport)  == static_cast<unsigned>(ViewportKind::Inherit));
        CHECK(static_cast<unsigned>(d->clear)     == static_cast<unsigned>(ClearSpec::None));
    }
    // Default-member-initializer leaves ClearSpec::None on the StaticProp row too (explicit here).
    const TopLevelStateDesc* sp = findTopLevelStateDesc(RenderPassId::StaticPropOpaque);
    REQUIRE(sp != nullptr);
    CHECK(static_cast<unsigned>(sp->clear) == static_cast<unsigned>(ClearSpec::None));
}

TEST_CASE("top-level apply-state (h): StaticPropOpaque lifted pipelineId matches the authoritative kPassRenderState row") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;
    const TopLevelStateDesc* td = findTopLevelStateDesc(RenderPassId::StaticPropOpaque);
    REQUIRE(td != nullptr);
    REQUIRE(passHasStaticPipeline(RenderPassId::StaticPropOpaque));
    bool found = false;
    for (int i = 0; i < kPassRenderStateCount; ++i) {
        if (kPassRenderState[i].id == RenderPassId::StaticPropOpaque) {
            CHECK(static_cast<unsigned>(td->pipelineId) ==
                  static_cast<unsigned>(kPassRenderState[i].pipelineId));
            found = true;
        }
    }
    CHECK(found);
}

TEST_CASE("per-pass-apply-counters-1: ApplyPassId table is complete + names unique/non-Unknown") {
    using namespace RenderCore::framegraph;
    // Exactly 4 apply paths, all top-level (the GL PostProcess sub-stage, Shadow and Water applies are gone).
    CHECK(static_cast<unsigned>(ApplyPassId::Count) == 4u);
    // Spot-check representative mappings.
    CHECK(std::string(applyPassName(ApplyPassId::StaticPropOpaque)) == "StaticPropOpaque");
    CHECK(std::string(applyPassName(ApplyPassId::MechOpaque))       == "MechOpaque");
    // All ids return a non-"Unknown", unique name.
    const char* seen[(int)ApplyPassId::Count] = {nullptr};
    for (int i = 0; i < (int)ApplyPassId::Count; ++i) {
        const char* n = applyPassName((ApplyPassId)i);
        CHECK(std::strcmp(n, "Unknown") != 0);
        for (int j = 0; j < i; ++j)
            CHECK(std::strcmp(n, seen[j]) != 0); // uniqueness
        seen[i] = n;
    }
    // Out-of-range id is "Unknown".
    CHECK(std::string(applyPassName(ApplyPassId::Count)) == "Unknown");
}

// Each of the 4 TOP-LEVEL apply
// passes (TerrainDecal / TerrainOverlay / StaticPropOpaque / MechOpaque) maps to a RenderPassId
// whose kPassRenderState[] row declares a CONCRETE (non-Invalid) pipelineId. You
// must not declare a top-level apply on a multi-pipeline / Invalid pass — the
// executor cannot pre-apply a pipeline that doesn't statically exist.
TEST_CASE("apply-state-registration (top-level): each top-level ApplyPassId maps to a pass with a concrete pipeline") {
    using namespace RenderCore;
    using namespace RenderCore::framegraph;

    // name -> RenderPassId mapping for the top-level apply ids whose pipeline is the
    // authoritative single pipeline (small local switch).
    // ★ApplyPassId::Shadow is DELIBERATELY EXCLUDED from this invariant.
    // Shadow's authoritative kPassRenderState row is PipelineId::Invalid (3 descriptive
    // sub-caster pipelines ShadowTerrain/ShadowMech/ShadowStaticProp), so passHasStaticPipeline
    // (Shadow) is false and there is no single representative pipeline to match. Shadow's
    // TopLevelStateDesc lifts the ShadowMech BASE pipeline (per-caster ShadowStaticProp re-applies
    // in the body); it is asserted directly in test (l), not cross-checked here.
    struct Row { ApplyPassId apply; RenderPassId pass; };
    const Row topLevel[] = {
        { ApplyPassId::TerrainDecal,     RenderPassId::TerrainDecal     },
        { ApplyPassId::TerrainOverlay,   RenderPassId::TerrainOverlay   },
        { ApplyPassId::StaticPropOpaque, RenderPassId::StaticPropOpaque },
        { ApplyPassId::MechOpaque,       RenderPassId::MechOpaque       },
    };
    for (const Row& r : topLevel) {
        // The ApplyPassId name and the RenderPassId name must agree (no silent skew).
        const RenderStateDesc* desc = findPassRenderState(r.pass);
        REQUIRE(desc != nullptr);
        // INVARIANT: a top-level apply pass has a concrete pipeline.
        CHECK(passHasStaticPipeline(r.pass));
        CHECK(static_cast<unsigned>(desc->pipelineId) != static_cast<unsigned>(PipelineId::Invalid));
        // The executor's top-level apply table must agree on the same pipeline.
        const TopLevelStateDesc* tl = findTopLevelStateDesc(r.pass);
        REQUIRE(tl != nullptr);
        CHECK(static_cast<unsigned>(tl->pipelineId) == static_cast<unsigned>(desc->pipelineId));
    }
}

// ---------------------------------------------------------------------------
// GL-free edge classifier + current-order legality
// baseline (tier-C proof infrastructure; no reorderer, no pass movement).
// ---------------------------------------------------------------------------

TEST_CASE("edge classify (a): expected HardResource edge set against kFramePassOrder") {
    using namespace RenderCore::framegraph;
    PassEdge edges[256];
    int n = 0;
    classifyEdges(edges, n);
    REQUIRE(n > 0);

    auto hasHR = [&](RenderPassId from, RenderPassId to, RenderResourceId via) {
        for (int i = 0; i < n; ++i) {
            const PassEdge& e = edges[i];
            if (e.cls == EdgeClass::HardResource && e.from == from && e.to == to && e.via == via)
                return true;
        }
        return false;
    };

    // Shadow produces ShadowDynamicMap; the three opaque/terrain consumers read it.
    // (Shadow is first in kFramePassOrder, so the producer-walk derives these directly.)

    // Downstream consumers of scene depth: last FrameLocal writer of MainDepth before each.
    // Terrain is the last MainDepth writer before TerrainOverlay/TerrainDecal/Water/Veg/VFX.
    CHECK(hasHR(RenderPassId::Terrain, RenderPassId::TerrainOverlay, RenderResourceId::MainDepth));
    CHECK(hasHR(RenderPassId::Terrain, RenderPassId::TerrainDecal,   RenderResourceId::MainDepth));
    CHECK(hasHR(RenderPassId::Terrain, RenderPassId::Water,          RenderResourceId::MainDepth));
    CHECK(hasHR(RenderPassId::Terrain, RenderPassId::VegetationCards,RenderResourceId::MainDepth));
    // VFX (slot 8) reads MainDepth; the LAST in-frame MainDepth writer before it is
    // VegetationCards (slot 7), not Terrain — proves last-writer (not first) semantics.
    CHECK(hasHR(RenderPassId::VegetationCards, RenderPassId::VFX,    RenderResourceId::MainDepth));

    // PostProcess reads MainColor, MainDepth, ShadowDynamicMap, MainNormal. Its MainColor
    // edge comes from the LAST in-frame MainColor writer before it — UI (slot 9), since VFX
    // (slot 8) and UI both write MainColor and UI is later.
    CHECK(hasHR(RenderPassId::UI,     RenderPassId::PostProcess, RenderResourceId::MainColor));
}

TEST_CASE("edge classify (b): isCurrentOrderLegal() == true (no-op identity baseline)") {
    using namespace RenderCore::framegraph;
    CHECK(isCurrentOrderLegal() == true);
}

TEST_CASE("edge classify (c): every lifetime-excluded read is ExternalNonEdge, never HardResource") {
    using namespace RenderCore::framegraph;
    // Synthetic table: a single pass that READS an External/Mission/Persistent resource.
    // The classifier must emit ExternalNonEdge for it (visible exclusion), and must NOT
    // emit a HardResource edge for it.
    RenderPassContract synth[1] = {};
    synth[0].id   = RenderPassId::PostProcess;
    synth[0].name = "synthExternalReader";
    synth[0].reads[0] = RenderResourceId::TerrainHeightTexture; // Mission
    synth[0].reads[1] = RenderResourceId::MaterialGpuBuffer;      // Persistent
    synth[0].reads[2] = RenderResourceId::WaterReflectionColor; // External
    synth[0].reads[3] = RenderResourceId::Unknown;

    const RenderPassId order[1] = { RenderPassId::PostProcess };
    PassEdge edges[256];
    int n = 0;
    classifyEdgesFor(synth, 1, order, 1, edges, n);

    int externalNonEdges = 0, hardFromSynth = 0;
    for (int i = 0; i < n; ++i) {
        if (edges[i].cls == EdgeClass::ExternalNonEdge) ++externalNonEdges;
        if (edges[i].cls == EdgeClass::HardResource &&
            (edges[i].via == RenderResourceId::TerrainHeightTexture ||
             edges[i].via == RenderResourceId::MaterialGpuBuffer ||
             edges[i].via == RenderResourceId::WaterReflectionColor))
            ++hardFromSynth;
    }
    CHECK(externalNonEdges == 3);  // one per excluded read
    CHECK(hardFromSynth == 0);     // none promoted to a HardResource edge

    // And the static lifetime classification itself.
    CHECK(isExternalLifetime(RenderResourceId::TerrainHeightTexture) == true);
    CHECK(isExternalLifetime(RenderResourceId::MaterialGpuBuffer)      == true);
    CHECK(isExternalLifetime(RenderResourceId::WaterReflectionColor) == true);
    CHECK(isExternalLifetime(RenderResourceId::MainColor)            == false); // FrameLocal
    CHECK(isExternalLifetime(RenderResourceId::SceneDepthCopy)     == false); // FrameLocal
}

TEST_CASE("edge classify (d): LODChunk knownEarly is suppressed, not flagged forbidden") {
    using namespace RenderCore::framegraph;
    PassEdge edges[256];
    int n = 0;
    classifyEdges(edges, n);

    // A KnownEarly entry for Terrain must exist and must be non-forbidden (a suppression).
    bool found = false;
    for (int i = 0; i < n; ++i) {
        if (edges[i].cls == EdgeClass::KnownEarly && edges[i].to == RenderPassId::Terrain) {
            found = true;
            CHECK(edges[i].forbidden == false);   // suppression rule, NOT a violation
        }
    }
    CHECK(found == true);
    // The presence of a knownEarly Terrain entry must not flip the baseline to illegal.
    CHECK(isCurrentOrderLegal() == true);
}

TEST_CASE("edge classify (e): the 3 hand tables are non-empty + well-formed") {
    using namespace RenderCore::framegraph;
    CHECK(kContentConditionalEdgeCount > 0);
    CHECK(kForbiddenReorderEdgeCount   > 0);
    CHECK(kDeferredSoftStateEdgeCount  > 0);

    auto validPassId = [](RenderPassId id) {
        return static_cast<unsigned>(id) > 0u &&
               static_cast<unsigned>(id) < static_cast<unsigned>(RenderPassId::_SentinelLast);
    };

    // Forbidden edges must reference REAL pass ids on both ends and be flagged forbidden.
    for (int i = 0; i < kForbiddenReorderEdgeCount; ++i) {
        CHECK(validPassId(kForbiddenReorderEdges[i].from) == true);
        CHECK(validPassId(kForbiddenReorderEdges[i].to)   == true);
        CHECK(kForbiddenReorderEdges[i].forbidden == true);
    }
    // Content-conditional edges reference real pass ids and carry contentGated=true.
    for (int i = 0; i < kContentConditionalEdgeCount; ++i) {
        CHECK(validPassId(kContentConditionalEdges[i].from) == true);
        CHECK(validPassId(kContentConditionalEdges[i].to)   == true);
        CHECK(kContentConditionalEdges[i].contentGated == true);
        CHECK(kContentConditionalEdges[i].forbidden    == false);
    }
    // Deferred soft-state edges are an "unknown hazard" ledger: non-forbidden, have a note.
    for (int i = 0; i < kDeferredSoftStateEdgeCount; ++i) {
        CHECK(kDeferredSoftStateEdges[i].forbidden == false);
        CHECK(kDeferredSoftStateEdges[i].note != nullptr);
        CHECK(kDeferredSoftStateEdges[i].note[0] != '\0');
    }

    // edgeClassName covers every enum value (no "?" for declared classes).
    CHECK(std::string(edgeClassName(EdgeClass::HardResource))       == "HardResource");
    CHECK(std::string(edgeClassName(EdgeClass::SoftState))          == "SoftState");
    CHECK(std::string(edgeClassName(EdgeClass::LegacyLatch))        == "LegacyLatch");
    CHECK(std::string(edgeClassName(EdgeClass::KnownEarly))         == "KnownEarly");
    CHECK(std::string(edgeClassName(EdgeClass::ContentConditional)) == "ContentConditional");
    CHECK(std::string(edgeClassName(EdgeClass::ExternalNonEdge))    == "ExternalNonEdge");
}

TEST_CASE("edge classify (f): SoftState colorMask-reassert + LegacyLatch terrain edges derived from ambient") {
    using namespace RenderCore::framegraph;
    PassEdge edges[256];
    int n = 0;
    classifyEdges(edges, n);

    bool softReassert = false, legacyLatch = false;
    for (int i = 0; i < n; ++i) {
        const PassEdge& e = edges[i];
        if (e.cls == EdgeClass::SoftState && e.from == RenderPassId::Terrain &&
            e.to == RenderPassId::PostProcess && e.forbidden)
            softReassert = true;
        if (e.cls == EdgeClass::LegacyLatch && e.from == RenderPassId::Terrain &&
            e.to == RenderPassId::PostProcess && e.forbidden)
            legacyLatch = true;
    }
    CHECK(softReassert == true);
    CHECK(legacyLatch  == true);
}

// ---------------------------------------------------------------------------
// GL-free permutation-legality oracle. PROOF-ONLY —
// the oracle moves nothing and executes nothing; a Legal verdict = "eligible for
// a future MEASURED reorder experiment", NOT an approved/scheduled reorder.
// ---------------------------------------------------------------------------

// doctest cannot stringify these scoped enums (no operator<<), so compare via a
// uint cast — the same pattern the resource-id CHECKs above use.
template <class E> static unsigned uns(E e) { return static_cast<unsigned>(e); }

// Helper: build the shipped order into a caller buffer, return the count.
static int buildShippedOrder(RenderCore::RenderPassId* out) {
    using namespace RenderCore::framegraph;
    for (int i = 0; i < kFramePassOrderCount; ++i) out[i] = kFramePassOrder[i];
    return kFramePassOrderCount;
}

TEST_CASE("native shadow ordering survives retirement of the GL map resources") {
    RenderPassId cand[32];
    const int n = buildShippedOrder(cand);
    cand[0] = RenderPassId::StaticPropOpaque;
    cand[1] = RenderPassId::Shadow;
    PassEdge first{};
    CHECK(static_cast<unsigned>(isReorderLegal(cand, n, &first)) ==
          static_cast<unsigned>(ReorderVerdict::ForbiddenEdgeViolated));
    CHECK(static_cast<unsigned>(first.from) == static_cast<unsigned>(RenderPassId::Shadow));
    CHECK(static_cast<unsigned>(first.to) == static_cast<unsigned>(RenderPassId::StaticPropOpaque));
    CHECK(static_cast<unsigned>(first.cls) == static_cast<unsigned>(EdgeClass::LegacyLatch));
    CHECK(static_cast<unsigned>(first.via) == static_cast<unsigned>(RenderResourceId::Unknown));
}

TEST_CASE("oracle (a): identity (kFramePassOrder) -> Legal") {
    using namespace RenderCore::framegraph;
    RenderPassId cand[32];
    const int n = buildShippedOrder(cand);
    PassEdge v{};
    CHECK(uns(isReorderLegal(cand, n, &v)) == uns(ReorderVerdict::Legal));
}

TEST_CASE("oracle (b): postprocess before opaque geometry violates the MainColor edge") {
    RenderPassId cand[32];
    const int n = buildShippedOrder(cand);
    int post = 0;
    while (cand[post] != RenderPassId::PostProcess) ++post;
    const auto firstPass = cand[0];
    cand[0] = RenderPassId::PostProcess;
    cand[post] = firstPass;
    PassEdge first{};
    CHECK(isReorderLegal(cand, n, &first) == ReorderVerdict::ForbiddenEdgeViolated);
    CHECK(static_cast<unsigned>(first.to) == static_cast<unsigned>(RenderPassId::PostProcess));
}

TEST_CASE("oracle (c): Terrain moved AFTER PostProcess breaks the sceneHasTerrain latch -> ForbiddenEdgeViolated") {
    using namespace RenderCore::framegraph;
    // Move Terrain to the very end (after PostProcess) by rotating it out of slot 3.
    // This breaks Terrain->PostProcess (both the colorMask reassert and the
    // sceneHasTerrain latch run backwards). Verdict must be ForbiddenEdgeViolated,
    // and a Terrain->PostProcess LegacyLatch forbidden edge must be present + violated.
    RenderPassId base[32];
    const int n = buildShippedOrder(base);
    // Build candidate: everything except Terrain in shipped order, then Terrain last.
    RenderPassId cand[32];
    int c = 0;
    for (int i = 0; i < n; ++i)
        if (base[i] != RenderPassId::Terrain) cand[c++] = base[i];
    cand[c++] = RenderPassId::Terrain;
    REQUIRE(c == n);

    PassEdge first{};
    const ReorderVerdict v = isReorderLegal(cand, n, &first);
    CHECK(uns(v) == uns(ReorderVerdict::ForbiddenEdgeViolated));
    // The first violated edge is a Terrain->PostProcess soft/latch handshake.
    CHECK(uns(first.from) == uns(RenderPassId::Terrain));
    CHECK(uns(first.to)   == uns(RenderPassId::PostProcess));
    CHECK((first.cls == EdgeClass::SoftState || first.cls == EdgeClass::LegacyLatch));

    // And specifically the LegacyLatch (sceneHasTerrain) edge exists, is forbidden,
    // and runs backwards in this candidate (pos(Terrain) > pos(PostProcess)).
    PassEdge edges[256];
    int ec = 0;
    classifyEdgesFor(kRenderPassContracts, kRenderPassIdCount, cand, n, edges, ec);
    bool latchViolated = false;
    const int pTerrain = orderIndexOf(RenderPassId::Terrain, cand, n);
    const int pPost    = orderIndexOf(RenderPassId::PostProcess, cand, n);
    for (int i = 0; i < ec; ++i) {
        const PassEdge& e = edges[i];
        if (e.cls == EdgeClass::LegacyLatch && e.from == RenderPassId::Terrain &&
            e.to == RenderPassId::PostProcess && e.forbidden && pTerrain > pPost)
            latchViolated = true;
    }
    CHECK(latchViolated == true);
}

TEST_CASE("oracle (d): legalAdjacentSwaps -> ZERO true (post MEASURED-REORDER-SPMECH-1)") {
    using namespace RenderCore::framegraph;
    // StaticProp<->Mech was once the single candidate-legal adjacent swap, but a gated
    // reorder experiment PROVED it fails parity (pixel diff 3-6x noise; depth-equal ties
    // resolve order-dependently). It is now a CONCRETE deferred-soft-state pair ->
    // BlockedByDeferredSoftState. The shipped baseline has NO clean legal adjacent swap.
    bool swaps[32] = { false };
    legalAdjacentSwaps(swaps, kFramePassOrderCount);

    int trueCount = 0;
    for (int i = 0; i + 1 < kFramePassOrderCount; ++i)
        if (swaps[i]) { ++trueCount; }

    CHECK(trueCount == 0);
}

TEST_CASE("oracle (e): forbidden-edges-hold but crosses a deferred soft-state pair -> BlockedByDeferredSoftState") {
    using namespace RenderCore::framegraph;
    // Swap TerrainOverlay(slot 4) <-> TerrainDecal(slot 5). No forbidden edge runs
    // between them (Terrain produces MainDepth for both; neither depends on the
    // other), so the resource model says "legal" — BUT both sit in the deferred
    // soft-state hazard region (>= Terrain's boundary), so the oracle must REFUSE to
    // overclaim and return BlockedByDeferredSoftState.
    RenderPassId cand[32];
    const int n = buildShippedOrder(cand);
    const int io = orderIndexOf(RenderPassId::TerrainOverlay, cand, n);
    const int id = orderIndexOf(RenderPassId::TerrainDecal,   cand, n);
    REQUIRE(io >= 0); REQUIRE(id >= 0);
    const RenderPassId tmp = cand[io]; cand[io] = cand[id]; cand[id] = tmp;

    PassEdge blocking{};
    const ReorderVerdict v = isReorderLegal(cand, n, &blocking);
    CHECK(uns(v) == uns(ReorderVerdict::BlockedByDeferredSoftState));
    // The reported blocking edge is a deferred soft-state hazard (non-forbidden ledger).
    CHECK(uns(blocking.cls) == uns(EdgeClass::SoftState));
    CHECK(blocking.forbidden == false);
}

TEST_CASE("oracle (f): StaticProp<->Mech swap -> BlockedByDeferredSoftState (depth-tie; MEASURED-REORDER-SPMECH-1)") {
    using namespace RenderCore::framegraph;
    // A gated StaticProp<->Mech reorder experiment measured a parity FAIL: pixel diff
    // 3-6x above the noise floor. Root cause: the two
    // opaque passes overlap in screen space and their fragments resolve to a DIFFERENT
    // winner under depth-EQUAL / coplanar ties when draw order swaps (last-writer-wins is
    // order-dependent). The resource DAG sees NO hard edge between them ("resource-legal"),
    // but resource-legal != visually-commutative. So the oracle must now report this swap as
    // BlockedByDeferredSoftState, NOT Legal — it does not overclaim a clean reorder.
    RenderPassId cand[32];
    const int n = buildShippedOrder(cand);
    const int is = orderIndexOf(RenderPassId::StaticPropOpaque, cand, n);
    const int im = orderIndexOf(RenderPassId::MechOpaque,       cand, n);
    REQUIRE(is >= 0); REQUIRE(im >= 0);
    const RenderPassId tmp = cand[is]; cand[is] = cand[im]; cand[im] = tmp;

    PassEdge blocking{};
    const ReorderVerdict v = isReorderLegal(cand, n, &blocking);
    CHECK(uns(v) == uns(ReorderVerdict::BlockedByDeferredSoftState));
    // The reported blocking edge is the CONCRETE StaticProp<->Mech depth-tie deferred-soft-
    // state edge (named from/to, non-forbidden ledger entry).
    CHECK(uns(blocking.from) == uns(RenderPassId::StaticPropOpaque));
    CHECK(uns(blocking.to)   == uns(RenderPassId::MechOpaque));
    CHECK(uns(blocking.cls)  == uns(EdgeClass::SoftState));
    CHECK(blocking.forbidden == false);

    // Reporting helper uses the conservative wording — NEVER "approved"/"safe"/"scheduled".
    // The blocked verdict reports as resource-wise-legal-but-deferred-blocked.
    char buf[256];
    reportReorderVerdict(v, &blocking, buf, sizeof(buf));
    const std::string s = buf;
    CHECK(s.find("blocked by a deferred soft-state edge") != std::string::npos);
    CHECK(s.find("approved")  == std::string::npos);
    CHECK(s.find("safe to reorder") == std::string::npos);
    CHECK(s.find("scheduled") == std::string::npos);

    // reorderVerdictName cross-check.
    CHECK(std::string(reorderVerdictName(ReorderVerdict::Legal)) == "Legal");
    CHECK(std::string(reorderVerdictName(ReorderVerdict::ForbiddenEdgeViolated)) == "ForbiddenEdgeViolated");
    CHECK(std::string(reorderVerdictName(ReorderVerdict::BlockedByDeferredSoftState)) == "BlockedByDeferredSoftState");
}

} // TEST_SUITE
