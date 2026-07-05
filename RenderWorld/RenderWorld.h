// RenderWorld/RenderWorld.h
//
// Slice M1: engine-facing scene API. The first surface in the
// RenderWorld boundary; takes engine types only.
//
// Spec: docs/superpowers/specs/2026-05-22-renderworld-boundary-spec.md
//       section 4 (lifecycle), section 13 (first-slice scope).
//
// Firewall (spec section 12, load-bearing):
//   - This header MUST NOT include any game-side header.
//   - This header MUST NOT forward-declare any game-side type
//     (no `class Appearance;`, no `class BldgAppearance;`, ...).
//   - Adapters in GameAdapters/ may bridge both sides; this header
//     may not.

#pragma once

#include "../RenderCore/Handle.h"
#include "../RenderCore/RenderObjectDesc.h"
#include "VisibilityRequest.h"

namespace RenderWorld {

// Lifecycle.
//
// Phase 1 / M1: a process-singleton implementation lives in
// RenderWorld.cpp. M1 does not yet model mission scope at the
// RenderWorld layer; static-prop slots are mission-lifetime by virtue
// of the underlying GpuStaticPropRegistry behavior. M2+ promotes
// begin/endMission to a real boundary call.
void init();
void destroy();

// Engine-facing upsert. Adapter calls this after building a StaticPropDesc
// from a game-side Appearance. The desc is MOVED in; ownership of the
// vector inside the desc transfers to the forwarder.
//
// Returns RenderObjectHandle::invalid() on failure. Caller MUST translate
// at the adapter boundary if a legacy sentinel (-1) is expected upward.
RenderCore::RenderObjectHandle upsertStaticProp(RenderCore::StaticPropDesc desc);

// m5 fix (adversarial review pass 2 2026-05-22): late-spawn path. The
// adapter's syncStaticPropLateSpawn calls the legacy
// GpuStaticPropRegistry::registerStaticPropAndReturnRecipe(Appearance*)
// which actually creates the recipe; THIS function wraps the already-
// created recipe index in a Handle so the counter and handle table stay
// honest. Does NOT create a new registry entry. Returns invalid() if
// recipeIndex < 0.
RenderCore::RenderObjectHandle adoptStaticPropRecipe(int32_t recipeIndex);

// Engine-facing destroy. Adapter calls this when a registration must be
// torn down (per-actor invalidate path). No-op on invalid() input.
void destroy(RenderCore::RenderObjectHandle h);

// Engine-facing visibility mark. M1: thin forwarder onto
// GpuStaticPropRegistry::markVisible. lightDataIndex and extentRadius
// match the existing registry signature; default args preserved.
void markVisible(RenderCore::RenderObjectHandle h,
                 uint32_t lightDataIndex = 0xFFFFFFFFu,
                 float    extentRadius  = 0.0f);

// Validity probe (engine-side; useful for debug asserts above the adapter).
bool isReady(RenderCore::RenderObjectHandle h);

// Frame banner emission. Called once per frame from gamecam.cpp's
// frame-end path (Task 14). Emits `[RENDER_WORLD v1]` with the current
// active prop count read from the underlying registry counters.
//
// Env-gated:
//   MC2_RENDER_WORLD_TRACE=1 -> per-frame banner
//   default                  -> monotonic 600-frame summary
void frameBannerTick();

// M1.5: object-ID buffer env-flag accessor. Reads
// MC2_OBJECT_ID_BUFFER once at first call; subsequent calls return the
// cached value. Flipping the env var requires a process restart (the
// linked shader's GLSL macro is fixed at program-load time per
// memory/glsl_preprocessor_does_not_inherit_cpp_build_flags.md).
//
// Consumed by:
//   - gos_postprocess.cpp setSceneDrawBuffers() / createFBOs() / beginScene()
//   - gos_static_prop_batcher.cpp producer (objectIdRaw fill)
//   - RenderWorld.cpp lookupAtPixel() guard
//   - RenderWorld.cpp frameBannerTick() banner token
//   - C++ side of static-prop makeProgram() (gates the GLSL #ifdef prefix)
bool IsObjectIdBufferEnabled();

// GAMEADAPTERS-VISUAL-STATE-BRIDGE: handle index mask.
// RenderObjectHandle packs [19:0]=index, [31:20]=generation. This is the index
// field width, mirrored from RenderCore::Handle (validated by a static_assert
// in RenderWorld.cpp). A GPU pass that reads raw handle bits from the object-ID
// buffer must mask with this before comparing an index. Keep in sync with
// Handle.h; the static_assert fails the build if the layouts diverge.
static constexpr uint32_t kHandleIndexMask = 0xFFFFFu;

// GAMEADAPTERS-VISUAL-STATE-BRIDGE: DELIBERATE, RenderWorld-OWNED classification
// API — the one sanctioned exception to handle opacity. RenderWorld owns the
// handle index partition (mechs occupy indices >= this base; static props /
// terrain are below it — INVARIANT in RenderWorld.cpp), so RenderWorld is the
// correct authority to PUBLISH a classification threshold. Callers do NOT
// reverse-engineer an opaque handle; they consume a partition fact RenderWorld
// chooses to expose (precedent: RenderWorld.cpp iterateHandles uses the same
// base). The object-ID buffer carries raw handle bits, so a postprocess pass
// classifies a pixel as a MECH via:  (rawId & kHandleIndexMask) >= MechHandleIndexBase().
// SCOPE: mechs ONLY. Vehicles render through the static-prop batcher and carry
// static-prop-range handles (below this base) -> they are NOT classified here;
// making vehicles engine-bearing is THERMAL-VIEW-VEHICLE-HOT-1 (needs a distinct
// RenderObjectKind::Vehicle + handle base). Do NOT assume ">= base == any future
// engine kind"; today it means exactly "mech".
uint32_t MechHandleIndexBase();

// M1.6: static-prop pick master enable. When this is OFF, the missiongui
// Shift+click wiring is dormant even if MC2_OBJECT_ID_BUFFER=1. Three-gate
// opt-in stack per spec Section 9; defense-in-depth so a dev can enable
// the substrate for log-driven inspection without changing click behavior.
//
// Process-lifetime cached; restart required to flip. Consumed by:
//   - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick guard
bool IsStaticPropPickEnabled();

// M1.6 + M2.6: static-prop pick verbose-log enable. When this is OFF,
// the `[GAMEPLAY_PICK v1] miss kind=StaticProp ...` line is suppressed
// (high-frequency empty-Shift+click gesture would otherwise spam
// stderr). `hit` lines fire unconditionally per spec Section 7.
//
// Process-lifetime cached. Consumed by:
//   - code/missiongui.cpp MissionInterfaceManager::tryStaticPropPick miss branch
bool IsStaticPropPickDebugEnabled();

// M2.6: master enable for the mech-pick wiring. When OFF, the
// missiongui Shift+click mech wiring is dormant even if
// MC2_OBJECT_ID_BUFFER=1 and MC2_STATIC_PROP_PICK=1. Default OFF.
bool IsMechPickEnabled();

// M2.6: mech-pick verbose-log enable. Gates the miss / gated /
// stale-handle diagnostic logs from the mech caller. `hit` lines
// always fire when MC2_MECH_PICK=1 and the fog gate passes.
bool IsMechPickDebugEnabled();

// M2.6: dev/debug override that allows inspect through sensor fog.
// Default OFF preserves stock gameplay (cannot incidentally reveal
// undetected enemy mechs via the inspect log).
bool IsMechPickPierceFogEnabled();

// GPU_PICK_HOVER_DYNAMIC-1: hover-pick gate (MC2_GPU_PICK_HOVER=1).
// Requires MC2_OBJECT_ID_BUFFER=1. When enabled, updateTarget() uses
// lookupAtPixel to identify mechs under the cursor before falling through
// to the CPU findObjectByMouse scan. Static props still use CPU path.
bool IsGpuPickHoverEnabled();

// GPU_PICK_HOVER_DYNAMIC-1: hover-pick verbose trace (MC2_GPU_PICK_HOVER_TRACE=1).
// Logs attempts/hits/fallbacks from the hover pick block.
bool IsGpuPickHoverTraceEnabled();

// GPU-GROUND-PICK-PARITY-1 (S1): ground-pick parity oracle gate
// (MC2_GPU_GROUND_PICK_PARITY=1). Requires MC2_OBJECT_ID_BUFFER=1 to have a
// live depth+objectID attachment to read. When enabled, the missiongui hover
// and per-click move paths run BOTH the CPU Camera::inverseProject ground pick
// AND the GPU depth-unproject (lookupAtPixel worldX/Y/Z) at the same cursor
// pixel, log the position delta + per-path timing, and RETURN the CPU result
// unchanged (parity-only; GPU result never consumed). Mirrors the
// MC2_TERRAIN_PICK_PARITY oracle discipline. Default OFF; restart to flip.
bool IsGpuGroundPickParityEnabled();

// M1.5 C1 fix: centralize Handle encoding. Returns 0 for invalid
// recipeIndex (< 0). The producer in gos_static_prop_batcher.cpp
// calls this with the result of GpuStaticPropRegistry::getRecipeIndexForType().
uint32_t objectIdRawForStaticPropRecipe(int32_t recipeIndex);

// M2: kind tag for the unified handle/record table. Every RenderObjectHandle
// issued by this module has an associated kind stored in the record.
// The kind disambiguates static props from mechs (and future kinds) when
// a caller examines a handle returned by lookupAtPixel or any other API.
//
// Values are stable across releases (never renumber; only append).
enum class RenderObjectKind : uint8_t {
    StaticProp = 0,
    Mech       = 1,
    // M3 v1 (2026-05-24): RESERVATION ONLY. No writer is wired in v1.
    // See docs/superpowers/specs/2026-05-23-renderworld-slice-m3-terrain-spec.md
    // and the resolutions sidecar
    // docs/superpowers/specs/2026-05-24-renderworld-slice-m3-m4-m5-resolutions.md
    // for the future-trigger contract that would flip M3 to an
    // implementation slice. lookupAtPixel emits a one-shot WARN and
    // returns isValid=false if this kind ever surfaces in a record —
    // that is the trip-wire for an unintended writer. Future terrain
    // variants (water/decal/mine) use a `subKind` payload field, NOT
    // additional RenderObjectKind values.
    Terrain    = 2,
    Vfx        = 3,   // M4: RESERVED. No writer ships in v1.
                      // VFX shaders are PROHIBITED from writing color-attachment-2
                      // (R32_UINT objectID substrate) — last-write-wins on integer
                      // attachments clobbers M2.6 mech-pick under translucent/additive
                      // particles. Enforced by scripts/check-vfx-no-objectid.sh.
                      // See docs/superpowers/specs/2026-05-23-renderworld-slice-m4-vfx-spec.md.
    // Overlay reserved/deferred (M5 2026-05-24): the word "overlay" had
    // 7 in-tree meanings without an identity-needing consumer. See
    // docs/superpowers/specs/2026-05-23-renderworld-slice-m5-overlay-spec.md
    // for the clarification rationale. If a future use case emerges,
    // ship as a new named slice (HoverKindIndicator /
    // RenderWorldDebugOverlay / M5-perf overlay-decal GPU port) — NOT
    // as "M5 Overlay."
};

// M1.5: per-slot inspection record. Indexed by handle.index().
// Always populated (M1 decision: mission/upsert-time RenderWorld
// metadata; ~127 KB peak at tier1 mc2_24 = 2641 props (48 bytes/record
// with M2 kind+debugCookie fields)). Slot recycle bumps generation;
// alive=false marks a retired slot.
//
// Most fields are documentary in M1.5 (PipelineId / DrawPacket /
// pathReasonCode have no real consumers yet); their sentinels are
// returned through LookupResult so M2+ slices can fill them without
// API churn.
struct RenderObjectRecord {
    uint16_t generation       = 0;          // mirrors handle.generation() for staleness check
    uint16_t flags            = 0;          // bit 0: alive
    uint32_t meshHandleBits   = 0;          // RenderCore::MeshHandle bits (sentinel: 0 = unknown)
    uint32_t materialHandleBits = 0;        // RenderCore::MaterialHandle bits (sentinel: 0)
    uint8_t  lodLevel         = 0xFFu;      // 0 = highest, 0xFF = unknown
    uint8_t  pad0             = 0;
    uint16_t pipelineId       = 0;          // M1.5 sentinel: 0 = unknown
    uint32_t drawPacketIndex  = 0xFFFFFFFFu; // M1.5 sentinel
    uint32_t pathReasonCode   = 0;          // M1.5 sentinel: 0 = m1.5-static-prop-indirect
    uint32_t gameObjectId     = 0;          // optional engine-side cookie
    // M2: kind tag. Populated by registerMech (kind=Mech) and upsertStaticProp
    // (kind=StaticProp). lookupAtPixel callers MUST check kind before consuming
    // kind-specific fields.
    RenderObjectKind kind     = RenderObjectKind::StaticProp;  // default for M1 legacy slots
    // M2: opaque debug cookie. Stored for log output; never dereferenced by engine.
    // For mechs: reinterpret_cast<uintptr_t>(&mech3DAppearance). For static props: 0.
    uintptr_t debugCookie     = 0;
};

static constexpr uint16_t kRenderObjectFlagAlive = 1u << 0;

// M1.5: result of lookupAtPixel. isValid=false on background pixel or
// generation mismatch (stale-pixel-after-destroy). Caller MUST check
// isValid before consuming any other field.
struct LookupResult {
    bool                            isValid          = false;
    RenderCore::RenderObjectHandle  handle           = RenderCore::RenderObjectHandle::invalid();
    uint32_t                        meshHandleBits   = 0;
    uint32_t                        materialHandleBits = 0;
    uint8_t                         lodLevel         = 0xFFu;
    uint16_t                        pipelineId       = 0;
    uint32_t                        drawPacketIndex  = 0xFFFFFFFFu;
    uint32_t                        pathReasonCode   = 0;
    uint32_t                        gameObjectId     = 0;
    // M2.6: kind discriminator copied from RenderObjectRecord.kind.
    // Caller MUST check this before consuming kind-specific fields
    // (recipeIndex for StaticProp; BattleMech reverse-lookup for Mech).
    // Defaults to StaticProp to preserve M1.6 caller behavior on an
    // isValid=false return (callers should gate on isValid first
    // anyway; the default is only relevant for compile-time
    // initializer compatibility).
    RenderObjectKind                kind             = RenderObjectKind::StaticProp;
    // World position from depth-buffer readback + inverse-VP unproject.
    // worldPosValid=false when depth==0 (sky/far plane), inverseViewProj
    // unavailable, or object-ID buffer disabled.
    bool                            worldPosValid    = false;
    float                           worldX           = 0.f;
    float                           worldY           = 0.f;
    float                           worldZ           = 0.f;
    // M3-inspector: raw 32-bit pixel value read from attachment-2.
    // Populated even when isValid=false (0 = background / OID disabled).
    // Bits[19:0]=index, bits[31:20]=generation (same as handle.bits).
    uint32_t                        rawObjectId      = 0u;
    // M3-inspector: static string literal describing why isValid is false.
    // nullptr when isValid=true.
    const char*                     lookupFailReason = nullptr;
};

// MODDER-DEBUG-1: handle-direct object record query.
//
// Returns a snapshot copy of the CPU-side record for `handle`. No GPU read-back,
// no pixel stall. Returns true and fills `*out` on success.
// Returns false on null out-pointer (no fill). Zero-fills `*out` on non-null
// failure (invalid handle, generation mismatch, dead slot).
//
// Rules:
//   - Generation check required. Stale handles (destroyed objects) return false.
//   - Returns by value copy only — no reference to internal RenderObjectRecord.
//   - No GL calls. No batcher internals. Pure CPU table read under mutex.
//   - Intended for: ImGui inspector, editor queries, scripting, log output.
//   - NOT a per-frame call. Use at click/inspect rate only.
//
// Field types: all integer fields are uint32_t regardless of the underlying
// record storage width (uint8_t lod, uint16_t pipelineId) — normalized for
// API uniformity and ImGui display. Sentinel values are preserved exactly.
struct ObjectRecordView {
    bool             valid              = false;
    RenderObjectKind kind               = RenderObjectKind::StaticProp;
    RenderCore::RenderObjectHandle handle;
    uint32_t         meshHandleBits     = 0;
    uint32_t         materialHandleBits = 0;
    uint32_t         lod                = 0xFFu;   // 0 = highest; 0xFF = unknown
    uint32_t         pipelineId         = 0;       // 0 = unknown; 1 = Opaque; 2 = AlphaTest
    uint32_t         drawPacketIndex    = 0xFFFFFFFFu;
    uint32_t         pathReasonCode     = 0;
    uint32_t         gameObjId          = 0;
};

bool getObjectRecordView(RenderCore::RenderObjectHandle handle, ObjectRecordView* out);

// M1.5: synchronous pixel -> handle lookup. screenX/Y in GL convention
// (origin bottom-left). Returns LookupResult{isValid=false} when env-OFF,
// FBO not initialized, pixel==0, or generation mismatch. Stalls the GPU
// to read the prior frame's attachment-2 -- intended for click-time
// (max ~10/sec) debug; not per-frame.
//
// Spec: 2026-05-23-renderworld-slice-m1-5-objectid-buffer-spec.md sec 7
LookupResult lookupAtPixel(int screenX, int screenY);

// M2.6 (META-FIX of the M1.6 per-kind selection debug state): most-recent
// gameplay pick debug state. Single mutex-guarded slot; latest pick
// across all kinds wins. Retires the per-kind state-slot pattern that
// would otherwise multiply at M3 (terrain) and M4 (VFX).
//
// Kind-specific payload:
//   kind == StaticProp -> recipeIndex carries the recipe (M1.6 semantic)
//   kind == Mech       -> handle alone carries the identity. Callers
//                          re-resolve via GameAdapters::Mech::findMechByHandle
//                          to avoid stale-pointer dereference after destroyMech.
//                          (Adversarial CRITICAL-1: no stable cookie at
//                          syncSpawn -- partId reassigned post-init at
//                          code/mission.cpp:2987. Handle IS the identity.)
//   future kinds       -> add a tagged-union payload field at that
//                          slice; do NOT widen the struct prematurely.
//
// Cleared on per-mission RenderWorld::destroy() (same lifecycle as the
// retired M1.6 selection debug state).
//
// Spec: 2026-05-23-renderworld-slice-m2-6-mech-pickup-spec.md sec 4.3.
struct GameplaySelectionDebugState {
    bool                            valid              = false;
    RenderObjectKind                kind               = RenderObjectKind::StaticProp;
    RenderCore::RenderObjectHandle  handle             = RenderCore::RenderObjectHandle::invalid();
    int32_t                         recipeIndex        = -1;  // kind==StaticProp only; -1 otherwise
    int32_t                         lastPickMouseX     = 0;   // Win32 origin top-left
    int32_t                         lastPickMouseY     = 0;
    int32_t                         lastPickGlX        = 0;   // GL origin bottom-left
    int32_t                         lastPickGlY        = 0;
    uint64_t                        lastPickFrameIndex = 0;
};

// M2.6: populate from a valid LookupResult. Caller passes the
// pre-checked kind (must match res.kind). Kind-specific payload
// extracted from the appropriate source:
//   kind==StaticProp: recipeIndex sampled from handleToRecipeIndex
//                     (existing M1.6 path)
//   kind==Mech:       no kind-specific payload; handle alone identifies
//                     the mech (CRITICAL-1 note above).
void setLastGameplayPick(RenderObjectKind kind,
                         const LookupResult& res,
                         int32_t mouseX, int32_t mouseY,
                         int32_t glX,    int32_t glY);

// M2.6: reset to default (valid=false). Idempotent. Called on
// (a) empty Shift+click from the static-prop caller, and
// (b) per-mission RenderWorld::destroy() lifecycle hook.
void clearLastGameplayPick();

// M2.6: read-only access. Caller MUST check .valid before consuming
// any other field; then dispatch on .kind for payload semantics.
GameplaySelectionDebugState getLastGameplayPick();

// M2: mech spawn descriptor. Engine types only -- no Mech3DAppearance*,
// no Mech3DAppearanceType*. Firewall: spec section 12 + M2 spec section 10.
//
// mechTypeId: 0 in M2 by design (type identity deferred to M2.5).
//   RenderWorld MUST NOT dereference it.
//
// gameObjectId: opaque uint32_t; the engine-side echo of a game-side
//   identifier. Never dereferenced by RenderWorld. Used for future
//   object-ID correlation in M2.5.
//
// debugCookie: opaque uintptr_t; never dereferenced by engine. Carries
//   the raw Mech3DAppearance* echo for log output in MC2_RENDER_WORLD_TRACE
//   builds. RenderWorld stores it in the record but never casts or follows it.
struct RenderMechDesc {
    uint32_t  mechTypeId;    // 0 in M2; real value deferred to M2.5
    uint32_t  gameObjectId;
    uintptr_t debugCookie;
};

// M2: register a mech with RenderWorld. Returns a new RenderObjectHandle
// on success; invalid() on failure (OOM or internal error).
//
// MUST NOT be called upsert-style (no overwrite of an existing handle).
// This is spawn-only: if the caller's handle is already valid, it means
// a prior destroyMech was missed. The adapter asserts on this in debug.
//
// Route-only in M2: no new GPU path. RenderWorld records the handle in
// the unified s_objectRecords table with kind=Mech; the handle is valid
// from this call until destroyMech.
RenderCore::RenderObjectHandle registerMech(RenderMechDesc desc);

// M2: retire a mech handle. No-op on invalid() input.
//
// AUTHORITATIVE handle retirement path. After this call the handle is
// invalid; any subsequent use of the old handle with lookupAtPixel or
// any future API returns invalid/false.
//
// endMission() force-clears remaining live mech records after logging
// a warning; correctness of per-mech destroy during normal play must
// not depend on endMission() being called.
void destroyMech(RenderCore::RenderObjectHandle h);

// M2: force-clear all live mech records in the unified table. Called by
// GameAdapters::Mech::endMission() after logging a leaked-handle warning.
// Marks every record with kind=Mech and alive=true as alive=false,
// bumps generation, and decrements s_mechs_alive_rw for each.
void clearAllMechRecords();

// M2.6: read-only accessor for the engine-side live-mech counter
// (sourced from MechRenderAdapter via registerMech/destroyMech). Used
// by RunMechPickSelfTest in GameAdapters/MechRenderAdapter.cpp to
// assert no drift across a synthetic register+destroy pair. Returns
// a relaxed-load uint64_t snapshot; no synchronization across
// multiple consumers.
uint64_t getMechsAliveCount();

// Seq C step 1 — VisibilityRequest v0: reporting-only cull query.
//
// Wraps existing visibility/cull facts. No new culling decisions;
// no draw-submission change. Output is counts by kind only.
//   static_props: GpuStaticPropRegistry active-recipe count
//   mechs:        engine-side alive-mech counter
//   terrain:      deferred (CPU terrain picking remains canonical)
//   vfx:          prohibited (no stable object identity in v0)
//
// req.viewId / req.kindMask / req.layerMask are stored but not
// acted upon in v0 -- present for API stability going into v1.
//
// Env-gated log: MC2_RENDER_WORLD_TRACE=1 -> per-frame [VISIBILITY v1];
// default -> 600-frame monotonic summary (same gate as frameBannerTick).
VisibilityResult queryVisibility(VisibilityRequest req);

// --- Extraction v1: read-only enumeration (render_snapshot.cpp consumer) ---

struct StaticPropRecordView {
    RenderCore::RenderObjectHandle handle;          // invalid() if slot is dead
    int32_t                        recipeIndex;     // == handle.index() == slot index in s_objectRecords.
                                                     // Equals the GpuStaticPropRegistry recipeIndex by construction
                                                     // (handleToRecipeIndex is an identity mapping per RenderWorld.cpp:85-88).
                                                     // -1 if dead.
    bool                           alive;           // true iff flags bit 0 is set
    bool                           generationValid; // invariant: always equals alive (handle is constructed from
                                                     // current record generation, so it is self-consistent for live slots)
};

// Returns current allocated StaticProp slot count (alive + dead).
// WARNING: The count can change between this call and fillStaticPropSlots().
// Use for initial buffer sizing only. Always check fillStaticPropSlots() return
// value: if total > capacity, resize and retry.
uint32_t getStaticPropSlotCount();

// One-pass fill: writes up to `capacity` StaticProp slots into out[0..].
// Returns TOTAL matching StaticProp slot count (may exceed capacity if buffer
// is too small). Caller detects truncation via: total > capacity — in that case,
// resize buffer to `total` and call again. Mutex is held for the duration of
// the fill but NOT between getStaticPropSlotCount() and this call.
// Handles are generation-validated internally; callers must not re-read s_objectRecords.
// Only processes slots where kind == StaticProp.
uint32_t fillStaticPropSlots(StaticPropRecordView* out, uint32_t capacity);

} // namespace RenderWorld
