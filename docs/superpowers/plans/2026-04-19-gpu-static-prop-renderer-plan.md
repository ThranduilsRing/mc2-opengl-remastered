# GPU Static Prop Renderer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace MC2's CPU-side static-prop submission pipeline (`TG_Shape::Render` → `mcTextureManager` → MLR) with a GPU-residency batcher that submits every static prop every frame and lets the GPU clip naturally. Eliminates the MLR `uint16` vertex-index ceiling and the broken `recalcBounds` angular cull responsible for objects disappearing at wolfman zoom.

**Architecture:** Per-map immutable shared VBO/IBO containing every static-prop type and damage variant; per-frame persistent-mapped ring buffers holding instance SSBO + CPU-baked vertex-color stream; new `static_prop` and `static_prop_shadow` shader programs. Submission is scatter — `*Appearance::render()` appends the instance into its type's bucket. Draw is gather — flush repacks each type into a contiguous SSBO slice, binds that slice via `glBindBufferRange`, and issues one `glDrawElementsInstancedBaseVertex(..., instanceCount, ...)` per packet. `gl_InstanceID` in the shader indexes the bound range directly (no dependence on `gl_BaseInstance`). Old CPU path stays behind a killswitch (RAlt+0) for side-by-side validation.

**Tech Stack:** OpenGL 4.6 core (persistent coherent mapped buffers, `ARB_buffer_storage`), GLSL 420 via `makeProgram()`, CMake/MSBuild (MSVC 2022), Tracy profiler. Target GPU: AMD Radeon RX 7900 XTX (driver invariants enforced per `docs/amd-driver-rules.md`).

**Reference docs (read these before starting):**
- Design spec: [docs/superpowers/specs/2026-04-19-gpu-static-prop-renderer-design.md](../specs/2026-04-19-gpu-static-prop-renderer-design.md)
- Render contract: [docs/render-contract.md](../../render-contract.md)
- AMD driver rules: [docs/amd-driver-rules.md](../../amd-driver-rules.md)
- Uniform discipline: [memory/deferred_vs_direct_uniforms.md] in user-memory
- Existing shadow pipeline: `mclib/txmmgr.cpp` (renderLists, Shadow.DynPass), `GameOS/gameos/gameos_graphics.cpp` (`gos_DrawShadowObjectBatch`)
- Existing prop render sites (to wire into): `mclib/bdactor.cpp:1506` (`BldgAppearance::render`), `mclib/bdactor.cpp:3910` (`TreeAppearance::render`), `mclib/genactor.cpp`, `mclib/gvactor.cpp`, `mclib/appear.cpp`

**Verification model.** This codebase has no unit-test infrastructure. "Test" in each task means one or more of:
- **Build gate** — compile succeeds under `RelWithDebInfo` via `/mc2-build`
- **Static-assert gate** — layout asserts fire on intentional mismatch, pass after correct fix
- **Color-address debug gate** — renders expected pattern in `MC2_DEBUG_STATIC_PROP_ADDR=1/2` modes
- **Visual diff gate** — toggle RAlt+0 in-game, compare old vs new path, document pass criterion per task

**Build/deploy skills.** Always build with `/mc2-build` and deploy with `/mc2-deploy` (never `cp -r`). Verify deployment with `/mc2-check`.

---

## Phase 0 — Preflight (Task 1)

### Task 1: Create plan tracking scaffold

**Files:**
- Create: `docs/superpowers/plans/progress/2026-04-19-gpu-static-prop-renderer-progress.md`

- [ ] **Step 1: Create progress tracker file**

Write:

```markdown
# GPU Static Prop Renderer — Implementation Progress

Plan: docs/superpowers/plans/2026-04-19-gpu-static-prop-renderer-plan.md

## Phase status
- [ ] Phase 1 Scaffolding (Tasks 2–5)
- [ ] Phase 2 Geometry table (Tasks 6–7)
- [ ] Phase 3 Submit/flush + main shader (Tasks 8–10)
- [ ] Phase 4 Killswitch + buildings (Task 11)
- [ ] Phase 5 Color-address validation (Task 12)
- [ ] Phase 6 Shadow path (Tasks 13–14)
- [ ] Phase 7 M1 step 1 sign-off (Task 15)
- [ ] Phase 8 Expand to other types (Tasks 16–17)

## Notes
- Killswitch hotkey: RAlt+0 (confirmed free in gameosmain.cpp)
- Target GPU: AMD RX 7900 XTX — all AMD invariants from spec apply
```

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/plans/progress/2026-04-19-gpu-static-prop-renderer-progress.md
git commit -m "docs: add GPU static prop renderer progress tracker"
```

---

## Phase 1 — Scaffolding (Tasks 2–5)

### Task 2: Batcher header with struct layout contracts

**Files:**
- Create: `GameOS/gameos/gos_static_prop_batcher.h`

- [ ] **Step 1: Write the header**

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>
#include "Stuff/Stuff.hpp"
#include "tgl.h"

// Per-instance shader-visible struct.
// Layout mirror of the GLSL std430 struct in shaders/static_prop.vert.
// CHANGING THIS STRUCT REQUIRES CHANGING THE SHADER IN LOCKSTEP.
struct alignas(16) GpuStaticPropInstance {
    float    modelMatrix[16];   // shape-to-world, row-major (uploaded GL_FALSE)
    uint32_t typeID;
    uint32_t firstColorOffset;  // into per-frame color SSBO
    uint32_t flags;             // bit 0: lightsOut, bit 1: isWindow, bit 2: isSpotlight
    uint32_t _pad0;
    float    aRGBHighlight[4];
    float    fogRGB[4];
};

// Layout: 64 (mat4) + 16 (4 x uint32) + 16 (vec4) + 16 (vec4) = 112 bytes.
static_assert(sizeof(GpuStaticPropInstance) == 112,
              "GpuStaticPropInstance size must match std430 GLSL struct");
static_assert(offsetof(GpuStaticPropInstance, modelMatrix)      ==  0, "modelMatrix offset");
static_assert(offsetof(GpuStaticPropInstance, typeID)           == 64, "typeID offset");
static_assert(offsetof(GpuStaticPropInstance, firstColorOffset) == 68, "firstColorOffset offset");
static_assert(offsetof(GpuStaticPropInstance, flags)            == 72, "flags offset");
static_assert(offsetof(GpuStaticPropInstance, _pad0)            == 76, "_pad0 offset");
static_assert(offsetof(GpuStaticPropInstance, aRGBHighlight)    == 80, "aRGBHighlight offset");
static_assert(offsetof(GpuStaticPropInstance, fogRGB)           == 96, "fogRGB offset");

// Packet descriptor (CPU-side only — not uploaded as an SSBO).
struct GpuStaticPropPacket {
    uint32_t firstIndex;     // into shared IBO
    uint32_t indexCount;
    int32_t  baseVertex;     // into shared VBO
    uint32_t textureHandle;  // mcTextureManager GL handle
    uint32_t materialFlags;  // bit 0: ALPHA_TEST_BIT
    uint32_t owningTypeID;
};

constexpr uint32_t STATIC_PROP_FLAG_ALPHA_TEST = 1u << 0;

// Per-type descriptor: range of packets + vertex count (for color block sizing).
struct GpuStaticPropType {
    uint32_t firstPacket;
    uint32_t packetCount;
    uint32_t vertexCount;    // number of vertices in the owning TG_TypeShape
    const TG_TypeShape* source;
};

class GpuStaticPropBatcher {
public:
    static GpuStaticPropBatcher& instance();

    // Called from gameosmain at map load / unload.
    void onMapLoad();
    void onMapUnload();

    // Register one TG_TypeShape (idempotent). Builds packet table entries
    // and appends geometry to the in-progress VBO/IBO staging.
    // Called during onMapLoad for every static-prop type + its damage variants.
    void registerType(TG_TypeShape* typeShape);

    // Called at end of registration to upload the immutable VBO/IBO.
    void finalizeGeometry();

    // Per-frame submission. shape->listOfColors must be fresh (set by
    // TransformMultiShape earlier in the frame).
    // Returns true if the instance was accepted. Returns false (Layer B
    // safety net) when the type was never registered; in that case the
    // caller MUST render the shape via the old CPU path this frame.
    [[nodiscard]] bool submit(TG_Shape* shape,
                              const Stuff::Matrix4D& shapeToWorld,
                              uint32_t highlightARGB,
                              uint32_t fogARGB,
                              uint32_t flags);

    // Per-frame dispatch.
    void flush();         // main color pass
    void flushShadow();   // depth-only into dynamic shadow FBO

    // Debug: color-address validation mode. 0=off, 1=gradient, 2=hash.
    void setDebugAddrMode(int mode);
    int  getDebugAddrMode() const { return debugAddrMode_; }

private:
    GpuStaticPropBatcher() = default;

    // Declared here so the whole batcher state is visible for review;
    // implementation details live in .cpp.
    struct Impl;
    // State is file-static in .cpp to keep this header light; singleton
    // method bodies there forward to those statics.
    int debugAddrMode_ = 0;
};
```

- [ ] **Step 2: Trigger static_assert to validate the contract**

Temporarily change `offsetof(GpuStaticPropInstance, typeID) == 64` to `== 63`. Run `/mc2-build`.
Expected: build fails with `static_assert failed: "typeID offset"`.

- [ ] **Step 3: Revert the intentional break**

Change `== 63` back to `== 64`. Run `/mc2-build`.
Expected: build succeeds (header is not yet included anywhere, so the only effect is that the asserts themselves compile).

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.h
git commit -m "feat(props): batcher header with std430 layout contract"
```

---

### Task 3: Batcher .cpp skeleton (no rendering yet)

**Files:**
- Create: `GameOS/gameos/gos_static_prop_batcher.cpp`
- Modify: `GameOS/gameos/CMakeLists.txt` (add the new source file)

- [ ] **Step 1: Write the skeleton**

```cpp
#include "gos_static_prop_batcher.h"
#include "gos_profiler.h"
#include "gameos.hpp"
#include <GL/glew.h>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t RING_FRAMES = 3;
constexpr size_t   INITIAL_INSTANCES_PER_FRAME = 4096;
constexpr size_t   INITIAL_COLORS_PER_FRAME    = 1'000'000;  // uint32 ARGB entries

// Immutable per-map geometry.
GLuint s_sharedVbo = 0;
GLuint s_sharedIbo = 0;
GLuint s_sharedVao = 0;

// Per-frame persistent-mapped rings.
GLuint   s_instanceSsbo = 0;
GLuint   s_colorSsbo    = 0;
void*    s_instanceMap  = nullptr;
void*    s_colorMap     = nullptr;
GLsync   s_fence[RING_FRAMES] = {0};
uint32_t s_frameSlot = 0;

// CPU staging for the current frame.
// Instances are staged in per-type buckets (not a flat list) so that
// flush() can write each type's instances into a contiguous SSBO region.
// Binding that region via glBindBufferRange means gl_InstanceID in the
// shader is 0..N-1 within the bucket — NOT dependent on gl_BaseInstance
// and NOT requiring any extension.
struct PerTypeBucket {
    std::vector<GpuStaticPropInstance> instances;
    std::vector<uint32_t>              colors;  // concatenated per-instance color blocks
};
std::unordered_map<uint32_t, PerTypeBucket> s_bucketsByType;

// Populated at flush time: per-type contiguous byte offset into the
// ring-slot SSBO (instance + color), used to bind exactly that range.
struct TypeRangeSsbo {
    size_t instanceByteOffset;
    size_t instanceByteSize;
    size_t colorByteOffset;
    size_t colorByteSize;
    uint32_t instanceCount;
};

// Geometry table (immutable after finalizeGeometry).
std::vector<GpuStaticPropPacket>                   s_packets;
std::vector<GpuStaticPropType>                     s_types;
std::unordered_map<const TG_TypeShape*, uint32_t>  s_typeIndex;

// CPU-side staging during registration (cleared after finalizeGeometry).
std::vector<uint8_t>  s_stagingVbo;
std::vector<uint32_t> s_stagingIbo;

bool s_geometryFinalized = false;
bool s_fatalRegistrationFailure = false;

// Layer B fallback: types we failed to register (logged once, fall back to CPU path).
std::unordered_map<const TG_TypeShape*, bool> s_failedTypes;

} // namespace

GpuStaticPropBatcher& GpuStaticPropBatcher::instance() {
    static GpuStaticPropBatcher s;
    return s;
}

void GpuStaticPropBatcher::onMapLoad() {
    // Reset everything; called at every map boundary.
    s_packets.clear();
    s_types.clear();
    s_typeIndex.clear();
    s_stagingVbo.clear();
    s_stagingIbo.clear();
    s_failedTypes.clear();
    s_geometryFinalized = false;
    s_fatalRegistrationFailure = false;
}

void GpuStaticPropBatcher::onMapUnload() {
    if (s_sharedVbo) { glDeleteBuffers(1, &s_sharedVbo); s_sharedVbo = 0; }
    if (s_sharedIbo) { glDeleteBuffers(1, &s_sharedIbo); s_sharedIbo = 0; }
    if (s_sharedVao) { glDeleteVertexArrays(1, &s_sharedVao); s_sharedVao = 0; }
    // Ring buffers are kept across maps (sized to map's worst case — grow on demand).
}

void GpuStaticPropBatcher::registerType(TG_TypeShape* /*typeShape*/) {
    // Filled in Task 6.
}

void GpuStaticPropBatcher::finalizeGeometry() {
    // Filled in Task 7.
    s_geometryFinalized = true;
}

bool GpuStaticPropBatcher::submit(TG_Shape* /*shape*/,
                                  const Stuff::Matrix4D& /*shapeToWorld*/,
                                  uint32_t /*highlightARGB*/,
                                  uint32_t /*fogARGB*/,
                                  uint32_t /*flags*/) {
    // Filled in Task 8.
    return false;
}

void GpuStaticPropBatcher::flush() {
    // Filled in Task 10.
    s_bucketsByType.clear();
}

void GpuStaticPropBatcher::flushShadow() {
    // Filled in Task 13.
}

void GpuStaticPropBatcher::setDebugAddrMode(int mode) { debugAddrMode_ = mode; }
```

- [ ] **Step 2: Add the source to the CMake target**

Open `GameOS/gameos/CMakeLists.txt`. Find the `set(SRCS ...)` block (there is exactly one for the `gameos` target). Add these two lines alphabetically near the other `gos_*.cpp` entries:

```cmake
    gos_static_prop_batcher.cpp
    gos_static_prop_batcher.h
```

- [ ] **Step 3: Build**

Run `/mc2-build`.
Expected: PASS. New source compiles cleanly with no warnings. No linker errors (nothing calls into the batcher yet).

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp GameOS/gameos/CMakeLists.txt
git commit -m "feat(props): batcher .cpp skeleton (no rendering yet)"
```

---

### Task 4: Map-load/unload hooks

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp`

- [ ] **Step 1: Find the map load/unload site**

Grep `gameosmain.cpp` for the existing map transition handlers (look for where other per-map state is reset — e.g., `getGosPostProcess()` FBO resize hooks, or the Tracy `ZoneScopedN("Frame")` main loop boundary). Identify:
- the function called once per map load, and
- the function called once per map unload / shutdown.

Typical pattern in this codebase: an `Environment_Restart` / map-load entrypoint and a cleanup path on scene exit. If unclear, grep for where other renderer singletons are torn down (e.g., shadow FBO resets).

- [ ] **Step 2: Add the hooks**

At the end of the map-load path, add:

```cpp
#include "gos_static_prop_batcher.h"
// ...
GpuStaticPropBatcher::instance().onMapLoad();
// registerType() calls are added in Task 6, wired from the actor-spawn path.
```

At the map-unload / shutdown path, add:

```cpp
GpuStaticPropBatcher::instance().onMapUnload();
```

- [ ] **Step 3: Build**

Run `/mc2-build`.
Expected: PASS.

- [ ] **Step 4: Deploy and run a map load cycle**

Run `/mc2-deploy`, launch the game, enter a mission, exit to main menu, re-enter.
Expected: no crash, no error log entries from the batcher. Batcher's onMapLoad/onMapUnload are now on the correct hot path.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(props): wire batcher lifetime to map load/unload"
```

---

### Task 5: Killswitch global and RAlt+0 hotkey

**Files:**
- Modify: `code/mechcmd2.cpp` (define `g_useGpuStaticProps`)
- Modify: `GameOS/gameos/gameosmain.cpp` (hotkey handler)
- Create: `GameOS/gameos/gos_static_prop_killswitch.h` (one-line extern for callers)

- [ ] **Step 1: Declare the global**

Create `GameOS/gameos/gos_static_prop_killswitch.h`:

```cpp
#pragma once

// Global runtime toggle for the GPU static-prop renderer.
// Default false — old CPU path is active until validated.
// Toggled at runtime via RAlt+0 (see gameosmain.cpp).
extern bool g_useGpuStaticProps;
```

Define it in `code/mechcmd2.cpp` near other global render toggles (grep for `bool g_` and pick a site near existing toggles). Add:

```cpp
#include "gos_static_prop_killswitch.h"
bool g_useGpuStaticProps = false;
```

- [ ] **Step 2: Wire the hotkey**

Open `GameOS/gameos/gameosmain.cpp`. Find the `alt_debug` switch — the block starting near `case SDLK_8:` (terrain debug) and `case SDLK_9:` (SSAO). Add a new case **before the closing brace** of the switch:

```cpp
case SDLK_0:
    if (alt_debug) {
        g_useGpuStaticProps = !g_useGpuStaticProps;
        fprintf(stderr, "GPU Static Props: %s\n",
                g_useGpuStaticProps ? "ON" : "OFF");
    }
    break;
```

Add at the top of the file:
```cpp
#include "gos_static_prop_killswitch.h"
```

- [ ] **Step 3: Build**

Run `/mc2-build`.
Expected: PASS.

- [ ] **Step 4: Deploy and test the hotkey**

Run `/mc2-deploy`, launch the game, enter a mission, press RAlt+0 repeatedly.
Expected: stderr alternates `GPU Static Props: ON` / `OFF`. Nothing else changes yet (no call sites consume the flag).

- [ ] **Step 5: Commit**

```bash
git add code/mechcmd2.cpp GameOS/gameos/gameosmain.cpp GameOS/gameos/gos_static_prop_killswitch.h
git commit -m "feat(props): add g_useGpuStaticProps killswitch + RAlt+0 hotkey"
```

---

## Phase 2 — Geometry table (Tasks 6–7)

### Task 6: Type registration (packet enumeration + staging VBO/IBO)

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 1: Read context — and resolve the packet-level invariant before coding**

Read these sections before writing:
- `mclib/tgl.h:522-676` — `TG_TypeShape` fields (`numTypeVertices`, `listOfTypeVertices`, `listOfTypeTriangles`, `listOfTextures`, `numTextures`, `alphaTestOn`).
- `mclib/msl.h:232-300` — `TG_TypeMultiShape` and `listOfNodes` traversal pattern.
- Existing old-path submission in `TG_Shape::Render` (`mclib/tgl.cpp:2528`) — note whether the shader iterates over sub-node groupings within a single `TG_TypeShape` or just walks the flat `listOfTypeTriangles`.

**The design's packet-order invariant is: "packets within a type are written in the same order as `TG_TypeMultiShape::listOfNodes[0..numNodes-1]`".** Verify before coding that this maps cleanly onto the per-`TG_TypeShape` registration below:

- **Expected case (this fork):** each node in a `TG_TypeMultiShape::listOfNodes` is itself a `TG_TypeShape` leaf. The outer numNodes order is preserved by the caller registering each node in listOfNodes order (Step 3 below). Within one `TG_TypeShape`, the flat `listOfTypeTriangles` is the authored order — grouping contiguous-texture runs within it is equivalent to the old path's behavior.
- **If a single `TG_TypeShape` in this fork actually wraps multiple authored sub-nodes internally** (i.e., MC2 has "compound" TypeShapes) — packet enumeration by texture-run is **not** equivalent to numNodes order. Derive packets from the real sub-node traversal instead.

Read `TG_Shape::Render` (line 2528) carefully: look for whether it loops over sub-node groups or straight over `listOfVisibleFaces`. If the former, adjust packet enumeration in Step 2 to mirror that loop; if the latter, the texture-run grouping below is correct.

- [ ] **Step 2: Implement `registerType`**

In `gos_static_prop_batcher.cpp`, replace the stub with:

```cpp
void GpuStaticPropBatcher::registerType(TG_TypeShape* typeShape) {
    if (!typeShape) return;
    if (s_typeIndex.count(typeShape)) return;  // idempotent
    if (s_geometryFinalized) {
        // Layer B: register-after-finalize is a bug in the map-load walk.
        if (!s_failedTypes[typeShape]) {
            std::fprintf(stderr, "[GPUPROPS] late registerType for %p — "
                         "CPU-fallback for this type\n", (void*)typeShape);
            s_failedTypes[typeShape] = true;
        }
        return;
    }

    const uint32_t baseVertex   = static_cast<uint32_t>(s_stagingVbo.size() / kVertexStride);
    const uint32_t firstIndex   = static_cast<uint32_t>(s_stagingIbo.size());
    const uint32_t numVerts     = typeShape->GetNumTypeVertices();

    // Copy vertices (local pos, normal, uv, localVertexID).
    // The interleaved layout matches shaders/static_prop.vert:
    //   vec3 a_position (0..11)
    //   vec3 a_normal   (12..23)
    //   vec2 a_uv       (24..31)
    //   uint a_localVertexID (32..35)
    //   float _pad      (36..39)  (keep 40-byte stride, 4-byte alignment)
    for (uint32_t v = 0; v < numVerts; ++v) {
        const TG_TypeVertex& src = typeShape->listOfTypeVertices[v];
        uint8_t vert[kVertexStride] = {};
        std::memcpy(vert +  0, &src.position.x, 4);
        std::memcpy(vert +  4, &src.position.y, 4);
        std::memcpy(vert +  8, &src.position.z, 4);
        std::memcpy(vert + 12, &src.normal.x,   4);
        std::memcpy(vert + 16, &src.normal.y,   4);
        std::memcpy(vert + 20, &src.normal.z,   4);
        std::memcpy(vert + 24, &src.u,          4);
        std::memcpy(vert + 28, &src.v,          4);
        std::memcpy(vert + 32, &v,              4);  // a_localVertexID
        // bytes 36..39 zero-filled
        s_stagingVbo.insert(s_stagingVbo.end(), vert, vert + kVertexStride);
    }

    // Indices + packets, one packet per texture/material group.
    // TG_TypeShape exposes a flat triangle list (listOfTypeTriangles) with
    // a per-triangle texture index. Group triangles with the same
    // texture index into contiguous packets, preserving author order
    // (the old path iterates in author order — we match that so z-ties
    // and alpha-test edges fall identically).
    const uint32_t numTris = typeShape->numTypeTriangles;
    uint32_t runStart = 0;
    while (runStart < numTris) {
        const uint32_t runTextureIdx =
            typeShape->listOfTypeTriangles[runStart].textureIndex;
        uint32_t runEnd = runStart;
        while (runEnd < numTris &&
               typeShape->listOfTypeTriangles[runEnd].textureIndex == runTextureIdx) {
            ++runEnd;
        }

        const uint32_t packetFirstIndex = static_cast<uint32_t>(s_stagingIbo.size());
        for (uint32_t t = runStart; t < runEnd; ++t) {
            const TG_TypeTriangle& tri = typeShape->listOfTypeTriangles[t];
            s_stagingIbo.push_back(tri.Vertices[0]);
            s_stagingIbo.push_back(tri.Vertices[1]);
            s_stagingIbo.push_back(tri.Vertices[2]);
        }

        GpuStaticPropPacket pkt{};
        pkt.firstIndex    = packetFirstIndex;
        pkt.indexCount    = (runEnd - runStart) * 3;
        pkt.baseVertex    = static_cast<int32_t>(baseVertex);
        pkt.textureHandle = typeShape->listOfTextures
                              ? typeShape->listOfTextures[runTextureIdx].gosTextureHandle
                              : 0;
        pkt.materialFlags = typeShape->alphaTestOn ? STATIC_PROP_FLAG_ALPHA_TEST : 0;
        pkt.owningTypeID  = static_cast<uint32_t>(s_types.size());
        s_packets.push_back(pkt);

        runStart = runEnd;
    }

    // Compute packet range for this type by counting back from s_packets.size()
    // while owningTypeID still matches the index we're about to assign.
    const uint32_t newTypeID = static_cast<uint32_t>(s_types.size());
    uint32_t packetCount = 0;
    while (packetCount < s_packets.size() &&
           s_packets[s_packets.size() - 1 - packetCount].owningTypeID == newTypeID) {
        ++packetCount;
    }

    GpuStaticPropType type{};
    type.firstPacket = static_cast<uint32_t>(s_packets.size()) - packetCount;
    type.packetCount = packetCount;
    type.vertexCount = numVerts;
    type.source      = typeShape;

    s_typeIndex[typeShape] = newTypeID;
    s_types.push_back(type);
}
```

Add at top of file near the other `namespace { ... }` constants:

```cpp
constexpr size_t kVertexStride = 40;  // 3+3+2+1+1 floats/uints, see layout above
```

Note on field names: `TG_TypeVertex` and `TG_TypeTriangle` field names above (`position`, `normal`, `u`, `v`, `textureIndex`, `Vertices[3]`) must match the actual class. If names differ, grep `TG_TypeVertex` and `TG_TypeTriangle` in `mclib/tgl.h` and adjust to real field names before building — **do not guess**. If the triangle struct does not carry a per-triangle texture index (some MC2 forks store that on the vertex or on a separate list), use the existing iteration pattern in `TG_Shape::Render` as ground truth.

- [ ] **Step 3: Register every spawned type at map load**

Grep for where static-prop actors are loaded during mission start. The canonical callsite is where each `BldgAppearance`, `TreeAppearance`, `GenericAppearance`, `GVAppearance` constructor resolves its `TG_TypeMultiShape*` from the game's type library. Typical site: the actor's `init(...)` or the `*AppearanceType::loadMesh(...)` path.

At each of those sites, iterate the just-loaded `TG_TypeMultiShape`'s `listOfNodes[0..numNodes-1]`. For each node that is a `TG_TypeShape*` (check `GetNodeType() == SHAPE_NODE`), call:

```cpp
GpuStaticPropBatcher::instance().registerType(static_cast<TG_TypeShape*>(node));
```

Also register damage/destroyed/wreck variants. These are declared on the actor type data — grep `destroyedShape`, `damagedShape`, `wreckShape` in `mclib/bdactor.cpp`/`gvactor.cpp` for the exact field names. Register every variant the actor can swap into.

- [ ] **Step 4: Build**

Run `/mc2-build`.
Expected: PASS. Any compile errors here are near-certainly field-name mismatches in the TG_TypeVertex / TG_TypeTriangle access — grep the real names and fix.

- [ ] **Step 5: Deploy and log a map's registration size**

Temporarily add at the end of `registerType`:

```cpp
std::fprintf(stderr, "[GPUPROPS] reg %p: %u verts, %u packets, types=%zu\n",
             (void*)typeShape, numVerts, type.packetCount, s_types.size());
```

Run `/mc2-deploy`, launch a typical mission (pick one with many buildings).
Expected: tens of `[GPUPROPS] reg` lines at mission start; none during gameplay. If any appear during gameplay, the map-load walk missed a variant — note which type and fix the registration walk in Step 3. **Remove the printf before committing.**

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp <actor files modified in Step 3>
git commit -m "feat(props): type registration with per-packet granularity"
```

---

### Task 7: Finalize geometry — upload immutable VBO/IBO/VAO

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 1: Implement `finalizeGeometry`**

Replace the stub:

```cpp
void GpuStaticPropBatcher::finalizeGeometry() {
    if (s_geometryFinalized) return;

    glGenVertexArrays(1, &s_sharedVao);
    glBindVertexArray(s_sharedVao);

    glGenBuffers(1, &s_sharedVbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_sharedVbo);
    glBufferStorage(GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingVbo.size()),
                    s_stagingVbo.data(),
                    0);  // flags=0 → fully immutable, GPU-only (AMD-safe)

    glGenBuffers(1, &s_sharedIbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_sharedIbo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(s_stagingIbo.size() * sizeof(uint32_t)),
                    s_stagingIbo.data(),
                    0);

    // Vertex attribute layout — position MUST be location 0 (AMD invariant 1).
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT,    GL_FALSE, kVertexStride, (void*) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT,    GL_FALSE, kVertexStride, (void*)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT,    GL_FALSE, kVertexStride, (void*)24);
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT,      kVertexStride, (void*)32);

    glBindVertexArray(0);

    // Free CPU staging.
    s_stagingVbo.clear(); s_stagingVbo.shrink_to_fit();
    s_stagingIbo.clear(); s_stagingIbo.shrink_to_fit();

    std::fprintf(stderr, "[GPUPROPS] finalize: %zu types, %zu packets\n",
                 s_types.size(), s_packets.size());

    s_geometryFinalized = true;
}
```

- [ ] **Step 2: Call `finalizeGeometry` after all registration is done**

At the end of the map-load path in `gameosmain.cpp` (after every `registerType` call has fired — i.e., after the mission's actor list has been fully spawned):

```cpp
GpuStaticPropBatcher::instance().finalizeGeometry();
```

- [ ] **Step 3: Build and deploy**

Run `/mc2-build` then `/mc2-deploy`.
Expected: PASS.

- [ ] **Step 4: Verify in-game**

Launch a mission. Check stderr.
Expected: exactly one `[GPUPROPS] finalize: N types, M packets` line per mission start, where N matches the rough count of static-prop types in the map (tens to low hundreds) and M ≥ N.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp GameOS/gameos/gameosmain.cpp
git commit -m "feat(props): finalize immutable shared VBO/IBO/VAO at map load"
```

---

## Phase 3 — Submit/flush + main shader (Tasks 8–10)

### Task 8: `submit()` + per-frame ring buffers

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 1: Add ring-buffer allocation (called once lazily on first submit)**

Add a private static helper at the top of the anon namespace:

```cpp
size_t s_instanceCapacity = 0;
size_t s_colorCapacity    = 0;

void ensureRingCapacity(size_t neededInstances, size_t neededColorEntries) {
    const bool needGrow =
        s_instanceSsbo == 0 ||
        neededInstances > s_instanceCapacity ||
        neededColorEntries > s_colorCapacity;
    if (!needGrow) return;

    // Wait for all in-flight frames before resizing.
    for (uint32_t i = 0; i < RING_FRAMES; ++i) {
        if (s_fence[i]) {
            glClientWaitSync(s_fence[i], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
            glDeleteSync(s_fence[i]);
            s_fence[i] = 0;
        }
    }
    if (s_instanceSsbo) { glDeleteBuffers(1, &s_instanceSsbo); s_instanceSsbo = 0; }
    if (s_colorSsbo)    { glDeleteBuffers(1, &s_colorSsbo);    s_colorSsbo = 0; }

    s_instanceCapacity = std::max(neededInstances,    s_instanceCapacity ? s_instanceCapacity * 2 : INITIAL_INSTANCES_PER_FRAME);
    s_colorCapacity    = std::max(neededColorEntries, s_colorCapacity    ? s_colorCapacity    * 2 : INITIAL_COLORS_PER_FRAME);

    const GLbitfield storageFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield mapFlags     = storageFlags;

    glGenBuffers(1, &s_instanceSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_instanceSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance)),
                    nullptr, storageFlags);
    s_instanceMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                    RING_FRAMES * s_instanceCapacity * sizeof(GpuStaticPropInstance),
                    mapFlags);

    glGenBuffers(1, &s_colorSsbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_colorSsbo);
    glBufferStorage(GL_SHADER_STORAGE_BUFFER,
                    static_cast<GLsizeiptr>(RING_FRAMES * s_colorCapacity * sizeof(uint32_t)),
                    nullptr, storageFlags);
    s_colorMap = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                    RING_FRAMES * s_colorCapacity * sizeof(uint32_t),
                    mapFlags);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!s_instanceMap || !s_colorMap) {
        std::fprintf(stderr, "[GPUPROPS] persistent map failed; disabling GPU path\n");
        s_fatalRegistrationFailure = true;
    }
}
```

- [ ] **Step 2: Implement `submit`**

Replace the stub:

```cpp
bool GpuStaticPropBatcher::submit(TG_Shape* shape,
                                  const Stuff::Matrix4D& shapeToWorld,
                                  uint32_t highlightARGB,
                                  uint32_t fogARGB,
                                  uint32_t flags) {
    if (!shape || s_fatalRegistrationFailure) return false;
    TG_TypeShape* typeShape = shape->GetTypeShape();   // grep for the real accessor name in tgl.h
    auto it = s_typeIndex.find(typeShape);
    if (it == s_typeIndex.end()) {
        if (!s_failedTypes[typeShape]) {
            std::fprintf(stderr, "[GPUPROPS] unregistered type %p for shape %p — "
                         "caller must CPU-fallback\n", (void*)typeShape, (void*)shape);
            s_failedTypes[typeShape] = true;
        }
        return false;  // Layer B: caller calls shape->Render() on false.
    }

    const uint32_t typeID = it->second;
    const GpuStaticPropType& type = s_types[typeID];
    PerTypeBucket& bucket = s_bucketsByType[typeID];

    // firstColorOffset is the byte-offset-free index into the bucket's
    // color array: instance K reads colors starting at K * type.vertexCount.
    // (The shader binds the bucket's color range; firstColorOffset
    // becomes an index within that range.)
    const uint32_t firstColorOffset =
        static_cast<uint32_t>(bucket.colors.size());

    GpuStaticPropInstance inst{};
    // Matrix4D is row-major 4x4. Copy in row-major (shader uploads GL_FALSE).
    std::memcpy(inst.modelMatrix, &shapeToWorld, 16 * sizeof(float));
    inst.typeID           = typeID;
    inst.firstColorOffset = firstColorOffset;
    inst.flags            = flags;
    inst.aRGBHighlight[0] = ((highlightARGB >> 16) & 0xFF) / 255.0f;
    inst.aRGBHighlight[1] = ((highlightARGB >>  8) & 0xFF) / 255.0f;
    inst.aRGBHighlight[2] = ((highlightARGB >>  0) & 0xFF) / 255.0f;
    inst.aRGBHighlight[3] = ((highlightARGB >> 24) & 0xFF) / 255.0f;
    inst.fogRGB[0] = ((fogARGB >> 16) & 0xFF) / 255.0f;
    inst.fogRGB[1] = ((fogARGB >>  8) & 0xFF) / 255.0f;
    inst.fogRGB[2] = ((fogARGB >>  0) & 0xFF) / 255.0f;
    inst.fogRGB[3] = ((fogARGB >> 24) & 0xFF) / 255.0f;
    bucket.instances.push_back(inst);

    // Append this instance's vertex-color block.
    const uint32_t numColors = type.vertexCount;
    bucket.colors.insert(bucket.colors.end(),
        reinterpret_cast<const uint32_t*>(shape->listOfColors),
        reinterpret_cast<const uint32_t*>(shape->listOfColors) + numColors);

    return true;
}
```

Field name for the type accessor (`shape->GetTypeShape()`) and the color stream (`shape->listOfColors`) — both may need fix-up to match the real API in `tgl.h`; grep if the build fails.

- [ ] **Step 3: Build**

Run `/mc2-build`.
Expected: PASS (after any field-name fix-ups).

- [ ] **Step 4: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(props): submit() + persistent-mapped ring buffers"
```

---

### Task 9: Main shader pair (`static_prop.vert` / `.frag`)

**Files:**
- Create: `shaders/static_prop.vert`
- Create: `shaders/static_prop.frag`

- [ ] **Step 1: Write the vertex shader**

No `#version` line — that's supplied by `makeProgram()`.

```glsl
// shaders/static_prop.vert
layout(location = 0) in vec3  a_position;
layout(location = 1) in vec3  a_normal;
layout(location = 2) in vec2  a_uv;
layout(location = 3) in uint  a_localVertexID;

struct Instance {
    mat4  modelMatrix;
    uint  typeID;
    uint  firstColorOffset;
    uint  flags;
    uint  _pad0;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};

layout(std430, binding = 0) readonly buffer Instances { Instance i[]; } instances_;
layout(std430, binding = 1) readonly buffer Colors    { uint     c[]; } colors_;

uniform mat4 u_worldToClip;

out vec3  v_normal;
out vec2  v_uv;
flat out uint v_flags;
out vec4  v_highlight;
out vec4  v_fog;
out vec4  v_argb;
flat out uint v_localVertexID;  // for debug address mode

void main() {
    Instance inst = instances_.i[gl_InstanceID];

    vec4 world = inst.modelMatrix * vec4(a_position, 1.0);
    gl_Position = u_worldToClip * world;

    // Baked per-vertex lighting: colorBuffer[firstColorOffset + a_localVertexID].
    uint argbPacked = colors_.c[inst.firstColorOffset + a_localVertexID];
    vec4 argb;
    argb.a = float((argbPacked >> 24) & 0xFFu) / 255.0;
    argb.r = float((argbPacked >> 16) & 0xFFu) / 255.0;
    argb.g = float((argbPacked >>  8) & 0xFFu) / 255.0;
    argb.b = float((argbPacked >>  0) & 0xFFu) / 255.0;
    v_argb = argb;

    v_normal     = mat3(inst.modelMatrix) * a_normal;
    v_uv         = a_uv;
    v_flags      = inst.flags;
    v_highlight  = inst.aRGBHighlight;
    v_fog        = inst.fogRGB;
    v_localVertexID = a_localVertexID;
}
```

- [ ] **Step 2: Write the fragment shader**

```glsl
// shaders/static_prop.frag
in vec3  v_normal;
in vec2  v_uv;
flat in uint v_flags;
in vec4  v_highlight;
in vec4  v_fog;
in vec4  v_argb;
flat in uint v_localVertexID;

uniform sampler2D u_tex;
uniform uint  u_materialFlags;   // bit 0: ALPHA_TEST
uniform float u_fogValue;        // 1.0 = clear, 0.0 = fully fogged
uniform int   u_debugAddrMode;   // 0 normal, 1 gradient, 2 hash
uniform uint  u_maxLocalVertexID;// uploaded per type: the current type's
                                 // vertexCount, so debug gradient is
                                 // normalized per shape (not against a
                                 // global max across all types)
uniform uint  u_packetID;        // uploaded per-draw for mode 2

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 GBuffer1;  // scene normal buffer, alpha=0 for non-terrain

const uint ALPHA_TEST_BIT = 1u;

uint hash_u(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void main() {
    vec4 tex_color = texture(u_tex, v_uv);

    if ((u_materialFlags & ALPHA_TEST_BIT) != 0u && tex_color.a < 0.5) {
        discard;
    }

    // Debug address modes — validate the gl_VertexID-free indexing math.
    if (u_debugAddrMode == 1) {
        float t = float(v_localVertexID) / max(float(u_maxLocalVertexID), 1.0);
        FragColor = vec4(t, t, t, 1.0);
        GBuffer1  = vec4(0.0, 0.0, 1.0, 0.0);
        return;
    }
    if (u_debugAddrMode == 2) {
        uint h = hash_u(u_packetID * 2654435761u + v_localVertexID);
        FragColor = vec4(
            float((h >>  0) & 0xFFu) / 255.0,
            float((h >>  8) & 0xFFu) / 255.0,
            float((h >> 16) & 0xFFu) / 255.0,
            1.0);
        GBuffer1  = vec4(0.0, 0.0, 1.0, 0.0);
        return;
    }

    vec4 c = tex_color * v_argb;
    c.rgb += v_highlight.rgb * v_highlight.a;

    // Fog: matches the non-overlay path in gos_tex_vertex.frag.
    c.rgb = mix(v_fog.rgb, c.rgb, u_fogValue);

    FragColor = c;
    GBuffer1  = vec4(normalize(v_normal) * 0.5 + 0.5, 0.0);
}
```

- [ ] **Step 3: Load the program**

In `GameOS/gameos/gos_static_prop_batcher.cpp`, add at top of namespace:

```cpp
GLuint s_staticPropProgram = 0;
```

Add a helper in the file's anon namespace:

```cpp
void loadProgramsIfNeeded() {
    if (s_staticPropProgram) return;
    // makeProgram() is the project's shader-loader (see gameos_graphics.cpp
    // for usage examples — e.g., terrain program loading). Pass the
    // "#version 420\n" prefix explicitly.
    extern GLuint makeProgram(const char* versionLine,
                              const char* vertPath,
                              const char* fragPath);
    s_staticPropProgram = makeProgram("#version 420\n",
                                      "shaders/static_prop.vert",
                                      "shaders/static_prop.frag");
}
```

Check the actual `makeProgram` signature in `GameOS/gameos/utils/shader_builder.cpp` — adjust the call if the signature differs (the project already loads multiple shaders this way; follow the same pattern).

- [ ] **Step 4: Build and deploy shaders**

Run `/mc2-build` then `/mc2-deploy`.
Expected: PASS. Shader files deployed. No console errors on program load (visible in stderr at first use — which won't happen until Task 10's flush is wired).

- [ ] **Step 5: Commit**

```bash
git add shaders/static_prop.vert shaders/static_prop.frag GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(props): static_prop main shader pair + program loader"
```

---

### Task 10: `flush()` — per-packet instanced draw

**Files:**
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`
- Modify: `GameOS/gameos/gameos_graphics.cpp` (call `flush()` in the right render-order slot)

- [ ] **Step 1: Implement `flush`**

Replace the stub:

```cpp
// Per-frame upload state shared between flushShadow() (runs first) and
// flush() (runs later in the frame). flushShadow() owns the upload; flush()
// skips the upload if s_lastUploadedSlot == s_frameSlot.
namespace {
std::unordered_map<uint32_t, TypeRangeSsbo> s_typeRanges;
uint32_t s_lastUploadedSlot = 0xFFFFFFFFu;

bool uploadAllBucketsIfNeeded() {
    if (s_lastUploadedSlot == s_frameSlot) return true;

    // Compute total sizes (order-independent).
    size_t totalInstances = 0;
    size_t totalColors    = 0;
    for (auto& [typeID, b] : s_bucketsByType) {
        totalInstances += b.instances.size();
        totalColors    += b.colors.size();
    }
    if (totalInstances == 0) return false;

    loadProgramsIfNeeded();
    ensureRingCapacity(totalInstances, totalColors);

    s_frameSlot = (s_frameSlot + 1) % RING_FRAMES;
    if (s_fence[s_frameSlot]) {
        glClientWaitSync(s_fence[s_frameSlot], GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
        glDeleteSync(s_fence[s_frameSlot]);
        s_fence[s_frameSlot] = 0;
    }

    const size_t slotInstByteBase = s_frameSlot * s_instanceCapacity * sizeof(GpuStaticPropInstance);
    const size_t slotColByteBase  = s_frameSlot * s_colorCapacity    * sizeof(uint32_t);
    auto* instMapBase = static_cast<uint8_t*>(s_instanceMap) + slotInstByteBase;
    auto* colMapBase  = static_cast<uint8_t*>(s_colorMap)    + slotColByteBase;

    // Layout buckets in ascending typeID order (NOT unordered_map iteration
    // order) so SSBO packing is deterministic across runs. This makes Tracy
    // captures, RenderDoc diffs, and shader-debug repro work much cleaner.
    std::vector<uint32_t> sortedTypeIDs;
    sortedTypeIDs.reserve(s_bucketsByType.size());
    for (auto& kv : s_bucketsByType) sortedTypeIDs.push_back(kv.first);
    std::sort(sortedTypeIDs.begin(), sortedTypeIDs.end());

    s_typeRanges.clear();
    size_t instCursor = 0;  // byte cursor within the slot
    size_t colCursor  = 0;
    for (uint32_t typeID : sortedTypeIDs) {
        PerTypeBucket& b = s_bucketsByType[typeID];
        TypeRangeSsbo r{};
        r.instanceByteOffset = slotInstByteBase + instCursor;
        r.instanceByteSize   = b.instances.size() * sizeof(GpuStaticPropInstance);
        r.colorByteOffset    = slotColByteBase  + colCursor;
        r.colorByteSize      = b.colors.size() * sizeof(uint32_t);
        r.instanceCount      = static_cast<uint32_t>(b.instances.size());

        std::memcpy(instMapBase + instCursor, b.instances.data(), r.instanceByteSize);
        std::memcpy(colMapBase  + colCursor,  b.colors.data(),    r.colorByteSize);

        instCursor += r.instanceByteSize;
        colCursor  += r.colorByteSize;
        s_typeRanges[typeID] = r;
    }

    s_lastUploadedSlot = s_frameSlot;
    return true;
}
} // namespace

void GpuStaticPropBatcher::flush() {
    ZoneScopedN("GpuStaticProps.Flush");
    if (!s_geometryFinalized || s_fatalRegistrationFailure) {
        s_bucketsByType.clear();
        return;
    }
    if (!uploadAllBucketsIfNeeded()) {
        s_bucketsByType.clear();
        return;
    }

    glUseProgram(s_staticPropProgram);
    glBindVertexArray(s_sharedVao);

    // Direct uniforms (AMD invariant 6: direct, GL_FALSE transpose).
    // These getters return the project's current-frame matrix/fog — see
    // the terrain draw path in gameos_graphics.cpp for the existing source.
    extern const float* getWorldToClipMatrix();
    extern float getCurrentFogValue();
    glUniformMatrix4fv(glGetUniformLocation(s_staticPropProgram, "u_worldToClip"),
                       1, GL_FALSE, getWorldToClipMatrix());
    glUniform1i (glGetUniformLocation(s_staticPropProgram, "u_tex"),           0);
    glUniform1i (glGetUniformLocation(s_staticPropProgram, "u_debugAddrMode"), debugAddrMode_);
    glUniform1f (glGetUniformLocation(s_staticPropProgram, "u_fogValue"),      getCurrentFogValue());

    // Per-type drawing. For each type we:
    //   - bind the per-type instance SSBO range (so gl_InstanceID is 0..N-1)
    //   - bind the per-type color SSBO range
    //   - upload the type's vertexCount for debug mode normalization
    //   - issue one instanced draw per packet
    for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
        auto rit = s_typeRanges.find(typeID);
        if (rit == s_typeRanges.end()) continue;
        const TypeRangeSsbo& r = rit->second;
        const GpuStaticPropType& type = s_types[typeID];

        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo,
                          static_cast<GLintptr>(r.instanceByteOffset),
                          static_cast<GLsizeiptr>(r.instanceByteSize));
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, s_colorSsbo,
                          static_cast<GLintptr>(r.colorByteOffset),
                          static_cast<GLsizeiptr>(r.colorByteSize));
        glUniform1ui(glGetUniformLocation(s_staticPropProgram, "u_maxLocalVertexID"),
                     type.vertexCount);

        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const GpuStaticPropPacket& pkt = s_packets[type.firstPacket + p];
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, pkt.textureHandle);
            glUniform1ui(glGetUniformLocation(s_staticPropProgram, "u_materialFlags"),
                         pkt.materialFlags);
            glUniform1ui(glGetUniformLocation(s_staticPropProgram, "u_packetID"),
                         type.firstPacket + p);

            glDrawElementsInstancedBaseVertex(
                GL_TRIANGLES,
                pkt.indexCount,
                GL_UNSIGNED_INT,
                reinterpret_cast<void*>(pkt.firstIndex * sizeof(uint32_t)),
                r.instanceCount,
                pkt.baseVertex);
        }
    }

    s_fence[s_frameSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    glBindVertexArray(0);

    s_bucketsByType.clear();
    s_lastUploadedSlot = 0xFFFFFFFFu;  // reset for next frame
}
```

**Note on draw strategy.** One `glDrawElementsInstancedBaseVertex` per packet, with `instanceCount = type.instanceCount`. The per-type contiguous SSBO binding (via `glBindBufferRange`) is why `gl_InstanceID` in the shader maps directly to the correct instance row without any dependence on `gl_BaseInstance`. Expected draw count: ~packets-per-type × types-active-this-frame ≈ a few hundred draws, not thousands.

- [ ] **Step 2: Call `flush()` at the right slot in the render order**

Open `GameOS/gameos/gameos_graphics.cpp` and find where existing object-rendering batches are flushed (grep for `renderLists()` or `Render.3DObjects`). After that point (so props render after mechs, before post-process), add:

```cpp
extern bool g_useGpuStaticProps;
if (g_useGpuStaticProps) {
    GpuStaticPropBatcher::instance().flush();
}
```

- [ ] **Step 3: Build and deploy**

Run `/mc2-build` then `/mc2-deploy`.
Expected: PASS.

- [ ] **Step 4: Verify — flush runs with empty staging**

Launch a mission. Killswitch OFF. Press RAlt+0 (ON).
Expected: no crash, no rendering change (because no `*Appearance::render()` site calls `submit()` yet — flush sees empty staging and returns immediately). Tracy shows a `GpuStaticProps.Flush` zone with near-zero duration. If there's a program-link error, it appears in stderr at this point — fix any GLSL errors.

- [ ] **Step 5: Commit**

```bash
git add GameOS/gameos/gos_static_prop_batcher.cpp GameOS/gameos/gameos_graphics.cpp
git commit -m "feat(props): flush() per-packet instanced draw path"
```

---

## Phase 4 — Killswitch wiring: buildings (Task 11)

### Task 11: Wire `BldgAppearance::render()` behind the killswitch

**Files:**
- Modify: `mclib/bdactor.cpp` (at `BldgAppearance::render`, line 1506)

- [ ] **Step 1: Read the existing render function and resolve the per-child transform question**

Read `mclib/bdactor.cpp:1506..` — the full `BldgAppearance::render` body. Note:
- what the current `bldgShape->Render(...)` call looks like (argument list, return value)
- where `shapeToWorld` and the highlight/fog state are available in the function's scope
- whether there's an early-out (e.g., `if (!inView) return;`) that would prevent `submit()` from being called at all — if so, we need to **bypass** that gate on the GPU path (the whole point is not to cull CPU-side)

**Per-child transform check (critical).** The submit loop below passes the parent's `shapeToWorld` to every child `TG_Shape`. This is only correct if child shapes do not carry their own local-to-parent transform inside `TG_MultiShape::Render()`. Verify:

- Read `TG_MultiShape::Render()` (grep `TG_MultiShape::Render` in `mclib/tgl.cpp`). Does it apply a per-child matrix (e.g., `child->localTransform`, `listOfNodes[i]->relativeTransform`, or similar) before calling each `child->Render`?
- If yes, the submit loop must compose: `childWorldMat = shapeToWorld * child->localTransform`. Otherwise all children will render stacked at the parent origin.
- If no (parent transform is shared verbatim), the loop below is correct as written.

Apply whichever case the code shows — **do not assume**.

- [ ] **Step 2: Add the killswitch branch**

Right before the `bldgShape->Render(...)` call, add:

```cpp
#include "gos_static_prop_killswitch.h"
#include "gos_static_prop_batcher.h"

// ... inside render() ...

if (g_useGpuStaticProps) {
    // Bypass the old inView gate on this path — see spec section
    // "Culling — C2 (render everything)".
    // bldgShape is a TG_MultiShape*; iterate its child TG_Shapes so each
    // authored sub-shape (matching numNodes order) is submitted individually.
    // This preserves packet traversal semantics of the old
    // TG_MultiShape::Render() path.
    bool anyFallback = false;
    const int numChildren = bldgShape->getNumShapes();   // grep real accessor
    for (int i = 0; i < numChildren; ++i) {
        TG_Shape* child = bldgShape->getShape(i);        // grep real accessor
        if (!child) continue;

        uint32_t flags = 0;
        if (child->IsLightsOut()) flags |= (1u << 0);
        if (child->IsWindow())    flags |= (1u << 1);
        if (child->IsSpotlight()) flags |= (1u << 2);

        const uint32_t highlight = child->GetHighlightARGB();  // or read field directly
        const uint32_t fog       = child->GetFogARGB();

        // Layer B: if submit() returns false the type wasn't registered;
        // fall back to the CPU path for this one child this frame.
        if (!GpuStaticPropBatcher::instance().submit(
                child, shapeToWorld, highlight, fog, flags)) {
            anyFallback = true;
        }
    }
    if (anyFallback) {
        // At least one child failed registration — fall the WHOLE multishape
        // back to the CPU path this frame, so that the visual is
        // self-consistent rather than mixed. Next frame the batcher may
        // have more types registered (unlikely — registration is at map
        // load — but keeps the behavior predictable).
        bldgShape->Render(/*existing args*/);
    }
    return /*existing return value*/ 0;
}
// else: old path continues below unchanged
```

Accessor names (`IsLightsOut`, `GetHighlightARGB`, etc.) need to match the real `TG_Shape` API in `mclib/tgl.h` — grep and fix. If no accessor exists, read the public member directly (the fields `lightsOut`, `isWindow`, `isSpotlight`, `aRGBHighlight`, `fogRGB` are all on `TG_Shape` per `tgl.h:725-735`).

- [ ] **Step 3: Bypass inView gate for the update() path**

In `BldgAppearance::update()` (grep `void BldgAppearance::update` in the same file), find the `if (inView)` gate that skips `TransformMultiShape`. Change:

```cpp
if (inView) {
    bldgShape->TransformMultiShape(/*...*/);
}
```

to:

```cpp
if (inView || g_useGpuStaticProps) {
    bldgShape->TransformMultiShape(/*...*/);
}
```

Include the killswitch header.

- [ ] **Step 4: Build and deploy**

Run `/mc2-build` then `/mc2-deploy`.
Expected: PASS.

- [ ] **Step 5: Visual validation — side-by-side**

Launch a mission with lots of buildings (pick one where disappearances were reproducible previously — the session summary mentions wolfman zoom on large maps).

1. Killswitch OFF (default): buildings render via old CPU path, disappearances reproduce at wolfman zoom — confirm the bug is still there as baseline.
2. Press RAlt+0 → ON: buildings render via new GPU path.

Pass criteria:
- Buildings visible at **every** camera angle and zoom, including wolfman.
- No gross visual regressions (wrong color, wrong position, missing textures).
- No crashes.
- Tracy shows `GpuStaticProps.Flush` zone with non-zero instance count.

Minor visual differences (exact shadow tone, fog edge) are expected at this step — full shadow reception is not wired until Task 14.

If buildings render but disappear/flicker: likely a registration gap (damage variants). Check stderr for `[GPUPROPS] unregistered type` messages.

- [ ] **Step 6: Commit**

```bash
git add mclib/bdactor.cpp
git commit -m "feat(props): route BldgAppearance::render through GPU batcher behind killswitch"
```

---

## Phase 5 — Color-address validation (Task 12)

### Task 12: Validate address math via debug modes

**Files:**
- Modify: `GameOS/gameos/gameosmain.cpp` (cycle `MC2_DEBUG_STATIC_PROP_ADDR` via hotkey)

- [ ] **Step 1: Add hotkey to cycle the debug mode**

In `gameosmain.cpp`, in the `alt_debug` key handler, add:

```cpp
case SDLK_MINUS:  // RAlt + '-' cycles 0 → 1 → 2 → 0
    if (alt_debug) {
        int m = (GpuStaticPropBatcher::instance().getDebugAddrMode() + 1) % 3;
        GpuStaticPropBatcher::instance().setDebugAddrMode(m);
        const char* name[] = {"OFF", "GRADIENT", "HASH"};
        fprintf(stderr, "GPU Props Addr Debug: %s\n", name[m]);
    }
    break;
```

- [ ] **Step 2: Build and deploy**

Run `/mc2-build` then `/mc2-deploy`.

- [ ] **Step 3: Validate mode 1 (gradient)**

In-game, killswitch ON, RAlt+- once (mode 1).
Expected: every building becomes a grayscale gradient. The gradient must be **smooth and monotonic** along vertex order within each type. If any building shows banding, wraparound, or discontinuous patches, the `a_localVertexID` indexing is wrong.

Pass: smooth gradient on every building type. Fail: any wraparound or bands.

- [ ] **Step 4: Validate mode 2 (hash)**

RAlt+- again (mode 2).
Expected: each packet renders a unique deterministic per-vertex hash pattern. Two different packets (even on the same building) must render visibly different patterns. Pattern must **not change frame to frame**.

Pass: stable per-packet patterns with no two packets visually identical. Fail: flicker (racing buffers) or identical patterns across packets (address collision).

- [ ] **Step 5: Return to normal**

RAlt+- again (mode 0).
Expected: normal rendering resumes.

- [ ] **Step 6: Commit**

```bash
git add GameOS/gameos/gameosmain.cpp
git commit -m "feat(props): RAlt+- hotkey to cycle color-address debug modes"
```

**Gate:** If modes 1 or 2 fail, stop and fix `a_localVertexID` / `firstColorOffset` / packet address math **before proceeding** to Phase 6. Shadow work on top of broken indexing wastes a day.

---

## Phase 6 — Shadow path (Tasks 13–14)

### Task 13: Shadow shader + `flushShadow()`

**Files:**
- Create: `shaders/static_prop_shadow.vert`
- Create: `shaders/static_prop_shadow.frag`
- Modify: `GameOS/gameos/gos_static_prop_batcher.cpp`

- [ ] **Step 1: Write the shadow shaders**

`shaders/static_prop_shadow.vert`:

```glsl
layout(location = 0) in vec3 a_position;

struct Instance {
    mat4  modelMatrix;
    uint  typeID;
    uint  firstColorOffset;
    uint  flags;
    uint  _pad0;
    vec4  aRGBHighlight;
    vec4  fogRGB;
};

layout(std430, binding = 0) readonly buffer Instances { Instance i[]; } instances_;

uniform mat4 u_lightSpaceMVP;

void main() {
    Instance inst = instances_.i[gl_InstanceID];
    gl_Position = u_lightSpaceMVP * inst.modelMatrix * vec4(a_position, 1.0);
}
```

`shaders/static_prop_shadow.frag`:

```glsl
// AMD invariant 2: must explicitly write gl_FragDepth in depth-only shaders.
layout(location = 0) out vec4 dummy;  // AMD invariant 3: dummy color attachment required

void main() {
    gl_FragDepth = gl_FragCoord.z;
    dummy = vec4(0.0);
}
```

- [ ] **Step 2: Load the shadow program**

In `gos_static_prop_batcher.cpp`, add:

```cpp
GLuint s_staticPropShadowProgram = 0;
```

Extend `loadProgramsIfNeeded`:

```cpp
if (!s_staticPropShadowProgram) {
    s_staticPropShadowProgram = makeProgram("#version 420\n",
                                            "shaders/static_prop_shadow.vert",
                                            "shaders/static_prop_shadow.frag");
}
```

- [ ] **Step 3: Implement `flushShadow`**

Replace the stub:

```cpp
void GpuStaticPropBatcher::flushShadow() {
    ZoneScopedN("GpuStaticProps.Shadow");
    if (s_fatalRegistrationFailure) return;
    if (!uploadAllBucketsIfNeeded()) return;  // shares upload with flush()

    // --- AMD invariants around shadow draw ---
    // Invariant 4: unbind shadow map from sampler unit 9 before rendering
    // INTO the shadow FBO. Engine-owned: the shadow map is managed by
    // gosPostProcess; we ask it to rebind after we're done rather than
    // query/save/restore.
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram(s_staticPropShadowProgram);
    glBindVertexArray(s_sharedVao);

    // Engine-owned state (AMD invariants, no save/query/restore).
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // lightSpaceMVP — read from the same frame-snapshot global the existing
    // object-shadow collector uses (grep gos_DrawShadowObjectBatch in
    // gameos_graphics.cpp for the matrix source).
    extern const float* getDynamicShadowLightSpaceMatrix();
    glUniformMatrix4fv(glGetUniformLocation(s_staticPropShadowProgram, "u_lightSpaceMVP"),
                       1, GL_FALSE, getDynamicShadowLightSpaceMatrix());

    for (uint32_t typeID = 0; typeID < s_types.size(); ++typeID) {
        auto rit = s_typeRanges.find(typeID);
        if (rit == s_typeRanges.end()) continue;
        const TypeRangeSsbo& r = rit->second;
        const GpuStaticPropType& type = s_types[typeID];

        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, s_instanceSsbo,
                          static_cast<GLintptr>(r.instanceByteOffset),
                          static_cast<GLsizeiptr>(r.instanceByteSize));

        for (uint32_t p = 0; p < type.packetCount; ++p) {
            const GpuStaticPropPacket& pkt = s_packets[type.firstPacket + p];
            // M1 step 1 (buildings only): skip alpha-test packets — they
            // leak depth from leaves/windows. Task 16 (trees) replaces this
            // skip with a texture-sampled discard in the shadow shader.
            if (pkt.materialFlags & STATIC_PROP_FLAG_ALPHA_TEST) continue;

            glDrawElementsInstancedBaseVertex(
                GL_TRIANGLES,
                pkt.indexCount,
                GL_UNSIGNED_INT,
                reinterpret_cast<void*>(pkt.firstIndex * sizeof(uint32_t)),
                r.instanceCount,
                pkt.baseVertex);
        }
    }

    glBindVertexArray(0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    // Invariant 4: ask the shadow map owner to rebind unit 9. The binding
    // will also be re-established by the next terrain/object draw that
    // samples the shadow map via its own uniform setup, but doing it here
    // keeps the invariant's scope local to this function.
    extern void gosPostProcessRebindShadowSampler();  // implement in gos_postprocess.cpp
    gosPostProcessRebindShadowSampler();
    glActiveTexture(GL_TEXTURE0);
}
```

Also adjust `flush()` to **skip upload** if `s_lastUploadedSlot == s_frameSlot` — split the upload into a helper.

- [ ] **Step 4: Ensure the Shadow.DynPass FBO has a color attachment**

Open `GameOS/gameos/gos_postprocess.cpp` (or wherever `Shadow.DynPass` is defined — grep the string). Inspect whether the FBO has a color attachment. If it's depth-only, attach a 1×1 `GL_R8` renderbuffer as `GL_COLOR_ATTACHMENT0` for AMD invariant 3. If it already has one (e.g., reused from an existing pass), leave it.

Note: the batcher's shadow draw must happen **while that FBO is bound**. Task 14 wires that.

- [ ] **Step 5: Build and deploy**

Run `/mc2-build` then `/mc2-deploy`.
Expected: PASS. (Not yet called from anywhere — safe.)

- [ ] **Step 6: Commit**

```bash
git add shaders/static_prop_shadow.vert shaders/static_prop_shadow.frag \
        GameOS/gameos/gos_static_prop_batcher.cpp GameOS/gameos/gos_postprocess.cpp
git commit -m "feat(props): shadow shader pair + flushShadow() with AMD invariants"
```

---

### Task 14: Wire `flushShadow()` into `Shadow.DynPass` + gate `g_shadowShapes`

**Files:**
- Modify: `mclib/txmmgr.cpp` (where `Shadow.DynPass` is invoked)
- Modify: `mclib/tgl.cpp` (where static-prop shapes are added to `g_shadowShapes`)

- [ ] **Step 1: Gate the shadow-shape collector**

Open `mclib/tgl.cpp`. Grep `g_shadowShapes` — find the site where static-prop shapes are pushed into it (the collector site for `TG_Shape::Render`). Wrap:

```cpp
#include "gos_static_prop_killswitch.h"
// ...
// Inside the collector, before the push:
if (g_useGpuStaticProps) {
    // The GPU batcher handles this shape's shadow — don't collect.
    // (Mechs use a different collector path and are unaffected.)
    return;  // or continue, depending on the loop structure
}
g_shadowShapes.push_back(shape);
```

Mech shadows go through a different path (see memory on `Shadow.DynPass`) and are not affected — do not gate the mech collector.

- [ ] **Step 2: Call `flushShadow()` inside Shadow.DynPass**

Open `mclib/txmmgr.cpp`. Grep `Shadow.DynPass` — find where the per-frame dynamic-shadow draw happens. After the existing collector-based draw loop (or at the equivalent slot if the collector is now empty), add:

```cpp
#include "gos_static_prop_killswitch.h"
#include "gos_static_prop_batcher.h"
// ...
if (g_useGpuStaticProps) {
    GpuStaticPropBatcher::instance().flushShadow();
}
```

This must run **while the shadow FBO is bound** and **before** the FBO is unbound or the shadow map's sampler-unit-9 rebind happens elsewhere.

- [ ] **Step 3: Build and deploy**

Run `/mc2-build` then `/mc2-deploy`.
Expected: PASS.

- [ ] **Step 4: Visual validation — shadows**

Launch mission. Killswitch ON.

Pass criteria:
- Buildings cast shadows on terrain.
- Shadow quality visually matches the killswitch-OFF baseline (toggle to compare).
- No sampler-9 feedback loop black screens.
- No stuck-on shadow from previous frame.
- First 5 frames after mission load match the killswitch-OFF behavior (both are "correct eventually" or both have first-frame artifacts — they shouldn't differ).

- [ ] **Step 5: Commit**

```bash
git add mclib/tgl.cpp mclib/txmmgr.cpp
git commit -m "feat(props): wire GPU shadow flush into Shadow.DynPass behind killswitch"
```

---

## Phase 7 — M1 step 1 sign-off (Task 15)

### Task 15: Full buildings validation + record baselines

**Files:**
- Modify: `docs/superpowers/plans/progress/2026-04-19-gpu-static-prop-renderer-progress.md`

- [ ] **Step 1: Run the full test plan from the spec for buildings only**

In-game with killswitch toggled, validate each bullet from the spec's `§Test plan`:

1. Standard RTS zoom, pan a building-heavy map. Both paths: no disappearances.
2. Wolfman zoom (altitude 6000). Killswitch ON: no disappearances (fix confirmed). Killswitch OFF: disappearances should reproduce (baseline).
3. Pause and pan. Both paths: no disappearances while paused.
4. Destroy a building. Mesh swap renders correctly on GPU path.
5. Dynamic shadows from buildings land correctly on terrain.
6. Tracy: `GpuStaticProps.Submit`, `GpuStaticProps.Flush`, `GpuStaticProps.Shadow` zones visible with non-zero duration. Compare total to old-path `Render.3DObjects` zone.

- [ ] **Step 2: Update progress tracker**

Append to `progress/2026-04-19-gpu-static-prop-renderer-progress.md`:

```markdown
## M1 step 1 (buildings) sign-off — YYYY-MM-DD

- [x] Bug reproduces with killswitch OFF at wolfman zoom
- [x] Bug fixed with killswitch ON at wolfman zoom
- [x] Paused-camera visibility correct on GPU path
- [x] Damage mesh swap correct on GPU path
- [x] Shadows cast correctly on GPU path
- [x] Tracy zones observed; new path ~<Xms> vs old <Yms>
- Notes: <anything surprising>
```

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/plans/progress/2026-04-19-gpu-static-prop-renderer-progress.md
git commit -m "docs: M1 step 1 (buildings) validation sign-off"
```

**Do not proceed to Phase 8 if any test criterion above fails.** Fix underlying cause first.

---

## Phase 8 — Expand to remaining types (Tasks 16–17)

### Task 16: Wire `TreeAppearance` (alpha-test gate first)

**Files:**
- Modify: `mclib/bdactor.cpp` at `TreeAppearance::render` (line 3910)
- Modify: `shaders/static_prop_shadow.frag` (add alpha-test `discard` for leaves)

- [ ] **Step 1: Audit tree material for `GL_BLEND` usage**

Grep `mclib/bdactor.cpp` for how `TreeAppearance::render` currently submits to MLR/mcTextureManager. Confirm whether leaves use alpha-test (`discard`) or alpha-blend. Also check `TG_TypeShape::alphaTestOn` for tree types by reading the tree type-load path (grep for `LoadTGShapeFromASE` calls in tree init and trace `SetAlphaTest`).

- If alpha-test only: proceed.
- If any tree uses alpha-blend: per the spec §Risk 3, trees stay on the old path — return from this task with a note in the progress tracker and add a follow-up spec for a sorted transparent batch. Do NOT proceed.

- [ ] **Step 2: Extend shadow shader for alpha-test leaves**

Update `shaders/static_prop_shadow.frag` to sample the diffuse texture and `discard` on alpha-test. This requires passing UV and the texture handle through the shadow pipeline.

Minimal change — add UV to the shadow vertex shader:

```glsl
// static_prop_shadow.vert — add:
layout(location = 2) in vec2 a_uv;
out vec2 v_uv;

// In main(): v_uv = a_uv;
```

Fragment:

```glsl
// static_prop_shadow.frag — replace with:
in vec2 v_uv;
uniform sampler2D u_tex;
uniform uint  u_materialFlags;
layout(location = 0) out vec4 dummy;
const uint ALPHA_TEST_BIT = 1u;

void main() {
    if ((u_materialFlags & ALPHA_TEST_BIT) != 0u) {
        if (texture(u_tex, v_uv).a < 0.5) discard;
    }
    gl_FragDepth = gl_FragCoord.z;
    dummy = vec4(0.0);
}
```

Update `flushShadow()` in the batcher: per-packet, bind texture + upload `u_materialFlags`, and stop skipping alpha-test packets.

- [ ] **Step 3: Wire the killswitch branch in `TreeAppearance::render`**

Mirror Task 11's approach at `mclib/bdactor.cpp:3910`. Same submit call shape, same update-gate bypass.

- [ ] **Step 4: Build, deploy, validate**

Run `/mc2-build` and `/mc2-deploy`.

Pass criteria:
- Trees visible, leaves correct (no broken alpha).
- Tree shadows cast correctly without leaking through leaves.
- Toggle RAlt+0 a few times — behavior remains stable.

- [ ] **Step 5: Commit**

```bash
git add mclib/bdactor.cpp shaders/static_prop_shadow.vert shaders/static_prop_shadow.frag \
        GameOS/gameos/gos_static_prop_batcher.cpp
git commit -m "feat(props): wire TreeAppearance with alpha-test shadow discard"
```

---

### Task 17: Wire `GenericAppearance`, `GVAppearance`, and plane fallback

**Files:**
- Modify: `mclib/genactor.cpp` at `GenericAppearance::render`
- Modify: `mclib/gvactor.cpp` at `GVAppearance::render`
- Modify: `mclib/appear.cpp` at the plane-mesh fallback render (grep `class Appearance` for the base `render()` that the plane fallback uses)

- [ ] **Step 1: Apply the same killswitch branch + update-gate bypass in all three files**

Each site follows the Task 11 pattern exactly:

```cpp
// Use the same child-iteration + Layer B fallback pattern as Task 11.
// myShape is a TG_MultiShape*; iterate its child TG_Shapes.
if (g_useGpuStaticProps) {
    bool anyFallback = false;
    const int numChildren = myShape->getNumShapes();
    for (int i = 0; i < numChildren; ++i) {
        TG_Shape* child = myShape->getShape(i);
        if (!child) continue;
        uint32_t flags = 0;
        if (child->IsLightsOut()) flags |= (1u << 0);
        if (child->IsWindow())    flags |= (1u << 1);
        if (child->IsSpotlight()) flags |= (1u << 2);
        if (!GpuStaticPropBatcher::instance().submit(
                child, shapeToWorld,
                child->GetHighlightARGB(), child->GetFogARGB(), flags)) {
            anyFallback = true;
        }
    }
    if (anyFallback) myShape->Render(/*existing args*/);
    return 0;
}
```

In each `update()`, change `if (inView)` to `if (inView || g_useGpuStaticProps)`.

- [ ] **Step 2: Build, deploy, validate**

Run `/mc2-build` and `/mc2-deploy`.

Pass criteria:
- All static-prop types render via GPU path when killswitch ON.
- No new disappearances.
- No crashes toggling killswitch repeatedly during gameplay.
- Registration logs (`[GPUPROPS] unregistered type`) do not fire — if they do, fix the registration walk to cover the missing variant.

- [ ] **Step 3: Update progress tracker and run full spec test plan one more time**

Append final M1 sign-off to the progress file (all three sub-steps complete).

- [ ] **Step 4: Commit**

```bash
git add mclib/genactor.cpp mclib/gvactor.cpp mclib/appear.cpp \
        docs/superpowers/plans/progress/2026-04-19-gpu-static-prop-renderer-progress.md
git commit -m "feat(props): wire GenericAppearance, GVAppearance, plane fallback

Completes M1: all five static-prop appearance types route through the GPU
batcher when g_useGpuStaticProps is set. Follow-up: remove the old CPU
branch (tracked in spec §Migration step 4)."
```

---

## Self-review notes

After completing all tasks, the killswitch branch in the five `*Appearance::render()` sites is a declared bridge per the render contract (§Bucket D, exit plan: step M1.4 in the spec). A follow-up plan will delete the old CPU branch once the new path has been the default for enough play time to be confident.

Known deliberately-deferred items (tracked in spec risks, not this plan):
- Instance batching via ARB_base_instance contiguous SSBO regions (perf optimization).
- Sorted transparent pass for any blended static props (spec §Risk 3).
- Retiring `mcTextureManager` static-prop submission entirely (M1 step 4 in spec).
