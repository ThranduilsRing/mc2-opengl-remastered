<!-- doc-status: current | reviewed:2026-10-11 -->
# Engine Standalone Seams — `ENGINE-STANDALONE-SEAMS-RECON-1`

**Status:** recon / design only. No production code changes. Companion branch
`claude/engine-standalone-seams-recon-1` (off `claude/nifty-mendeleev` @ 29aebfe5).

**One-line conclusion:** Worth doing now — but only for the **render/asset-preview
ring**, and most of the seam *already exists*. `RenderCore/` and `RenderWorld/` are
already firewalled, GL-free, and Mission-free (CI-enforced). The work is plumbing a
small host-services contract and extracting two welded-in pieces, **not** a rewrite.

> **Clarification.** RenderWorld is game/Mission-free and largely GL-free by contract,
> but it still has one backend-direction dependency on `GameOS/gameos/gos_postprocess.h`. That
> is the hard blocker **B1** and the first Ring-2 cleanup.

> **Note (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).** A modern-vs-legacy
> pass-routing section has been added below (see "Modern-spine vs legacy pass
> routing" + "RenderWorld API-spine inventory"). The key correction: RenderWorld
> owns **identity / lifecycle / visibility-reporting** but **NOT dispatch**. The
> modern GPU batchers (static-prop, mech) still *flush inside the legacy driver*
> `MC_TextureManager::renderLists()` (`mclib/txmmgr.cpp:2251-3679`; static-prop
> flush `:3081`, mech flush `:3097`, shadow lanes `:2789-2812`). The "agnostic
> spine" is a description/visibility layer, not a render-graph executor.
> Cross-reference `docs/render-backend-seams/render-contract-index-1.md`.

---

## Goal

Make MC2's render/asset-preview engine layer **caller-agnostic**: usable by `mc2.exe`,
`mc2_asset_viewer.exe`, the mission editor, future Blender tooling, and headless
test/smoke harnesses, **without** any of them having to start a `Mission` or rely on
`GameObjectManager` / global-camera singletons.

Caller-agnostic means the engine requires only a small host contract:

```text
Host provides:                    Engine provides:
- GL context (windowed/headless)  - renderer init/shutdown
- filesystem / asset resolver     - asset load/cook hooks
- logging sink                    - preview scene API
- time / frame dt                 - render-frame API
- runtime config / env gates      - debug/capture hooks
- (optional) ImGui surface        - validation / errors
- (optional) input events
```

## Non-goals

1. Full `Mission` runtime detached from the game.
2. Gameplay sim / AI / campaign logic as a reusable library.
3. Editor runtime with level placement.
4. Material graph/editor.
5. Making every legacy MLR/VFX path standalone before the viewer needs it.
6. **No** asset-format decisions, **no** renderer behavior changes, **no**
   `Mission`/`GameObjectManager` rewrite, **no** `git add -A`.

---

## What recon found (load-bearing facts)

The singleton-heavy reputation is mostly wrong for the **render** layer. Evidence:

- **`RenderCore/` + `RenderWorld/` are already a firewalled, GL-free, caller-agnostic
  seam.** Zero hits for `Mission::instance`, `ObjectManager`, `Appearance`, global
  camera inside those dirs. Proven by `tests/unit/test_rendercore.cpp` (header: "GL-free
  unit tests… No game startup, no GL context, no asset loading required") and
  machine-enforced by `scripts/check-include-firewall.sh:22-33` (forbids `GL/glew.h`,
  `Stuff/Stuff.hpp`, `mission.h`, `objmgr.h`, `appear.h`, `mech3d.h` + symbols `Mission`,
  `ObjectManager`, `*Appearance`, `MechWarrior` in scope dirs).
- **The camera is *pushed* into the renderer as data, not pulled.**
  `GameOS/gameos/gameos_graphics.cpp` has **zero `eye->` references**. The game packs
  3 matrices into a `ViewUniforms` POD and calls `RenderCore::setCurrentView` /
  `uploadViewUniforms` (`code/gamecam.cpp:192-204,277-278`). `RenderCore::EngineView`
  (`RenderCore/EngineView.h:61-69`) + `ViewUniforms` (144-byte ABI;
  `RenderCore/ViewUniforms.h`, binding 3) **is** the caller-agnostic camera seam, and
  it already exists.
- **RenderCore registries need no init** — zero-initialized process statics, valid at
  first use (`PipelineRegistry`, `RenderResourceRegistry`, `RendererFeatureRegistry`,
  `IblShRegistry`). `RenderWorld::init()` (`RenderWorld.cpp:522-564`) only reads env
  gates + prints banners; no game globals.
- **The asset viewer is already a standalone host.** `mc2_asset_viewer`
  (`tools/asset_viewer/`) owns its own SDL2 GL context + GLEW + ImGui, links **none** of
  RenderCore/RenderWorld/mclib/game code, and has zero Mission coupling. It already
  defines the backend-agnostic seam `PreviewSurface.h` (Stage1=`TexturePreview2D`,
  Stage2=`MaterialPreviewPBR`, Stage3=`ModelPreviewRenderCore`).
- **A reusable include-splicing shader compiler exists**:
  `GameOS/gameos/utils/shader_builder.cpp` — `load_shader()` + recursive
  `parse_includes()` (`:262-308`), GL-context-only, no Mission. Shaders carry **no
  `#version`**; it's supplied as a prefix string at compile (`:393`), as are feature
  defines (`MC2_COALESCE`, `MC2_USE_VIEW_UNIFORMS`, …). **Not currently compiled into the
  viewer target** — Backend A must add it.
- **`KtxLoader` + `MaterialGpu` are standalone-safe.** `RenderCore::ktxLoadRgba8`
  (`RenderCore/KtxLoader.h:44`) is pure CPU KTX2/BC7 parse, no globals/GL/Mission.
  `MaterialGpu` (`RenderCore/MaterialGpu.h:88`) is a 32-byte std430 slot struct
  (albedo/normal/ORM/emissive + PBR scalars), GLSL-mirrored and cook-manifest-backed.

### The blockers (small, concentrated)

| # | Blocker | Where | Severity |
|---|---|---|---|
| B1 | **RenderWorld → GameOS downward include** (`#include "../GameOS/gameos/gos_postprocess.h"`) — the "agnostic" module depends on the GL backend. Firewall guards *upward* (game types) but not *downward*. | `RenderWorld/RenderWorld.cpp:28` | **HARD** — needs a backend interface, not a re-route |
| B2 | `mcTextureManager` reached as a **global pointer** from many `mclib/` sites (class itself is already construct-anywhere: 5 ctor sites incl. `editor/`, `Viewer/`). | `mclib/txmmgr_internal.h:78`, `txmmgr.h:1310` | MODERATE — thread a handle or host-inject |
| B3 | Per-call **behavioral env gates** inside draw fns (terrain lighting v1/v2, water reflection/skytint). | `gameos_graphics.cpp:5395-5719,2274-2356` | MODERATE — become explicit render-config fields |
| B4 | `eye->` reach-ups inside the legacy flush (scene-UBO fog + shadow dynamic-projection box). | `mclib/txmmgr.cpp:1770,1807-1818,2039` | MODERATE — feed from an `EngineView`/scene-params struct |
| B5 | **Logging**: RenderWorld logs via hard-coded `fprintf(stderr)` (20+ sites); engine-wide uses `SPEW`/`gosASSERT`. No host-logger callback. | `RenderWorld.cpp:215+` | MODERATE — route through injected sink |
| B6 | Material-table build/upload/bind **welded into the static-prop batcher** (file-scope statics + `mcTextureManager`). | `GameOS/gameos/gos_static_prop_batcher_mapload.cpp:288-439` | MODERATE — extract to a RenderCore service |
| B7 | Filesystem path resolution is a **global** (`fastFiles[]` registry + `CDInstallPath`). Escape hatch: `File::open` works on raw disk path with no registry; `KtxLoader`/BC7 sidecar already use raw `fopen`. | `mclib/fastfile.cpp:17-74`, `file.cpp:90-99` | MODERATE — accept resolved abs path; FastFile = optional provider |

Trace-only env gates (`static const bool`, read-once) are TRIVIAL: hoist to an
init-time config struct. `RenderWorld` already models the pattern with
`envFlag()`/`envFlagDefaultOn()` cached accessors (`RenderWorld.cpp:64-72`).

---

## Host services proposal

Introduce **interfaces/types only** (no behavior migration). The renderer doesn't read
config/clock today, so these are thin from day one.

```cpp
// New, header-only contract — no game includes, no GL includes.
namespace mc2host {

struct IFileSystem {
    // Resolved-absolute-path in; bytes out. FastFile is an optional provider.
    virtual bool   exists(const char* path) const = 0;
    virtual bool   read(const char* path, std::vector<uint8_t>& out) const = 0;
    virtual ~IFileSystem() = default;
};

struct ILogger {
    enum class Level { Trace, Info, Warn, Error };
    virtual void log(Level, const char* category, const char* msg) = 0;
    virtual ~ILogger() = default;
};

struct IClock {
    virtual double elapsedSeconds() const = 0;   // wraps gos_GetElapsedTime
    virtual ~IClock() = default;
};

struct IConfig {
    // Replaces scattered getenv("MC2_*"). Read-once at init.
    virtual bool        flag(const char* key, bool dflt) const = 0;
    virtual const char* str (const char* key, const char* dflt) const = 0;
    virtual ~IConfig() = default;
};

struct GLContextInfo {   // host already has a current GL context
    int   glMajor, glMinor;
    bool  coreProfile;
    bool  hasBPTC;        // GLEW_ARB_texture_compression_bptc, for BC7
};

struct HostServices {
    IFileSystem* files;
    ILogger*     log;
    IClock*      clock;
    IConfig*     config;
};

} // namespace mc2host
```

**Migration order is mechanical, not behavioral:** `IClock` wraps the single existing
`gos_GetElapsedTime` API; `IConfig` replaces `getenv("MC2_*")` reads; `ILogger` gives
RenderWorld's `fprintf(stderr)` (B5) a real sink; `IFileSystem` takes a resolved path
(B7). None require Mission.

---

## RenderCore standalone-init requirements

To drive one frame with no Mission, a host supplies exactly three things — none are
game globals:

1. **A current GL context.** GameOS already creates one Mission-independently via SDL
   (`gos_render.cpp:340-361` → `SDL_GL_CreateContext`; `GameOS/gameos/gameosmain.cpp:1572` `glewInit`).
   The asset viewer proves a tool can create its own (`tools/asset_viewer/main.cpp:23-73`).
   Pure-RenderCore (registries, PipelineDesc, view POD, KTX parse, SH lookup) runs with
   **no context at all**.
2. **View data.** Fill `ViewUniforms` (`worldToClipGL[16]`, `worldToViewGL[16]`,
   `cameraWorldPos[4]`) + a viewport, push via `setCurrentView` / `uploadViewUniforms`.
   No `Camera` class. The transpose lambda at `gamecam.cpp:192-204` is the entire contract.
   GL upload entry: `RenderCore::initViewUniformsUbo()` (`view_uniforms_gl.cpp:27-37`),
   needs only a current context.
3. **Scene content** via `RenderWorld::upsertStaticProp` / `registerMech` (engine POD
   descs). The only Mission coupling is the `RenderWorld/legacy/static_prop_backend` →
   `GpuStaticPropRegistry` reach-through and the `GameAdapters/` that translate game
   `Appearance`/`BattleMech` into POD descs — both already isolated behind the firewall
   and marked TEMPORARY.

Precedent: `EditorBridge/EditorRenderBridge.h` (`init()` reads only `MC2_EDITOR_MODE`)
is an existing non-game host driving this substrate.

**Open item gating Backend A:** a *lit* draw needs more than init — a shader program +
its uniform/SSBO expectations + light setup. Must be proven by a running spike before
committing to "real shader standalone" (see Slice 2 / Stage-2 Task 0).

---

## PreviewScene API shape

The asset viewer's existing `PreviewSurface` seam is the right abstraction. A
PreviewScene sits behind it and **must not** know about Mission / GameObjectManager /
gameplay objects.

```cpp
namespace mc2preview {

struct CameraDesc { float fovY, aspect, nearZ, farZ; float orbitYaw, orbitPitch, dist; };
struct LightDesc  { float dirWorld[3]; float color[3]; float intensity; float ambient[3]; };

struct MaterialDesc {                       // slot-aware; explicit colorspace
    const char* baseColorPath;  // sRGB
    const char* normalPath;     // linear
    const char* ormPath;        // linear, R=AO G=Roughness B=Metallic
    const char* emissivePath;   // sRGB
    float baseColorFactor[4], metallicFactor, roughnessFactor;
    uint32_t flags;             // mirrors MaterialGpu::flags
};

struct MeshDesc { /* sphere | static-prop | model handle */ };

class PreviewScene {
public:
    bool init(const mc2host::HostServices&, const mc2host::GLContextInfo&);
    void setCamera(const CameraDesc&);
    void setLight(const LightDesc&);
    void setMaterial(const MaterialDesc&);   // builds MaterialGpu, loads slots
    void setMesh(const MeshDesc&);
    void renderToFbo(uint32_t fbo, int w, int h);  // save/restore GL state
    void shutdown();
};

} // namespace mc2preview
```

Hard rules carried from Stage-2 spec: per-slot colorspace (albedo/emissive sRGB,
normal/ORM linear; ORM = R:AO G:Rough B:Metal); offscreen FBO → `ImGui::Image` with
full GL-state save/restore and `GL_FRAMEBUFFER_COMPLETE` check; generate tangents in the
mesh (`static_prop.vert` has **no** tangent attribute); broad-tolerance smoke, no
exact-pixel goldens.

---

## Systems table

| System | File(s) | Class |
|---|---|---|
| `EngineView` / `ViewUniforms` POD | `RenderCore/EngineView.h`, `ViewUniforms.h` | **STANDALONE-SAFE** |
| PipelineDesc + PipelineRegistry | `RenderCore/PipelineDesc.h`, `PipelineRegistry.*` | **STANDALONE-SAFE** |
| RenderResourceRegistry / FeatureRegistry / IblShRegistry / RenderDebugView | `RenderCore/*` | **STANDALONE-SAFE** |
| KtxLoader (CPU KTX2/BC7 parse) | `RenderCore/KtxLoader.*` | **STANDALONE-SAFE** |
| MaterialGpu struct + cook manifest | `RenderCore/MaterialGpu.h`, `tools/material_cook/` | **STANDALONE-SAFE** |
| Handle / DrawPacket / FrameArena / MechVisualState | `RenderCore/*` | **STANDALONE-SAFE** |
| Shader include-splicer | `GameOS/gameos/utils/shader_builder.cpp` | **STANDALONE-SAFE** (GL-only; not yet linked into viewer) |
| Asset viewer Stage 1 + `PreviewSurface` | `tools/asset_viewer/*` | **STANDALONE-SAFE** (already a standalone host) |
| ViewUniforms UBO upload + EngineView registry | `GameOS/gameos/view_uniforms_gl.*` | NEEDS-ADAPTER (current GL context; no Mission) |
| PipelineDesc → GL apply | `GameOS/gameos/pipeline_binder.*` | NEEDS-ADAPTER (GL context only) |
| RenderWorld scene/handle/visibility | `RenderWorld/RenderWorld.*` | NEEDS-ADAPTER (init pure; impl pulls `gos_postprocess.h` — see B1) |
| GL context + window (SDL) | `gos_render.cpp:340`, `gameosmain.cpp:1019` | NEEDS-ADAPTER (Mission-independent; reusable as host service) |
| `loadTexture` CPU cache | `mclib/txmmgr_lightrow.cpp:701` | NEEDS-ADAPTER (needs `File` + `mcTextureManager` injected) |
| GL texture realize | `mclib/txmmgr_render_phases.cpp:385` | NEEDS-ADAPTER (GL context + `g_gos_renderer` + `mcTextureManager`) |
| BC7 uploaders (2D + array) | `mclib/txmmgr_render_phases.cpp:480`, `GameOS/gameos/gos_static_prop_batcher_program.cpp:787` | NEEDS-ADAPTER (GL context + BPTC) |
| Material-table assembly/upload/bind | `GameOS/gameos/gos_static_prop_batcher_mapload.cpp:288-439` | NEEDS-ADAPTER (extract from batcher; reaches `mcTextureManager`) |
| sRGB/linear policy | `gl_utils.cpp:53-64`, batcher `:2790` | NEEDS-ADAPTER (inconsistent; make explicit per-slot) |
| Filesystem path resolution | `mclib/fastfile.cpp`, `file.cpp` | NEEDS-ADAPTER (global registry; accept resolved abs path) |
| EditorBridge | `EditorBridge/EditorRenderBridge.h` | NEEDS-ADAPTER (non-game host precedent) |
| Camera matrix producer | `code/gamecam.cpp:183-232` | MISSION-BOUND (uses game `Camera`; output POD is the seam) |
| Static-prop legacy backend | `RenderWorld/legacy/static_prop_backend.*` | MISSION-BOUND (grandfathered reach-through; TEMPORARY) |
| GameAdapters (Mech/StaticProp) | `GameAdapters/*` | MISSION-BOUND (by design — bridges game types) |
| `static_prop` shader as preview shader | `shaders/static_prop.{vert,frag}` | MISSION-BOUND for preview use (batcher-SSBO-bound, no tangent/normal/ORM sampling) |
| `MaterialGpu` normal/ORM/emissive slots | `MaterialGpu.h` (fields exist, all `kMaterialTexAbsent`) | DEFER (greenfield; gated on mech texture-identity decision) |
| Shader reflection / schema CI | `tools/shader_reflect/` | DEFER (offline tooling, not a runtime dep) |
| Mission / ObjectManager / AI / campaign | `code/*` | DEFER (Ring 3, out of scope) |

---

## Modern-spine vs legacy pass routing (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1)

RenderWorld/RenderCore/GameAdapters is the *identity & visibility* spine, not a
render-graph executor. `RenderPassContract.h` is descriptive ("NOT a scheduler... the
imperative frame loop continues to call each pass-owner's draw functions directly",
`RenderPassContract.h:12-15`). All passes are still *dispatched* by the legacy
enqueue/flush loop (`code/gamecam.cpp` enqueue; `mclib/txmmgr.cpp:2251-3679` flush;
post-process after `gos_RendererEndFrame`).

| Pass (PassIdentity) | RenderPassId | Owner subsystem | Identity/visibility spine | Dispatch driver | ViewUniforms (b=3) | PipelineDesc | Snapshot-authoritative |
|---|---|---|---|---|---|---|---|
| StaticProp | StaticPropOpaque (1) | GpuStaticPropBatcher | RenderWorld (upsert/markVisible/handle) | legacy renderLists() flush @ txmmgr :3081 | yes | yes | yes |
| OpaqueObject (mechs) | MechOpaque (3) | GpuMechBatcher | RenderWorld route-only (registerMech, M2) | legacy renderLists() flush @ txmmgr :3097 | yes | yes | yes |
| ShadowCaster | Shadow (4) | gosPostProcess + per-lane shadow programs | partial (static-prop/mech lanes via batchers) | legacy renderLists() flush @ txmmgr :2789-2812 | no | no | no |
| TerrainBase | Terrain (2) | TerrainPatchStream / gos_terrain_indirect | legacy (CPU slim cull+reduction) | legacy renderLists() | no (passive snapshot row only) | no | no |
| ParticleEffect | VFX (5) | mc2::particles::Batcher | none (object-ID prohibited) | post-renderLists, gamecam | no | no | no |
| OpaqueObject (vehicles/legacy buildings) | MechOpaque (lossy) | static-prop batcher / TGL | partial / none | legacy renderLists() | mixed | mixed | partial |
| AlphaObject | (orphan) | TGL / MLR | none | legacy / MLR immediate | no | no | no |
| TerrainOverlay / TerrainDecal | (orphan) | quad.cpp M2d producer | none | legacy renderLists() | no | no | no |
| Grass / VegetationCards | (orphan) | gos_postprocess / VegetationAdapter | none | post-renderLists, gamecam :520 | no | no | no |
| Water | (orphan) | quad.cpp / renderWaterFastPath | none (intentional projected, Bucket B1) | legacy + post-renderLists fastpath :512 | no | no | no |
| UI / HUD / text | (orphan) | GameOS 2D | none | post-renderLists | no | no | no |
| PostProcess | (orphan) | gos_postprocess | none | after gos_RendererEndFrame | partial | no | no |
| DebugOverlay | (orphan) | various | none | various | no | no | no |

## RenderWorld API-spine inventory (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1)

- RenderWorld API (`RenderWorld/RenderWorld.h`): lifecycle init/destroy (:31-32),
  upsertStaticProp (:40), adoptStaticPropRecipe (:49), destroy(handle) (:53),
  markVisible (:58), isReady (:63), frameBannerTick (:72), registerMech/destroyMech
  (:387/:398), queryVisibility (:428), extraction getStaticPropSlotCount/fillStaticPropSlots
  (:447/:456). Owns: unified handle/record table (RenderObjectRecord :205, kind tag :166),
  pick/lookup (lookupAtPixel :303), counters. Delegates: all GPU realize/draw to
  GpuStaticPropRegistry/batchers.
- VisibilityRequest.h: v0 reporting-only — counts by kind, NOT authoritative culling,
  does NOT feed draw submission (:5-21). terrain=deferred, vfx=prohibited.
- GameAdapters bridged today: StaticProp (StaticPropRenderAdapter.h — full lifecycle+sync),
  Mech (MechRenderAdapter.h — route-only/M2, handle register/destroy; draw still
  GpuMechBatcher), Sky (SkyRenderAdapter.h — route-only, no handle, HDRI fullscreen).
  NOT bridged (bypass to txmmgr/tgl/legacy): terrain, water, overlays/decals,
  grass/vegetation, particles/VFX, UI/HUD, post-process, legacy TGL/MLR.
- RenderCore registries (all GL-free, zero-init process statics): PipelineRegistry,
  RenderResourceRegistry (RenderResourceId enum, 8 slots, :10-21), RendererFeatureRegistry,
  IblShRegistry, EngineView/ViewUniforms (binding=3, 144B).
- GL-free firewall: scripts/check-include-firewall.sh + scripts/check-no-raw-gl-from-game.sh.
  Proven GL-free: RenderCore/, RenderWorld/ headers. Residual B1: RenderWorld/RenderWorld.cpp:28
  includes ../GameOS/gameos/gos_postprocess.h (backend-direction dependency — the hard
  blocker). B5: RenderWorld logs via fprintf(stderr).

---

## First implementation slices

### Slice 1 — `ENGINE-HOST-SERVICES-0`
Introduce **interfaces/types only**, no behavior migration: `mc2host::HostServices`
(`IFileSystem`, `ILogger`, `IClock`, `IConfig`, `GLContextInfo`). Header-only, no game/GL
includes; add to the include-firewall scope so it stays clean. Provide a trivial
default impl (disk filesystem, stderr logger, `gos_GetElapsedTime` clock, `getenv`
config) so existing callers are untouched.
*Exit:* compiles; `test_rendercore`-style GL-free unit test instantiates the defaults.
**DONE** (`claude/engine-host-services-0`): `HostServices/{HostServices,DefaultHostServices}.*`
+ GL-free doctest suite "HostServices" (9 cases / 33 assertions); `HostServices` added to
the include-firewall scope.

### Slice 2 — `RENDERCORE-STANDALONE-INIT-0`
A tiny exe/test that, with **no Mission**: creates a GL context (reuse SDL path),
`initViewUniformsUbo()`, fills a `ViewUniforms` + `EngineView`, `setCurrentView`,
binds a `PipelineDesc`, clears + draws a pass marker, reads back center pixel, shuts
down clean (`glGetError()==0`). This is the **Stage-2 Task 0 spike** — it answers
"can the real RenderCore init/link/bind come up without a Mission?" (NOT "which backend
renders the material ball" — see the scope boundary below).
*Exit:* non-black readback + clean GL error, zero Mission/ObjectManager symbols linked.
**DONE — RenderCore init/link standalone confirmed** (`claude/rendercore-standalone-init-0`):
`tools/rendercore_standalone_spike/` (since deleted; recover from tag `archive/dead-infra-2026-10-09`) linked against ONLY SDL2 + GLEW + opengl32 + three
game-free engine TUs (`view_uniforms_gl.cpp`, `pipeline_binder.cpp`,
`RenderCore/PipelineRegistry.cpp`) + the Slice-1 host seam — no Mission/game libs. Runs
green on GL 4.4 (7900 XTX): init UBO → `ViewUniforms`/`EngineView` upload+register →
`applyPipeline(StaticPropOpaque)` → offscreen FBO clear → center pixel matches clear color,
`glGetError()==0`, exit 0. The view+pipeline substrate has zero Mission/game *link*
dependency, empirically confirming the static-closure recon.

> **Scope boundary — what the spike does and does NOT prove.**
> PROVES: RenderCore **init + link + pipeline-bind** are standalone — clean link with no
> Mission/game, view-as-data path works, `applyPipeline` raises no GL error, FBO
> clear→readback is exact. This de-risks "the asset viewer can link real RenderCore."
> Does NOT prove: a **mesh drawn through `static_prop` producing material output**. The
> chain stops at clear-color readback; it never drew geometry, bound a material/light SSBO,
> or sampled a texture. `applyPipeline` binding clean ≠ a material ball renders.
>
> **Reconciliation with the Stage-2 Backend B decision (still stands).** Stage-2 chose
> Backend B for the generic ORM **material ball** — and that choice was *never* about init
> feasibility (Stage-2 also found init = not-a-swamp). It was about a different fact this
> spike does not touch: `static_prop` is **not a material-preview shader** — sun-only
> Schlick spec on baked vertex light, no normal-map sampling, no tangent attribute,
> ORM maps `kMaterialTexAbsent` (roughness/metallic are scalars). So:
> - **Backend A** is now de-risked and is the right backend for a **real prop/mesh
>   preview** ("how MC2 actually draws this `.fit` prop") — real RenderCore + real
>   view/pipeline + real pass shader.
> - **Backend B** still delivers the **generic normal-mapped ORM material sphere**; Backend
>   A there would render baked-vertex-light props, not a material ball. This spike does not
>   change that — it clears the init risk, not the shader-is-not-a-material-shader risk.

### Slice 3 — `ASSET-VIEWER-PREVIEW-SCENE-0`
Give `mc2_asset_viewer` a real preview runtime behind `PreviewSurface`:
orbit camera + one directional light + ambient, offscreen FBO → `ImGui::Image`,
a generated UV sphere with tangents, a `MaterialPreviewPBR`. **Backend = B** for the
generic material ball — **not** because of init (Slice 2 cleared that) but because
`static_prop` is not a material-preview shader (no normal sampling, no tangent, ORM scalar-
only). Backend B = viewer-local Cook-Torrance GLSL, MUST be UI/README-labeled "Local PBR
approximation, not exact MC2 shader". Backend A (real RenderCore + real pass shader) is the
right choice for the *real prop/mesh* preview path — Slice 4/5, not the material ball. New
`MaterialTextureLoader` owns per-slot colorspace.
*Exit:* sphere renders lit; tangent validation (flat-blue == no-normal; known normal
perturbs; UV seam stable); smoke asserts center non-black + clean GL.

### Slice 4 — `MODEL-PREVIEW-STATICPROP-0`
Load + preview one static-prop/tree asset through the preview runtime
(`ModelPreviewRenderCore`). Requires extracting material-table build/bind out of the
batcher (B6) or a minimal standalone equivalent.
*Exit:* a real asset renders in the viewer with correct albedo (+normal/ORM if Backend A
and slots populated), no Mission.

---

## Risks

1. **B1 (RenderWorld → `gos_postprocess.h`) is a true dependency inversion.** If Slice 4
   needs RenderWorld's scene API, this surfaces. Mitigation: Slices 1–3 avoid RenderWorld
   (PreviewScene can drive RenderCore + a tiny local backend directly); defer B1 until a
   tool genuinely needs RenderWorld scene management.
2. **Backend A may not come up without a Mission.** ~~The lit-draw shader/light/SSBO setup
   is unproven standalone.~~ **RESOLVED for the view+pipeline path** by the Slice-2 spike:
   it links + runs with zero Mission/game dependency (Backend A viable). Residual unknown:
   a full *lit* static-prop draw (program + light SSBO + material table) is still unproven —
   that surfaces in Slice 4, not Slice 2. Backend B remains the fallback there.
3. **`static_prop` shader is not a preview shader.** No tangent attribute, no normal/ORM
   sampling, batcher-SSBO-bound, MRT + legacy light gather. Do not force-fit it; preview
   and runtime pass shaders stay distinct for now (converge later, not now).
4. **`mcTextureManager` global threading (B2) is broad.** Touches many `mclib/` sites.
   Mitigation: host-inject the existing global rather than removing it; the class is
   already construct-anywhere (5 ctor sites).
5. **Colorspace inconsistency (B3/sRGB).** Legacy linear / static-prop forced-sRGB /
   terrain format-driven. A standalone API must make colorspace explicit per-slot
   (`KtxImage::isSrgb` is the only honest source signal) and not inherit any current path.
6. **Scope creep into Ring 2/3.** Every "while we're here" toward Mission detachment is
   out of scope. Hold the line at render/asset preview.

## Stop conditions

Stop and re-plan (do not push through) if any of:

- A slice requires editing `Mission`, `GameObjectManager`, or gameplay sim. → out of scope.
- A slice requires changing renderer **behavior** (not just plumbing). → out of scope.
- B1 (RenderWorld backend inversion) blocks a slice before Slice 4. → defer the slice,
  don't rush an abstraction.
- The include-firewall (`check-include-firewall.sh`) goes red. → fix the layering, not
  the script.
- Backend A spike (Slice 2) fails to bring up a lit draw without Mission **and** Backend B
  is also blocked. → stop, reassess; the ring isn't ready.
- Any asset-format decision becomes load-bearing. → out of scope; route to a separate spec.

---

## Final recommendation

**Yes — standalone RenderCore/asset-preview is worth doing now.** It is easier now than
later because Track R/V already built the hard parts (firewalled GL-free `RenderCore` +
`RenderWorld`, `EngineView`/`ViewUniforms` camera-as-data, resource registries,
`KtxLoader`, `MaterialGpu`, the asset-viewer `PreviewSurface` seam, the reusable shader
include-splicer). The remaining work is a thin host-services contract plus extracting two
welded-in pieces — **plumbing, not a rewrite** — and every new tool built without this
seam manufactures more one-off coupling.

Scope it as three concentric rings:

- **Ring 1 — Asset/preview standalone: do now.** Viewer, material/mesh/VFX preview,
  validation/cook tools. Slices 1–4 above.
- **Ring 2 — Renderer standalone: do after the viewer proves the seams.** RenderCore init
  independent of Mission, preview scenes, capture/debug tooling. Resolve B1 here.
- **Ring 3 — Game standalone: defer.** Mission/GameObjectManager/sim/AI/campaign/editor
  level placement. Not now.

Full engine agnosticism is a later goal; do **not** attempt it in one pass.
