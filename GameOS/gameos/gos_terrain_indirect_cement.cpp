// gos_terrain_indirect: cement and concrete classification, transition masks and PopulateRecipeCementWords.
// Declared in the class headers; shared names in gos_terrain_indirect_internal.h.
#include "gos_terrain_indirect_internal.h"
#include "mc2fmt.h"


// ---------------------------------------------------------------------------
// Colormap atlas — single GL texture covering the full merged colormap.
//
// cpuColorMap (terrtxm2.h:93) already holds the entire RGBA atlas in CPU
// memory, retained at mission load.  We upload it once as a plain
// GL_TEXTURE_2D so gos_terrain_bridge_drawIndirect can bind it at unit 0
// instead of per-bucket tile binds.
//
// Atlas UV formula (mirrors terrtxm2.cpp:resolveTextureHandle):
//   posX  = (worldX - mapTopLeft3d.x) * oneOverWorldUnitsMapSide
//   posY  = (mapTopLeft3d.y - worldY) * oneOverWorldUnitsMapSide
//   tileX = floor((posX + 0.0005) * numTexturesAcross)
//   tileY = floor((posY + 0.0005) * numTexturesAcross)
//   atlasUV = (vec2(tileX, tileY) + perTileUV) / numTexturesAcross
//
// The per-tile UV (from recipe.uvData) is already in [0,1] within a tile.
// Dividing by numTexturesAcross converts tile-local to atlas-absolute.
// ---------------------------------------------------------------------------

namespace gos_terrain_indirect_detail {

GLuint  g_atlasGLTex              = 0;
int     g_atlasSize               = 0;
float   g_atlasNumTexturesAcross  = 0.f;
float   g_atlasMapTopLeftX        = 0.f;
float   g_atlasMapTopLeftY        = 0.f;
float   g_atlasOneOverWorldUnits  = 0.f;

// ---------------------------------------------------------------------------
// Cement catalog atlas — single GL_TEXTURE_2D, packed grid of N cement tile
// textures.  Built once per mission at BuildDenseRecipe() time via GPU
// readback from already-resident catalog textures (textureData[0] in
// tileRAMHeap is dead in stock gameplay — quickLoad gates the RAM path at
// terrtxm.cpp:561).  Bound at unit 3 by gos_terrain_bridge_drawIndirect.
//
// LAYER MAP: keyed by mcTextureNodeIndex (NOT textures[] slot).
//   q.terrainHandle returned by quad.cpp:546 (getTextureHandle) is the
//   nodeIdx, NOT the slot — see V22 in Verification Appendix.
// ---------------------------------------------------------------------------
GLuint  g_cementAtlasGLTex          = 0;
int     g_cementAtlasGridSide       = 0;   // cells per row/col (power of 2)
int     g_cementAtlasTileCount      = 0;   // distinct cement entries enumerated
bool    g_cementLayerMapReady       = false;
int     g_cementCatalogTruncated    = 0;   // 1 if N>=255 cap hit (Gate A FAIL)

// Dense lookup: mcTextureNodeIndex → atlas layer-index (0..N-1).
// Sized MC_MAXTEXTURES = 4096 (mclib/txmmgr.h:44) — node-index space.
// 0xFFFF = "not cement / not in atlas".
uint16_t g_cementLayerIndexByNodeIdx[MC_MAXTEXTURES];

// Slot-keyed lookup: textures[] slot index → atlas layer-index (0..N-1).
// SLOT IS THE STABLE KEY (persistent across frames).  nodeIdx (mcTextureManager
// handle) mutates per-frame per memory/mc2_texture_handle_is_live.md, so a
// nodeIdx-keyed lookup miss-hits per frame → cement validity bit flickers
// → visible concrete flicker.  Slot is allocated once by initTexture and is
// stable for the mission lifetime.
//
// Sized MC_MAX_TERRAIN_TXMS = 3000 (terrtxm.h:34) — the textures[] cap.
// 0xFFFF = "not cement / not in atlas".
uint16_t g_cementLayerIndexBySlot[MC_MAX_TERRAIN_TXMS];

static constexpr uint32_t kCementTransitionBit = 0x40000000u;  // IS_TRANSITION in cementWord
static constexpr uint32_t kCementMaskIdShift    = 24u;         // bits 29:24 = maskId
static constexpr uint32_t kCementMaskIdMask     = 0x3Fu;       // 6 bits (14 shapes used)
static constexpr int      kTransitionMaskLayers = 14;
static constexpr int      kTransitionMaskSize   = 256;

GLuint g_transitionMaskArrayGL = 0;
bool   g_transitionMaskReady   = false;

// binNumber = (c0?8:0)|(c1?4:0)|(c2?2:0)|(c3?1:0); 0 and 15 are degenerate.
// maskId = binNumber - 1 (0..13).
int deriveMaskIdFromCorners(bool c0, bool c1, bool c2, bool c3) {
    const uint32_t bn = (c0 ? 8u : 0u) | (c1 ? 4u : 0u) | (c2 ? 2u : 0u) | (c3 ? 1u : 0u);
    if (bn == 0u || bn == 15u) return -1;
    return static_cast<int>((bn - 1u));
}

// ---------------------------------------------------------------------------
// PAD-EDGE-ROAD-1 (2026-07-21, field residual of MISSING-RUNWAY-1): pad-edge
// transition tiles whose corners DO touch cement terrain stayed frag-owned
// (CEMENT-HARD-EDGE-1 analytic cut) — but where a ROAD runs INTO the pad the
// authored tile art carries the road-transition graphics, which the analytic
// cement/terrain edge cannot reproduce: the road stopped dead with a bare
// notch at the pad edge (mc2_01 runway-15W closeup). Fix: a cement-corner
// transition tile that is 8-adjacent to ROAD ART reverts fully to the stock
// pipeline — legacy decal baked (BuildDecalStaticVBO) AND no cement word
// (PopulateRecipeCementWords), so the frag draws plain base terrain under the
// authored art (no analytic wedge peeking through the art's alpha). The two
// predicates are complementary by construction (shared helpers below): every
// such tile has exactly ONE owner. Killswitch MC2_PAD_EDGE_ROAD_FIX=0 ->
// tonight's behavior (analytic edge, road notch).
// ---------------------------------------------------------------------------
// PAD-EDGE-ROAD-2: mode selector. 0 = fix OFF (analytic edge only — road
// notch returns); 1 = DEFAULT both-draw (analytic cement word kept for outline
// continuity + legacy road decal baked on top); 2 = PAD-EDGE-ROAD-1 legacy
// decal-only revert (no cement word on road-adjacent tiles — the outline-seam
// regression form, kept for A/B).
static int PadEdgeRoadFixMode() {
    static const int s = mc2::gate::get<"MC2_PAD_EDGE_ROAD_FIX">();
    return s;
}
bool PadEdgeRoadFixEnabled() { return PadEdgeRoadFixMode() != 0; }

// CEMENT-ATLAS-MAGENTA-KEY-1: solid cement catalog cells are uploaded through
// TerrainTextures::textureFromMemory (gos_Texture_Solid), which -- unlike the
// keyed transition path (textureFromMemoryAlpha, TERRAIN-ROAD-COLORKEY-
// TRANSPARENCY-1) -- never honours MC2's legacy 0xFF00FF colourkey. Any magenta
// left in a solid cement cell is packed into the gutterless atlas and sampled
// GL_LINEAR, which shows as thin pink lines/patches along road and pad edges.
// A census is ALWAYS printed so a map can be judged; the texels are left as authored.

// CEMENT-ALPHA-LAND-1 (successor to RUNWAY-APRON-TONE-2; user has now hit
// "we are missing part of the runway" on three maps):
//
// The transition-tile drop measured by [CEMENT_TRANSITION_DROP v1] is
// structural, not a tuning problem. `BuildCementCatalogAtlas` admits ONLY
// pure-cement (isCement && !isAlpha) slots into the catalog, so an alpha
// (transition) slot has no atlas layer of its own. `PopulateRecipeCementWords`
// therefore has to BORROW a layer from a corner vertex whose terrainType is a
// SOLID cement slot -- and where pavement crosses open ground there is no such
// corner. Result: mc2_10 278/320 = 87% of transition tiles bake no word at all;
// mc2_24 266/778 = 34%. MC2_CEMENT_TRANSITION_FALLBACK borrows from an
// orthogonal NEIGHBOUR instead of a corner, but dropped tiles form contiguous
// runs with no paved neighbour to borrow from, so it recovers only ~12%.
//
// This gate closes the hole at the source: alpha cement slots are APPENDED to
// the catalog atlas as their own cells (after all solid cells, so existing
// layer indices are byte-stable), and a dropped transition tile bakes a word
// pointing at ITS OWN slot's layer with the OWN-ALPHA bit set. The fragment
// shader then uses the atlas cell's own ALPHA channel as coverage -- the road
// art composites over the terrain instead of replacing it -- so no borrowed
// corner and no marching-squares mask are needed.
//
// Deliberately NARROW: the new path fires ONLY on tiles the existing logic
// DROPS. Tiles that already bake a marching-squares word keep it untouched, so
// the user-approved CEMENT-HARD-EDGE-1 crisp transitions are not disturbed.
// RUNWAY-CLOSE-1 (2026-08-02): always on. Measured at the user's mc2_10
// saved_204907 bookmark (exe 30c18df1): rescued swath px 15 -> 6506, their
// luma 32.4 (bare dirt) -> 54.7 = the historical on-concrete runway value
// (65.0-65.7 at runway_wide), i.e. the user criterion "the color they are
// on top of concrete". Control pose byte-identical.

// Slot -> atlas layer index for ALPHA (transition) cement slots. Kept SEPARATE
// from g_cementLayerIndexBySlot on purpose: that map is also the "is this
// terrainType a solid cement corner" oracle used by cemLayerForVertex() and by
// the solid-quad branch, and folding alpha slots into it would silently promote
// every transition tile to a full-replacement solid slab.
uint16_t g_cementAlphaLayerIndexBySlot[MC_MAX_TERRAIN_TXMS];
bool     g_cementAlphaLayerMapReady = false;

// cementWord bit 23 = OWN-ALPHA transition: layerIdx addresses an alpha cell
// whose own texel alpha is the coverage. Bits 31,30 and 29:24 (maskId) are
// spoken for; 21:16 are free (19:16 held the deleted runway edge-blend mask).
static constexpr uint32_t kCementOwnAlphaBit = 0x00800000u;

// ---------------------------------------------------------------------------
// PAVED-OVERLAY-CLASS-1 — the overlay-vs-base-slot classification gap.
//
// Roads and runways in MC2 are `Overlays` ids (PAVED_ROAD=1, RUNWAY=7, ...)
// painted over an ARBITRARY base terrain type. `isCement()` tests a flag on the
// BASE SLOT. The two channels are disjoint, so a paved overlay laid on dirt is
// never cement-classified: it never reaches the transition branch, increments no
// drop counter, and renders as LIT baked colormap right beside a
// lighting-EXEMPT cement apron. Measured by [CEMENT_PAVEMENT_GAP v1]: mc2_01
// 29%, mc2_03 81%, mc2_05 41%, mc2_10 58%, mc2_24 34% of paved-overlay tiles
// carry no cement word at all. At the user's mc2_10 @204937 pose the runway is
// 100% unclassified.
//
// Every pre-existing repair lever (MC2_CEMENT_LIT, the tone-match gates, the
// edge-blend dilate) lives INSIDE the `isCement && isAlpha` guard or is
// multiplied by `pureConcrete`, which is why none of them can move overlay-only
// pavement: ROAD-OVERLAY-LIGHT-1 captured the runway luma as BIT-IDENTICAL
// across all four MC2_CEMENT_LIT strengths.
//
// cementWord bit 22 = PAVED-OVERLAY FLAT: this tile carries a hard-paved overlay
// id but has no cement word of its own. Deliberately baked WITHOUT
// kCementLayerValidBit (bit 31): every existing reader -- the frag's cement
// override at :920, the diag-connect neighbour scan at :1085, the runway-edge
// dilate at :1151, the fallback donor search and the edge-blend pass below --
// gates on bit 31 first, so this bit is inert to all of them by construction and
// cannot perturb cement outline geometry. Only the lighting-exemption block in
// terrain_lod_chunk.frag consults it.
static constexpr uint32_t kCementPavedFlatBit = 0x00400000u;

// cementWord bit 24 = AUTHORED-ART-ON-CEMENT (OVERLAY-CEMENT-ADOPT-1, widened by
// OVERLAY-CEMENT-ARTGUARD-1 / CONCRETE-DELIVER-2). This tile has a VALID cement
// word (bit 31) AND carries an authored &Overlays id, i.e. hand-placed art
// painted on top of a genuine cement base slot. It marks the case where "this
// fragment sits on cement" is NOT sufficient licence to repaint it with the
// concrete atlas: the visible art there is the authored overlay tile, and
// replacing it would be exactly the albedo swap the user rejected twice as "WAY
// worse". terrain_overlay.frag reconciles such a tile's LIGHTING MODEL with the
// terrain (static-shadow exemption + no in-shader distance fog) but leaves its
// ALBEDO alone.
//
// WIDENED TWICE. (1) CONCRETE-DELIVER-2 went from IsPavedOverlayType() to "ANY
// AUTHORED &Overlays ID". The original landing marked only the 9 hard-paved ids,
// leaving EIGHT authored overlay types unmarked -- DIRT_ROAD(0), ROUGH(2),
// TWO_LANE_DIRT_ROAD(5), X_DIRT_2LANEDIRT(9), X_DIRT_DAMAGED(10),
// X_2LANEDIRT_DAMAGED(13), OBRIDGE(14), DAMAGED_BRIDGE(16). Any of those on a
// cement base slot (a dirt track running onto an apron, a bridge deck approaching
// a paved pad) was classified as a bare cement transition and had its authored art
// OVERPAINTED with plain concrete -- silently deleting hand-placed map art. The
// user's scoping was "runways, roads, OTHER OVERLAYS are their own layer", so the
// guard is the presence of authored art, not its paved-ness.
//
// (2) CORRECTION-FIRST-MERGE-1 went from "any &Overlays id" to TileIsRoadArt(),
// because the &Overlays channel is not the only place authored road art lives.
// mc2_10's roads and airbase runway are cement-ALPHA transition art over snow
// carrying NO &Overlays id, and they still arrive here with a valid cement word
// via the CEMENT-ALPHA-LAND-1 rescue -- so the id-based guard let the adoption
// repaint them (+46.8 L over 23.0% of the runway frame). TileIsRoadArt() tests
// "&Overlays id OR cement-alpha art with no cement corner" and so is a strict
// superset of (1).
//
// Ambiguity resolves to do-nothing in both directions: only a cement tile with NO
// authored art of EITHER kind is treated as a genuine concrete transition and
// repainted, and when the oracle's inputs are not ready the tile is protected.
//
// Inert to every pre-existing reader by construction: bits 30/23/22/15:0
// are untouched, and no existing code path masks or compares bit 24.
static constexpr uint32_t kCementAuthoredArtBit = 0x01000000u;

// MC2_OVERLAY_CEMENT_ADOPT — 1 = ON (default). See RendererFeatureRegistry.h and
// terrain_overlay.frag for the full mechanism. Host side, this gate only decides
// whether the bit-24 marking pass below runs; the frag reads its own uniform.
// 0 = OFF => no bit 24 written AND the frag uniform uploads 0 => byte-identical.
static bool OverlayCementAdoptEnabled() {
    static const bool s = mc2::gate::get<"MC2_OVERLAY_CEMENT_ADOPT">();
    return s;
}

// MC2_PAVED_OVERLAY_CLASS — 0 = OFF (default, byte-identical to stock)
//   1 = LIGHTING mode. Paved-overlay tiles get the same lighting exemption
//       cement gets (flat-lit), but KEEP their own baked colormap albedo. This
//       is the lever that cures "the runway changes tone along its own length"
//       (that variation IS the per-tile normal/shadow lighting the neighbouring
//       flat-lit cement does not receive) without swapping source textures.
//   2 = ALBEDO mode. Additionally routes the tile into the solid cement atlas
//       so it samples the same texture the apron does. Full convergence of tone
//       but replaces the baked road art; retained as an A/B arm because the two
//       prior albedo-swapping attempts (MC2_CEMENT_TONE_MATCH,
//       MC2_RUNWAY_TONE_MATCH) were user-rejected as "WAY worse".
static int PavedOverlayClassMode() {
    // PAVED-OVERLAY-CLASS-DEFAULT-ON-1 (CONCRETE-DELIVER-1) promoted this to
    // default 1. CONCRETE-DELIVER-2 REVERTS THAT to default 0, because the
    // justification it shipped was a claim a later lane had already measured
    // and retracted IN WRITING:
    //
    //   .claude/CEMENT-EDGE-CURVE-1-STATUS.md sec.4, measured at the user's
    //   own night poses: mode 1 produces max delta 20-29/255 and 9.2-21.7%
    //   HARD-STEP pixels -- "It ADDS hard-edged outlines rather than removing
    //   them." Same doc: "the ROAD-OVERLAY-LIGHT-1 attribution -- that the
    //   user's 'weird coloured edge' IS the classification boundary and that
    //   closing the classification gap cures it -- is wrong in the direction
    //   it claims... This should not propagate into another status doc."
    //   It then propagated into the CONCRETE-DELIVER-1 commit message and
    //   into this comment block verbatim.
    //
    // The mechanism refutes the promotion independently. Mode 1 is
    // LIGHTING-ONLY by construction (terrain_lod_chunk.frag:1897 sets
    // normalLight = 1.0 and the code there states "Albedo is deliberately
    // untouched... a residual albedo gap is expected and measured"). An
    // unclassified paved tile keeps its DARK baked colormap, so
    // CEMENT-EDGE-CURVE-1 sec.3 measured its ceiling at L~24 against concrete
    // at L~60-87: flat lighting alone can never make mc2_10's runway "the
    // color they are on top of concrete like in mc2_01", which is the user
    // requirement the promotion was justified by. The gap is albedo-side.
    //
    // The originating lane also set its own precondition, unmet:
    // .claude/PAVED-OVERLAY-CLASS-1-STATUS.md "Recommended next steps" 1-2 --
    // a user click-through on mc2_10 saved_204937 is "the single decision
    // that gates everything else", and pose coverage must be extended
    // "before any default-ON proposal". Neither happened.
    //
    // Default 0 = not one word written = byte-identical stock. The user can
    // evaluate it in one step with MC2_PAVED_OVERLAY_CLASS=1; nothing else
    // in the concrete-seam fix depends on it (OVERLAY-CEMENT-ADOPT-1 acts
    // only on tiles that already carry a valid cement word, which this pass
    // explicitly skips).
    static const int s = mc2::gate::get<"MC2_PAVED_OVERLAY_CLASS">();
    return s;
}

// Hard-paved overlay ids. DIRT_ROAD, ROUGH, TWO_LANE_DIRT_ROAD and the
// dirt-to-dirt crossings are deliberately EXCLUDED -- they are dirt surfaces and
// must keep normal terrain lighting; only surfaces the user calls "concrete" and
// "the roads"/"the runway" are in scope.
static inline bool IsPavedOverlayType(Overlays t) {
    return t == PAVED_ROAD || t == DIRT_ROAD_TO_PAVED_ROAD
        || t == PAVED_ROAD_CROSSING_DIRT_ROAD || t == DAMAGED_ROAD
        || t == RUNWAY || t == X_DIRT_PAVED || t == X_2LANEDIRT_PAVED
        || t == X_DAMAGED_PAVED || t == X_PAVED_ROAD_BRIDGE;
}

// Mean RGB over the [P50,P90] luminance window of atlas cell k (the "concrete
// band"). Returns false when the cell has too few usable texels.
static bool CementCellBandMean(const std::vector<uint32_t>& atlasBuf,
                               int atlasPixelSide, int gridSide, int txmSize,
                               int k, float& outR, float& outG, float& outB) {
    const int dstX = (k % gridSide) * txmSize;
    const int dstY = (k / gridSide) * txmSize;
    std::vector<float> lum;
    lum.reserve(static_cast<size_t>(txmSize) * static_cast<size_t>(txmSize));
    for (int py = 0; py < txmSize; ++py) {
        const uint32_t* row = &atlasBuf[static_cast<size_t>((dstY + py)) * atlasPixelSide + dstX];
        for (int px = 0; px < txmSize; ++px) {
            const uint32_t c = row[px];
            lum.push_back(0.299f * float((c >> 16) & 0xFFu)
                        + 0.587f * float((c >>  8) & 0xFFu)
                        + 0.114f * float( c        & 0xFFu));
        }
    }
    std::vector<float> sorted(lum);
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    const float p50 = sorted[n * 50 / 100];
    const float p90 = sorted[n * 90 / 100];
    double sr = 0.0, sg = 0.0, sb = 0.0; long cnt = 0;
    for (int py = 0; py < txmSize; ++py) {
        const uint32_t* row = &atlasBuf[static_cast<size_t>((dstY + py)) * atlasPixelSide + dstX];
        for (int px = 0; px < txmSize; ++px) {
            const uint32_t c = row[px];
            const float b = float( c        & 0xFFu);
            const float g = float((c >>  8) & 0xFFu);
            const float r = float((c >> 16) & 0xFFu);
            const float L = 0.299f * r + 0.587f * g + 0.114f * b;
            if (L < p50 || L > p90) continue;
            sr += r; sg += g; sb += b; ++cnt;
        }
    }
    if (cnt < 16) return false;
    outR = static_cast<float>((sr / static_cast<double>(cnt)));
    outG = static_cast<float>((sg / static_cast<double>(cnt)));
    outB = static_cast<float>((sb / static_cast<double>(cnt)));
    return outR >= 4.f && outG >= 4.f && outB >= 4.f;
}

// ---------------------------------------------------------------------------
// CEMENT-ATLAS-CELL-2 tunables. Deliberately NOT env-exposed: they define what
// "actual concrete" means, and a taste dial on the CLASSIFIER would make the
// two arms non-comparable in the review pack.
static const float kConcreteChromaMax  = 0.20f;  // >this = coloured art, not concrete
static const float kConcreteLumLo      = 0.72f;  // family window, x family centre
static const float kConcreteLumHi      = 1.32f;
// MEASURED CALIBRATION (mc2_24 atlas dump, 110 solid cells, 2026-07-28). The
// first cut tested |L - bandL| > 45 in BOTH directions and misfired badly: it
// flagged 78/110 cells, which gutted ARM B (remapped=31, visually inert).
// Dumping the atlas showed why -- NOT ONE solid cell on mc2_24 has a single
// texel more than 45 luma ABOVE its band mean (painted markings simply are not
// authored into solid cement cells on this map), while 78 cells carry a large
// DARK region: 36-52% of the tile at L~60 against concrete at L~147. Those dark
// regions are authored material (joints, tarmac, shadowed detail), not paint --
// and whether to keep them is exactly the ARM A vs ARM B question the user is
// grading, so they must NOT be silently excluded. Markings are therefore tested
// on the BRIGHT side only, at a delta no ordinary concrete texture reaches
// (measured max excursion above band on mc2_24: ~11 luma).
static const float kConcreteMarkDelta  = 60.f;   // L - bandL beyond this = bright paint
static const float kConcreteMarkFrac   = 0.02f;  // >2% such texels = cell carries markings

// CEMENT-INTRACELL-1 road-art thresholds. MEASURED (mc2_24/23/05/03/01/10 atlas
// dumps 2026-07-28): a solid cement cell carries a BAKED ASPHALT ROAD TILESET
// as dark texels 65 luma below its concrete background (background L 144.7..149.5,
// road art L 66.2..86.2, hard-edged), so a cell is classified against its own
// [P60,P95] background, not the band mean the road drags down.
static const float kRoadArtDarkDelta = 40.f;   // bgL - L beyond this = asphalt
static const float kRoadArtFrac      = 0.05f;  // >5% such texels = cell carries road art

// ---------------------------------------------------------------------------
// CEMENT-ATLAS-CELL-2: the CONCRETE-FAMILY classifier, shared by both arms.
//
// WHY THIS EXISTS. Every prior tone lever (MC2_CEMENT_TONE_MATCH,
// MC2_RUNWAY_TONE_MATCH) normalised EVERY solid cement cell toward one global
// median. That is why the user rejected them ("after is WAY worse" -- authored
// material variation flattened into a pale wash): an asphalt cell, a dirt-
// stained pad and a clean concrete apron were all dragged onto one tone.
// The user's scope ruling is narrower: "by all concrete I mean only the actual
// concrete ... runways, roads, other overlays etc. are their own layer".
//
// So this classifier answers a question no prior gate asked: IS THIS CELL
// ACTUALLY CONCRETE? Cells outside the family are left byte-identical by both
// arms. Runway/road art cannot reach here in the first place -- it arrives as
// an &Overlays id or as a cement-ALPHA transition cell, and the ALPHA cells are
// deliberately kept out of g_cementLayerIndexBySlot (see CEMENT-UNIFORM-2 at
// the slot-map build) and out of the [0,nSolidCells) range this operates on.
// Painted markings that DO live inside a solid concrete cell are detected here
// (markFrac) and reported per-cell, so they can be excluded by index.

// Neutrality + marking census for one cell, on top of the band mean.
void CementCellComputeStats(const std::vector<uint32_t>& atlasBuf,
                                   int atlasPixelSide, int gridSide, int txmSize,
                                   int k, CementCellStats& s) {
    s.ok = CementCellBandMean(atlasBuf, atlasPixelSide, gridSide, txmSize,
                              k, s.r, s.g, s.b);
    if (!s.ok) return;
    s.lum = 0.299f * s.r + 0.587f * s.g + 0.114f * s.b;
    const float mx = std::max(s.r, std::max(s.g, s.b));
    const float mn = std::min(s.r, std::min(s.g, s.b));
    s.chroma = (mx > 1.f) ? ((mx - mn) / mx) : 0.f;

    // Marking census: texels a long way off the cell's own band luminance are
    // paint (bright lane stripes / numbers) or tar (dark seams), not concrete.
    const int dstX = (k % gridSide) * txmSize;
    const int dstY = (k / gridSide) * txmSize;
    long offBand = 0, tot = 0;   // NB: not `far` -- that is a Windows keyword macro.
    std::vector<float> lums;
    lums.reserve(static_cast<size_t>(txmSize) * static_cast<size_t>(txmSize));
    for (int py = 0; py < txmSize; ++py) {
        const uint32_t* row = &atlasBuf[static_cast<size_t>((dstY + py)) * atlasPixelSide + dstX];
        for (int px = 0; px < txmSize; ++px) {
            const uint32_t c = row[px];
            if ((c & 0xFF000000u) == 0u) continue;   // transparent surround
            const float L = 0.299f * float((c >> 16) & 0xFFu)
                          + 0.587f * float((c >>  8) & 0xFFu)
                          + 0.114f * float( c        & 0xFFu);
            ++tot;
            lums.push_back(L);
            if (L - s.lum > kConcreteMarkDelta) ++offBand;   // BRIGHT side only
        }
    }
    s.markFrac = (tot > 0) ? static_cast<float>(offBand) / static_cast<float>(tot) : 0.f;
    s.marks    = s.markFrac > kConcreteMarkFrac;

    // CEMENT-INTRACELL-1 (ARM C): split the cell into concrete BACKGROUND and
    // baked ROAD art. The background reference is the [P60,P95] upper mode --
    // deliberately NOT s.lum, which on a road-heavy cell is dragged down by the
    // road itself and would hide it (that is exactly how the road tileset went
    // unnoticed in CEMENT-ATLAS-CELL-2).
    if (!lums.empty()) {
        std::sort(lums.begin(), lums.end());
        const size_t lo = static_cast<size_t>((static_cast<float>(lums.size()) * 0.60f));
        const size_t hi = std::min(lums.size() - 1, static_cast<size_t>((static_cast<float>(lums.size()) * 0.95f)));
        double acc = 0.0; long n = 0;
        for (size_t i = lo; i <= hi; ++i) { acc += lums[i]; ++n; }
        s.bgLum = (n > 0) ? static_cast<float>((acc / static_cast<double>(n))) : s.lum;
        // SECOND PASS. On a cell where the road covers >40% of the tile (the
        // crossroads and highway pieces do), the [P60,P95] window still straddles
        // the asphalt and drags the reference down -- measured, that reports a
        // 26.3-luma family background span on mc2_24 where the true concrete
        // span is 4.7. Re-mean over the texels the first pass identified as
        // background, so bgLum is the CONCRETE, not a road-weighted average.
        {
            double acc2 = 0.0; long n2 = 0;
            for (float L : lums) if (s.bgLum - L <= kRoadArtDarkDelta) { acc2 += L; ++n2; }
            if (n2 >= 16) s.bgLum = static_cast<float>((acc2 / static_cast<double>(n2)));
        }
        long dark = 0;
        for (float L : lums) if (s.bgLum - L > kRoadArtDarkDelta) ++dark;
        s.roadFrac = static_cast<float>(dark) / static_cast<float>(lums.size());
        s.road     = s.roadFrac > kRoadArtFrac;
    }
}

// Classify [0,NT) solid cells. Seeds the reference from the UPPER-cluster median
// (the apron tone, not a median dragged down by asphalt), admits neutral cells
// within a luminance window, then re-medians once over the admitted set so the
// reference is the family's own tone rather than the whole atlas's.
// Returns the family reference in refR/refG/refB and the canonical cell index.
int ClassifyConcreteFamily(std::vector<CementCellStats>& st, int NT,
                                  float& refR, float& refG, float& refB) {
    std::vector<float> okL;
    for (int k = 0; k < NT; ++k) if (st[static_cast<size_t>(k)].ok) okL.push_back(st[static_cast<size_t>(k)].lum);
    if (okL.empty()) return -1;
    std::sort(okL.begin(), okL.end());
    const float medL = okL[okL.size() / 2];

    std::vector<float> upper;
    for (float L : okL) if (L + 0.001f >= medL) upper.push_back(L);
    float seedL = upper.empty() ? medL : upper[upper.size() / 2];

    auto admit = [&](float centreL) {
        for (int k = 0; k < NT; ++k) {
            CementCellStats& c = st[static_cast<size_t>(k)];
            c.family = c.ok
                    && c.chroma <= kConcreteChromaMax
                    && c.lum >= centreL * kConcreteLumLo
                    && c.lum <= centreL * kConcreteLumHi;
        }
    };
    admit(seedL);

    std::vector<float> famL;
    for (int k = 0; k < NT; ++k) if (st[static_cast<size_t>(k)].family) famL.push_back(st[static_cast<size_t>(k)].lum);
    if (!famL.empty()) {
        std::sort(famL.begin(), famL.end());
        seedL = famL[famL.size() / 2];
        admit(seedL);
    }

    auto medOf = [&](float CementCellStats::* m) -> float {
        std::vector<float> t;
        for (int k = 0; k < NT; ++k) if (st[static_cast<size_t>(k)].family) t.push_back(st[static_cast<size_t>(k)].*m);
        if (t.empty()) return 0.f;
        std::sort(t.begin(), t.end());
        return t[t.size() / 2];
    };
    refR = medOf(&CementCellStats::r);
    refG = medOf(&CementCellStats::g);
    refB = medOf(&CementCellStats::b);
    if (!(refR > 0.f && refG > 0.f && refB > 0.f)) return -1;

    // Canonical cell = family member closest to the reference that carries NO
    // markings (substituting a marked cell everywhere would smear paint across
    // the whole apron). Lowest index wins ties -> deterministic across runs.
    int canon = -1; float bestD = 1e30f;
    for (int k = 0; k < NT; ++k) {
        const CementCellStats& c = st[static_cast<size_t>(k)];
        if (!c.family || c.marks) continue;
        const float d = (c.r - refR) * (c.r - refR)
                      + (c.g - refG) * (c.g - refG)
                      + (c.b - refB) * (c.b - refB);
        if (d < bestD - 1e-4f) { bestD = d; canon = k; }
    }
    return canon;
}


// True when tile (r,c) carries road/runway art: an &Overlays id, OR a
// cement-ALPHA transition art tile with NO cement-terrain corner (the
// MISSING-RUNWAY-1 decal-baked road/runway class). Requires
// g_cementLayerMapReady (callers guard).
static bool TileIsRoadArt(long r, long c, long mapSide, PostcompVertexPtr blocks) {
    if (r < 0 || c < 0 || r >= mapSide - 1 || c >= mapSide - 1) return false;
    Overlays ovT = INVALID_OVERLAY; DWORD ovO = 0;
    Terrain::mapData->getOverlay(r, c, ovT, ovO);
    if (ovT != INVALID_OVERLAY) return true;
    auto* tt = Terrain::terrainTextures;
    if (!tt || !blocks) return false;
    const DWORD slot = Terrain::mapData->getTexture(r, c) & 0xFFFFu;
    if (slot >= static_cast<DWORD>(MC_MAX_TERRAIN_TXMS)) return false;
    if (g_cementLayerIndexBySlot[slot] != 0xFFFFu) return false;  // solid cement, not road
    if (!(tt->isCement(slot) && tt->isAlpha(slot))) return false;
    auto cemCorner = [&](long rr, long cc) -> bool {
        const DWORD vt = blocks[static_cast<size_t>(rr) * static_cast<size_t>(mapSide) + static_cast<size_t>(cc)].terrainType;
        return vt < static_cast<DWORD>(MC_MAX_TERRAIN_TXMS)
               && g_cementLayerIndexBySlot[vt] != 0xFFFFu;
    };
    return !(cemCorner(r, c) || cemCorner(r, c + 1)
             || cemCorner(r + 1, c + 1) || cemCorner(r + 1, c));
}

bool TileRoadAdjacent8(long r, long c, long mapSide, PostcompVertexPtr blocks) {
    for (long dr = -1; dr <= 1; ++dr)
        for (long dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) continue;
            if (TileIsRoadArt(r + dr, c + dc, mapSide, blocks)) return true;
        }
    return false;
}

// Build 14-layer R8 GL_TEXTURE_2D_ARRAY procedurally (bilinear weights + smoothstep).
// Called once at end of BuildCementCatalogAtlas().
void BuildTransitionMaskArray() {
    const int N = kTransitionMaskLayers;
    const int S = kTransitionMaskSize;
    std::vector<uint8_t> buf(static_cast<size_t>(N) * S * S, 0u);
    for (int k = 0; k < N; ++k) {
        const uint32_t bn = static_cast<uint32_t>((k + 1));
        const bool c0 = (bn & 8u) != 0u;
        const bool c1 = (bn & 4u) != 0u;
        const bool c2 = (bn & 2u) != 0u;
        const bool c3 = (bn & 1u) != 0u;
        uint8_t* layer = &buf[static_cast<size_t>(k) * S * S];
        for (int py = 0; py < S; ++py) {
            for (int px = 0; px < S; ++px) {
                const float u  = (float(px) + 0.5f) / float(S);
                const float v  = (float(py) + 0.5f) / float(S);
                const float w0 = (1.f - u) * (1.f - v);  // corner 0: UV(0,0)
                const float w1 =        u  * (1.f - v);  // corner 1: UV(1,0)
                const float w2 =        u  *        v;   // corner 2: UV(1,1)
                const float w3 = (1.f - u) *        v;   // corner 3: UV(0,1)
                float cov = (c0 ? w0 : 0.f) + (c1 ? w1 : 0.f)
                          + (c2 ? w2 : 0.f) + (c3 ? w3 : 0.f);
                cov = cov * cov * (3.f - 2.f * cov);  // smoothstep sharpening
                layer[py * S + px] = static_cast<uint8_t>((std::min(cov, 1.f) * 255.f + 0.5f));
            }
        }
    }
    // TEX-CLASS: asset-pool -- terrain transition-mask 2D_ARRAY (content)
    if (g_transitionMaskArrayGL == 0) glGenTextures(1, &g_transitionMaskArrayGL);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_transitionMaskArrayGL);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, S, S, N, 0, GL_RED, GL_UNSIGNED_BYTE, buf.data());
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    g_transitionMaskReady = true;

    // REGISTRY-TERRAIN-SSBO-1: register the transition-mask 2D_ARRAY (observe-only).
    {
        RenderCore::RenderResourceDesc d;
        d.id        = RenderCore::RenderResourceId::TransitionMaskArray;
        d.kind      = RenderCore::RenderResourceKind::Texture2DArray;
        d.lifetime  = RenderCore::RenderResourceLifetime::Mission;  // rebuilt per mission load
        d.format    = RenderCore::RenderResourceFormat::RGBA8;  // R8 storage; closest enum slot
        d.debugName = "TransitionMaskArray";
        d.width     = static_cast<uint32_t>(S);
        d.height    = static_cast<uint32_t>(S);
        d.layers    = static_cast<uint32_t>(N);
        d.glName    = static_cast<uint32_t>(g_transitionMaskArrayGL);
        d.sizeBytes = static_cast<uint64_t>(N) * static_cast<uint64_t>(S) * static_cast<uint64_t>(S);
        d.valid     = true;
        RenderCore::registerOrUpdateRenderResource(d);
    }

    MC2_DIAGF("[CEMENT_ATLAS v1] event=transition_mask_array_built layers=%d size=%dx%d\n", N, S, S);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// PopulateRecipeCementWords — bake cementWord into recipe._wp3 for every slot.
// Must be called AFTER BuildCementCatalogAtlas() because g_cementLayerIndexBySlot
// is not populated until that function runs.
// Marks all modified slots dirty so FlushDirtyRecipeSlotsToGPU uploads them.
// ---------------------------------------------------------------------------
void PopulateRecipeCementWords() {
    if (!Terrain::mapData) return;
    const long mapSide = Terrain::realVerticesMapSide;
    if (mapSide <= 0) return;
    const size_t N = g_denseRecipes.size();
    constexpr uint32_t kCementLayerValidBit = 0x80000000u;

    PostcompVertexPtr blocks = Terrain::mapData->getBlocks();
    auto* tt = Terrain::terrainTextures;

    long cementQuadCount    = 0;
    long transitionQuadCount = 0;
    // CEMENT-TRANSITION-FALLBACK-1 census: transition tiles that bake NO cement
    // word (no solid-cement corner, or maskId<0) and therefore render bare
    // terrain even though the paving/road art exists in the catalog.
    long transitionDroppedNoLayer = 0;
    long transitionDroppedNoMask  = 0;
    // CEMENT-ALPHA-LAND-1: of the dropped tiles above, how many were rescued by
    // an own-alpha atlas cell. The drop counters still count them, so
    // dropped/total stays the LEGACY rate and (dropped-rescued)/total is the
    // rate with the gate on -- the two arms remain directly comparable.
    long transitionAlphaRescued   = 0;
    // CEMENT-UNIFORM-2 census: the TRUE pavement denominator.
    // [CEMENT_TRANSITION_DROP v1] counts only tiles already recognised as
    // cement-transition (isCement && isAlpha), so its dropped/total rate is
    // measured against a denominator that has ALREADY excluded the tiles this
    // census is about. Roads and runways in MC2 are &Overlays ids (PAVED_ROAD,
    // RUNWAY, ...) painted over an arbitrary base terrain type; cement is a
    // disjoint base-type set (BASE_CEMENT_TYPE, START..END_CEMENT_TYPE). A
    // paved overlay laid on dirt is therefore NOT cement-classified at all: it
    // never reaches the transition branch, increments no drop counter, and
    // renders as lit colormap + decal beside a lighting-exempt cement apron.
    // That is the user-annotated "plainly pavement but BLACK in DIAG=32768"
    // class. LEGEND (ground truth: shaders/terrain_lod_chunk.frag:1202-1215) --
    // RED(1)=SOLID cement-atlas, GREEN(2)=TRANSITION, BLUE(3)=diag-connect,
    // YELLOW(4)=runway-edge dilate, MAGENTA(5)=authored overlay sidecar,
    // CYAN(6)=own-alpha rescue, else base*0.35 = BLACK/dim = baked colormap
    // ONLY. So RED means CORRECTLY cement-classified; BLACK is the defect
    // marker. Prior notes (and the comment formerly at the census print below)
    // had this inverted and sent three lanes chasing the wrong tiles.
    // AUDIT-TRUTH-1: this census is a DIAGNOSTIC and must not run in a default
    // build. It is folded into the existing registered diag facility
    // MC2_TERRAIN_LOD_CHUNK_DIAG as bit 524288 (0x80000). Unset => the per-tile
    // getOverlay walk below and the [CEMENT_PAVEMENT_GAP v1] print are both
    // skipped entirely, which is byte-identical to the pre-a25037cf render.
    static const bool s_pavementGapDiag = (mc2::gate::get<"MC2_TERRAIN_LOD_CHUNK_DIAG">() & 524288) != 0;
    long pavedOverlayTiles      = 0;   // tiles carrying a hard-paved overlay id
    long pavedOverlayNoCement   = 0;   // ...of those, tiles that bake NO cement word
    long anyOverlayTiles        = 0;
    // PAVED-OVERLAY-CLASS-1: the census and the repair pass below MUST agree on
    // what "paved" means, so both now call the one file-scope predicate.
    auto IsPavedOverlay = [](Overlays t) -> bool { return IsPavedOverlayType(t); };
    // Step 5c: also collect a full per-vn cement-word array for the terrain LOD
    // chunk path (indexed vn = mx + my*mapSide, matching its heightfield SSBO).
    std::vector<uint32_t> chunkCementWords(static_cast<size_t>(mapSide) * static_cast<size_t>(mapSide), 0u);
    for (size_t vn = 0; vn < N; ++vn) {
        const long mx = static_cast<long>(vn) % mapSide;
        const long my = static_cast<long>(vn) / mapSide;
        if (mx >= mapSide - 1 || my >= mapSide - 1) continue;

        uint32_t cementWord = 0u;
        if (g_cementLayerMapReady) {
            const DWORD texData = Terrain::mapData->getTexture(my, mx);
            const DWORD slot    = texData & 0xFFFFu;
            if (slot < static_cast<DWORD>(MC_MAX_TERRAIN_TXMS)) {
                const uint16_t idx = g_cementLayerIndexBySlot[slot];
                if (idx != 0xFFFFu) {
                    cementWord = kCementLayerValidBit | (static_cast<uint32_t>(idx) & 0xFFFFu);
                    ++cementQuadCount;
                } else if (blocks && tt
                           && tt->isCement(static_cast<DWORD>(slot))
                           && tt->isAlpha(static_cast<DWORD>(slot))) {
                    // Transition quad: read per-vertex terrain types from PostcompVertex.
                    // v0=(my,mx) v1=(my,mx+1) v2=(my+1,mx+1) v3=(my+1,mx) — matches
                    // mapdata.cpp setTexture() corner ordering and createTransition() packing.
                    auto cemLayerForVertex = [&](long row, long col) -> uint16_t {
                        const size_t vi = static_cast<size_t>(row) * static_cast<size_t>(mapSide) + static_cast<size_t>(col);
                        const DWORD  vt = blocks[vi].terrainType;
                        return (vt < static_cast<DWORD>(MC_MAX_TERRAIN_TXMS))
                               ? g_cementLayerIndexBySlot[vt] : 0xFFFFu;
                    };
                    const uint16_t l0 = cemLayerForVertex(my,     mx);
                    const uint16_t l1 = cemLayerForVertex(my,     mx + 1);
                    const uint16_t l2 = cemLayerForVertex(my + 1, mx + 1);
                    const uint16_t l3 = cemLayerForVertex(my + 1, mx);
                    const bool c0 = (l0 != 0xFFFFu);
                    const bool c1 = (l1 != 0xFFFFu);
                    const bool c2 = (l2 != 0xFFFFu);
                    const bool c3 = (l3 != 0xFFFFu);
                    const int maskId = deriveMaskIdFromCorners(c0, c1, c2, c3);
                    const uint16_t cementLayerIdx = c0 ? l0 : (c1 ? l1 : (c2 ? l2 : l3));
                    // PAD-EDGE-ROAD-2 (regression fix on PAD-EDGE-ROAD-1, user
                    // field report 2026-07-21b): the -1 form baked NO cement
                    // word on road-adjacent tiles, so the pad OUTLINE switched
                    // styles mid-perimeter (legacy decal edge on the 8-adjacent
                    // revert tiles vs analytic cement edge on their neighbors —
                    // angular mismatched seams). -2 keeps the analytic cement
                    // word on those tiles (outline stays continuous) and lets
                    // BuildDecalStaticVBO ALSO bake the road-transition decal on
                    // top (road art renders over the analytic wedge — stock
                    // layering, decal depth-biased). MC2_PAD_EDGE_ROAD_FIX=2
                    // restores the -1 decal-only revert for A/B; =0 restores the
                    // pre-fix analytic-only (road notch) behavior.
                    if (maskId >= 0 && cementLayerIdx != 0xFFFFu
                        && PadEdgeRoadFixMode() == 2
                        && TileRoadAdjacent8(my, mx, mapSide, blocks)) {
                        // leave cementWord = 0 (legacy -1 behavior)
                    } else if (maskId >= 0 && cementLayerIdx != 0xFFFFu) {
                        cementWord = kCementLayerValidBit
                                   | kCementTransitionBit
                                   | (static_cast<uint32_t>((maskId & static_cast<int>(kCementMaskIdMask))) << kCementMaskIdShift)
                                   | (static_cast<uint32_t>(cementLayerIdx) & 0xFFFFu);
                        ++transitionQuadCount;
                    } else if (g_cementAlphaLayerMapReady
                               && g_cementAlphaLayerIndexBySlot[slot] != 0xFFFFu) {
                        // CEMENT-ALPHA-LAND-1 rescue. This tile is a transition
                        // tile the legacy logic DROPS -- it has no solid-cement
                        // corner to borrow a layer from, or no derivable mask.
                        // Its own alpha slot now has an atlas cell, so bake a
                        // word pointing at that cell with OWN_ALPHA set; the
                        // frag uses the cell's texel alpha as coverage and
                        // composites over the terrain. maskId is left at the
                        // all-corners value: the OWN_ALPHA branch in the shader
                        // never consults it, and any future reader that does
                        // sees "fully covered", which is the honest answer for
                        // a tile whose coverage lives in its alpha channel.
                        cementWord = kCementLayerValidBit
                                   | kCementTransitionBit
                                   | kCementOwnAlphaBit
                                   | (static_cast<uint32_t>((static_cast<int>(kCementMaskIdMask))) << kCementMaskIdShift)
                                   | (static_cast<uint32_t>(g_cementAlphaLayerIndexBySlot[slot]) & 0xFFFFu);
                        ++transitionAlphaRescued;
                        if (cementLayerIdx == 0xFFFFu) ++transitionDroppedNoLayer;
                        else                           ++transitionDroppedNoMask;
                    } else if (cementLayerIdx == 0xFFFFu) {
                        ++transitionDroppedNoLayer;   // no solid-cement corner
                    } else {
                        ++transitionDroppedNoMask;    // maskId < 0
                    }
                }
            }
        }
        // CEMENT-UNIFORM-2: true-denominator census (read-only, no behaviour).
        // AUDIT-TRUTH-1: gated on MC2_TERRAIN_LOD_CHUNK_DIAG bit 524288.
        if (s_pavementGapDiag && Terrain::mapData) {
            Overlays ovT = INVALID_OVERLAY; DWORD ovO = 0;
            Terrain::mapData->getOverlay(my, mx, ovT, ovO);
            if (ovT != INVALID_OVERLAY) {
                ++anyOverlayTiles;
                if (IsPavedOverlay(ovT)) {
                    ++pavedOverlayTiles;
                    if (cementWord == 0u) ++pavedOverlayNoCement;
                }
            }
        }
        memcpy(&g_denseRecipes[vn]._wp3, &cementWord, 4);
        if (vn < chunkCementWords.size()) chunkCementWords[vn] = cementWord;
    }
    // CEMENT-TRANSITION-FALLBACK-1: census (always) + gated repair of the tiles
    // the transition branch above dropped. Runs on the completed word array so a
    // neighbour's word is final; a dropped tile adopts the first orthogonal
    // neighbour that carries a valid word, as a TRANSITION word with the
    // all-corners mask (the shader re-derives its own coverage from neighbour
    // whole-tile state anyway -- CEMENT-HARD-EDGE-1 -- so the outline shape is
    // unaffected and only the missing paving is restored).
    MC2_DIAGF("[CEMENT_TRANSITION_DROP v1] event=census mission=%s transition_baked=%ld "
           "dropped_no_layer=%ld dropped_no_mask=%ld alpha_rescued=%ld\n",
           (::missionName[0] ? ::missionName : "unknown"), transitionQuadCount,
           transitionDroppedNoLayer, transitionDroppedNoMask, transitionAlphaRescued);
    // CEMENT-UNIFORM-2: the denominator [CEMENT_TRANSITION_DROP v1] cannot see.
    // paved_no_cement = pavement that renders as LIT colormap+decal beside a
    // lighting-exempt cement apron -- the tone split the user reports, and the
    // BLACK-in-DIAG=32768 tiles they circled (RED = correctly cement-classified;
    // see the legend at the census head above). cement_total is the transition
    // census denominator for direct comparison.
    // AUDIT-TRUTH-1: opt-in only -- MC2_TERRAIN_LOD_CHUNK_DIAG bit 524288.
    if (s_pavementGapDiag) {
        mc2::fmt::print_out("[CEMENT_PAVEMENT_GAP v1] event=census mission={} paved_overlay={:d} paved_no_cement={:d} any_overlay={:d} cement_total={:d}\n", mc2::fmt::safe((::missionName[0] ? ::missionName : "unknown")), mc2::fmt::as_i32(pavedOverlayTiles), mc2::fmt::as_i32(pavedOverlayNoCement), mc2::fmt::as_i32(anyOverlayTiles), mc2::fmt::as_i32(transitionQuadCount + transitionDroppedNoLayer + transitionDroppedNoMask));
    }
    fflush(stdout);

    // -----------------------------------------------------------------------
    // PAVED-OVERLAY-CLASS-1: classify overlay-only pavement.
    //
    // This pass is deliberately OUTSIDE the `isCement && isAlpha` guard that
    // contains every pre-existing lever -- that containment is precisely why
    // none of them can reach a runway painted on dirt. It runs AFTER the census
    // above so [CEMENT_PAVEMENT_GAP v1] always reports the PRE-repair
    // denominator and the two arms stay directly comparable.
    //
    // Gate OFF => not a single word is written => byte-identical to stock.
    // OVERLAY-CEMENT-ADOPT-1 (CONCRETE-DELIVER-1), scope widened by
    // OVERLAY-CEMENT-ARTGUARD-1 (CONCRETE-DELIVER-2): mark tiles that are BOTH
    // genuine cement (valid word) AND carry ANY authored &Overlays art, so
    // terrain_overlay.frag can reconcile their lighting without repainting their
    // albedo. This is the requirement-3 scoping guard ("runways, roads, other
    // overlays are their own layer") and it only ever ADDS a reason to leave a
    // fragment's art alone. A tile with no valid cement word is never marked and
    // is never touched by the adopt path at all.
    //
    // Runs BEFORE the PavedOverlayClass pass below, which early-outs on tiles
    // that already carry bit 31 and therefore never revisits these.
    if (OverlayCementAdoptEnabled() && Terrain::mapData) {
        long authoredArtMarked = 0;
        // ARTGUARD-1 attribution counters, one per widening, so the trace says
        // which guard saved what instead of asserting a single unfalsifiable
        // total. Diagnostic only -- neither changes a word or a pixel.
        //   pavedSubset     = tiles the ORIGINAL narrow IsPavedOverlayType()
        //                     predicate (9 hard-paved ids) would have caught.
        //   overlayIdSubset = tiles CONCRETE-DELIVER-2's "any &Overlays id"
        //                     widening would have caught (all 17 ids).
        // So (overlayIdSubset - pavedSubset) is the dirt-road/bridge art the
        // first widening saved, and (authoredArtMarked - overlayIdSubset) is the
        // MISSING-RUNWAY-1 cement-alpha class that ONLY TileIsRoadArt() reaches
        // -- the mc2_10 runway. Both numbers land in ONE headless trace run
        // instead of requiring an A/B pair of builds.
        long pavedSubset     = 0;
        long overlayIdSubset = 0;
        for (size_t vn = 0; vn < N; ++vn) {
            const long mx = static_cast<long>(vn) % mapSide;
            const long my = static_cast<long>(vn) / mapSide;
            if (mx >= mapSide - 1 || my >= mapSide - 1) continue;
            if (vn >= chunkCementWords.size()) continue;
            // Only genuine cement tiles are candidates -- bit 24 is meaningless
            // without bit 31 and must never be the sole content of a word.
            if ((chunkCementWords[vn] & kCementLayerValidBit) == 0u) continue;

            // Read for ATTRIBUTION ONLY. The decision is TileIsRoadArt() below;
            // ovT is kept so the trace can report how much each of the two
            // successive widenings actually bought.
            Overlays ovT = INVALID_OVERLAY; DWORD ovO = 0;
            Terrain::mapData->getOverlay(my, mx, ovT, ovO);

            // CONCRETE-DELIVER-2 landed the FIRST widening: from the 9 hard-paved
            // ids (IsPavedOverlayType) to ANY authored &Overlays id, because
            // DIRT_ROAD, ROUGH, TWO_LANE_DIRT_ROAD, the dirt/damaged crossings,
            // OBRIDGE and DAMAGED_BRIDGE are authored art too, and repainting
            // them with the concrete atlas deletes hand-placed map art.
            //
            // CONCRETE-DELIVER-1's own mc2_10 capture finding forces a SECOND
            // widening, and it is the one that matters most: even "any &Overlays
            // id" MISSES the entire MISSING-RUNWAY-1 class. mc2_10's roads and
            // its airbase runway are authored as cement-ALPHA transition art
            // over snow with NO &Overlays id at all. Those tiles still reach
            // this loop carrying a VALID cement word -- the CEMENT-ALPHA-LAND-1
            // rescue branch above sets kCementLayerValidBit for exactly the
            // corner-less alpha tiles -- so they went unmarked, the frag treated
            // them as plain concrete transitions, and the albedo adoption
            // repainted them: measured 23.0% of the mc2_10 runway frame
            // brightened by +46.8 L. The road art was being erased. That is
            // precisely the albedo swap the user rejected twice as "WAY worse".
            //
            // TileIsRoadArt() is the codebase's existing oracle for exactly this
            // question ("&Overlays id OR cement-alpha art with no cement
            // corner"), and its first clause is `ovT != INVALID_OVERLAY`, so it
            // is a strict SUPERSET of the widening CONCRETE-DELIVER-2 shipped:
            // strictly more tiles get protected, never fewer. Requires
            // g_cementLayerMapReady + blocks, which this function already has.
            const bool roadArt = (g_cementLayerMapReady && blocks)
                                     ? TileIsRoadArt(my, mx, mapSide, blocks)
                                     : true;   // unknown => protect (conservative)
            if (!roadArt) continue;

            const uint32_t word = chunkCementWords[vn] | kCementAuthoredArtBit;
            chunkCementWords[vn] = word;
            memcpy(&g_denseRecipes[vn]._wp3, &word, 4);
            ++authoredArtMarked;
            if (ovT != INVALID_OVERLAY) {
                ++overlayIdSubset;
                if (IsPavedOverlayType(ovT)) ++pavedSubset;
            }
        }
        static const bool s_adoptTrace = mc2::gate::get<"MC2_TERRAIN_INDIRECT_TRACE">();
        if (s_adoptTrace) {
            mc2::fmt::print_out("[OVERLAY_CEMENT_ADOPT v1] event=authored_art_marked mission={} tiles={:d} paved_subset={:d} overlay_id_subset={:d} art_saved_vs_paved={:d} art_saved_vs_overlay_id={:d}\n", mc2::fmt::safe((::missionName[0] ? ::missionName : "unknown")), mc2::fmt::as_i32(authoredArtMarked), mc2::fmt::as_i32(pavedSubset), mc2::fmt::as_i32(overlayIdSubset), mc2::fmt::as_i32(authoredArtMarked - pavedSubset), mc2::fmt::as_i32(authoredArtMarked - overlayIdSubset));
            fflush(stdout);
        }
    }

    if (PavedOverlayClassMode() != 0 && Terrain::mapData) {
        const int  mode = PavedOverlayClassMode();
        // Mode 2 only: a map-wide solid cement layer to adopt. A 1-ring donor
        // search (as the transition fallback below uses) cannot work here --
        // on mc2_10 the runway is 100% unclassified, so its interior tiles have
        // no classified neighbour at any radius reachable in one pass.
        uint16_t globalCementLayer = 0xFFFFu;
        if (mode == 2 && g_cementLayerMapReady) {
            for (int slot = 0; slot < MC_MAX_TERRAIN_TXMS; ++slot) {
                if (g_cementLayerIndexBySlot[slot] != 0xFFFFu) {
                    globalCementLayer = g_cementLayerIndexBySlot[slot];
                    break;
                }
            }
        }
        long pavedFlatBaked = 0, pavedAlbedoBaked = 0;
        for (size_t vn = 0; vn < N; ++vn) {
            const long mx = static_cast<long>(vn) % mapSide;
            const long my = static_cast<long>(vn) / mapSide;
            if (mx >= mapSide - 1 || my >= mapSide - 1) continue;
            if (vn >= chunkCementWords.size()) continue;
            // Already cement-classified: the legacy path owns this tile, and its
            // outline/mask state must not be disturbed.
            if ((chunkCementWords[vn] & kCementLayerValidBit) != 0u) continue;

            Overlays ovT = INVALID_OVERLAY; DWORD ovO = 0;
            Terrain::mapData->getOverlay(my, mx, ovT, ovO);
            if (ovT == INVALID_OVERLAY || !IsPavedOverlayType(ovT)) continue;

            uint32_t word = chunkCementWords[vn];
            if (mode == 2 && globalCementLayer != 0xFFFFu) {
                // Full cement classification: solid word (no transition bit) so
                // the tile samples the same atlas cell the apron does.
                word = kCementLayerValidBit | (static_cast<uint32_t>(globalCementLayer) & 0xFFFFu);
                ++pavedAlbedoBaked;
            } else {
                // Lighting-exemption only: keep the tile's own baked colormap
                // albedo, take cement's flat light. Bit 31 stays clear so this
                // word is invisible to every other reader.
                word |= kCementPavedFlatBit;
                ++pavedFlatBaked;
            }
            chunkCementWords[vn] = word;
            memcpy(&g_denseRecipes[vn]._wp3, &word, 4);
        }
        mc2::fmt::print_out("[PAVED_OVERLAY_CLASS v1] event=applied mission={} mode={:d} flat_baked={:d} albedo_baked={:d} global_layer={:d}\n", mc2::fmt::safe((::missionName[0] ? ::missionName : "unknown")), mc2::fmt::as_i32(mode), mc2::fmt::as_i32(pavedFlatBaked), mc2::fmt::as_i32(pavedAlbedoBaked), mc2::fmt::as_i32((globalCementLayer == 0xFFFFu) ? -1 : static_cast<int>(globalCementLayer)));
        fflush(stdout);
    }

    // Push to the chunk renderer (no-op if its SSBO/Init has not run yet).
    gos_TerrainLodChunk_UploadCementWordsFull(
        chunkCementWords.data(), static_cast<int>(chunkCementWords.size()), static_cast<int>(mapSide));
    MC2_DIAGF("[CEMENT_ATLAS v1] event=cement_words_baked cement_solid=%ld cement_transition=%ld total_vn=%zu\n",
           cementQuadCount, transitionQuadCount, N);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// CollectUniqueNodeIds — build list of unique terrain texture nodeIds from
// the baked _wp2 fields. Called once per mission after recipes are fully
// populated. Used by UploadTerrainHandleLUT() to resolve handles per frame
// without iterating the full quadList.
// ---------------------------------------------------------------------------
std::vector<uint32_t> g_uniqueTerrainNodeIds;

void CollectUniqueNodeIds() {
    g_uniqueTerrainNodeIds.clear();
    static bool seen[MC_MAXTEXTURES];
    memset(seen, 0, sizeof(seen));
    for (const TerrainQuadRecipe& rec : g_denseRecipes) {
        uint32_t nodeId;
        memcpy(&nodeId, &rec._wp2, 4);
        if (!gos_terrain_arm::IsTileHandle(nodeId, static_cast<uint32_t>(MC_MAXTEXTURES))) continue;
        if (!seen[nodeId]) {
            seen[nodeId] = true;
            g_uniqueTerrainNodeIds.push_back(nodeId);
        }
    }
}

// COLORMAP-CPU-RETIRE-1: free cpuColorMap + cpuDispAlpha after atlas upload.
// Default ON. Kill-switch MC2_COLORMAP_CPU_RETIRE=0 reverts (keeps CPU copy alive
// for legacy displacement; needed when MC2_COLORMAP_DISPLACE_PROBE=1).
bool retireCpuColorMap() {
    static const bool s = mc2::gate::get<"MC2_COLORMAP_CPU_RETIRE">();
    return s;
}

// Per-frame counter — incremented when the packer sees a quad whose
// q.terrainHandle is non-zero AND maps to no cement layer.  A non-zero count
// after Stage A.4 is wired indicates an enumeration miss (debug discipline).
uint32_t g_cementPackUnmappedCount = 0;

// Diagnostic Test 1 — per-frame cement classification flip detection.
// Reset at the start of PackThinRecordsForFrame, emitted every 60 frames
// when MC2_TERRAIN_INDIRECT_TRACE is on.
uint32_t g_cementMappedThisFrame       = 0;  // valid cement layer found
uint32_t g_concreteAllCornersThisFrame = 0;  // _wp0 == 3,3,3,3 (genuine pure-cement quad)
}  // namespace gos_terrain_indirect_detail
