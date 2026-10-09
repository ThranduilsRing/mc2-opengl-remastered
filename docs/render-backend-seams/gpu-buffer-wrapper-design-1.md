# GPU-BUFFER-WRAPPER-DESIGN-1

Design for a GpuBuffer / GpuRingBuffer RAII abstraction + a compile-time
binding-slot registry, scoped from the buffer census in
[gpu-buffer-owner-recon-1.md](gpu-buffer-owner-recon-1.md).

**Status:** DESIGN ONLY. No wrapper code, no engine edits in this slice. Every
"current code" claim is file:line against worktree nifty-mendeleev.
Part of the RENDER-BACKEND-SEAMS arc (slice 3). Prior: RENDER-PASS-CONTRACT-ENFORCEMENT-1
(8d250041), GPU-BUFFER-OWNER-RECON-1, WATER-THINRING-FENCE-1 (shipped -- the water
thin-ring now fences, gos_terrain_water_stream.cpp:2154-2168).

The wrapper generalizes patterns that already exist and are proven in three files:
gos_mech_batcher.cpp, gos_static_prop_batcher.cpp, gos_terrain_water_stream.cpp.
It does NOT invent new GPU semantics -- it folds the existing
fence/coherent-ring/align/bind discipline into one owner type so the discipline
cannot drift between structurally-identical sites (the same drift that shipped >=3
ordering bugs, per gos_gpu_sync.h:6-13).

## 0. Non-goals / boundaries

- Not a memory allocator / sub-allocator. One GpuBuffer == one GL buffer object.
- Does not replace gos_gpu_sync.h (gpuSyncBarrier, gpuBindSsboRange, gpuAlignUp,
  gpuSsboOffsetAlignment). The wrapper consumes those helpers; they remain the
  single source of barrier/alignment truth.
- Does not replace the MC2_GL_BufferData hitch-accounting macro
  (mc2_hitch_trace.h:135). The wrapper upload path calls through the same accounting
  (Sec 1.3) so Tier-1 telemetry survives migration.
- No Vulkan implementation. Sec 6 is a forward-mapping note only.

## 1. GpuBuffer -- RAII owner over one GL buffer

Subsumes Tier-0 (raw glBufferData/SubData, no helper) and Tier-1 (MC2_GL_BufferData
hitch macro). Two construction modes selected by a flag, not two classes, so call
sites stay uniform.

### 1.1 Modes

| Mode | GL backing | Update path | Covers (recon) |
|---|---|---|---|
| Immutable | glBufferStorage, no DYNAMIC_STORAGE bit | none after create (or full-recreate) | lod_chunk vbo/ibo/skirt, mech/static shared VBO/IBO, recipe SSBOs, permutation/per-draw/cmd-to-bucket, postprocess quadVBO_ |
| DynamicMutable | glBufferStorage(GL_DYNAMIC_STORAGE_BIT) preferred OR glBufferData (legacy orphan-realloc) | upload() full / uploadSub() range | s_heightSsbo dirty SubData, HUD gosMesh per-batch, LUT/quad-window SubData, light SSBO grow, MaterialGpu orphan-realloc |

Recon finding #5: DSA is absent today -- everything is glGenBuffers + bind-to-edit
(e.g. gos_mech_batcher.cpp:703-704, gos_terrain_water_stream.cpp:701-702). The
wrapper is the clean place to adopt glCreateBuffers + named-buffer DSA uniformly
(glNamedBufferStorage, glNamedBufferSubData), which is also the closest GL semantic
to a Vulkan VkBuffer allocation. DSA removes the bind-to-edit dance and its hidden
global binding-point clobber.

### 1.2 API sketch (illustrative -- not final)

    enum class GpuBufferKind   { Vertex, Index, Storage, Uniform, Indirect };
    enum class GpuBufferUpdate { Immutable, DynamicMutable };

    struct GpuBufferDesc {
        GpuBufferKind   kind;
        GpuBufferUpdate update;
        GLsizeiptr      bytes;        // initial size
        const void*     initialData;  // nullptr = uninitialized
        const char*     debugTag;     // KHR_debug label + residency report key
        bool            allowGrow;    // DynamicMutable only: orphan-realloc on grow
        bool            diagnostic;   // exclude from residency totals (finding #4)
    };

    class GpuBuffer {
    public:
        explicit GpuBuffer(const GpuBufferDesc&); // glCreateBuffers + storage
        ~GpuBuffer();                             // glDeleteBuffers
        GpuBuffer(GpuBuffer&&) noexcept;          // move-only RAII owner
        GpuBuffer(const GpuBuffer&)            = delete;
        GpuBuffer& operator=(const GpuBuffer&) = delete;
        void upload(const void* data, GLsizeiptr bytes);                // full
        void uploadSub(GLintptr off, GLsizeiptr bytes, const void* d);  // range
        void bindBase (BindingSlot slot);                          // glBindBufferBase
        void bindRange(BindingSlot slot, GLintptr off, GLsizeiptr sz); // via gpuBindSsboRange
        void bindTarget() const;                                   // Vertex/Index/Indirect
        GLuint id() const; GLsizeiptr bytes() const; const char* tag() const;
    };

### 1.3 Behavior-preservation requirements

- Hitch accounting must survive. Tier-1 sites (gos_static_prop_batcher.cpp:986,
  :3874, :4049, :7615; all of gos_terrain_indirect.cpp) currently route through
  MC2_GL_BufferData. The wrapper upload()/uploadSub() MUST increment the same
  g_mc2HitchAccum.glBufferData* counters (mc2_hitch_trace.h:135-151) so the hitch
  trace reads identically after migration. Choice: call the macro from inside the
  wrapper, OR bump the accumulator directly. Recommend the latter for DSA (the macro
  hardcodes the bind-to-target form). See OD-4.
- allowGrow orphan-realloc must match today MaterialGpu / light-SSBO grow behavior
  (gameos_graphics.cpp:8531 LIGHT_DATA path): on grow, recreate storage and re-bind
  base. Immutable buffers cannot grow -- assert.
- KHR_debug label from debugTag (new; today buffers are unlabeled). Free win for
  RenderDoc / residency.

## 2. GpuRingBuffer<N> -- N-frame fenced persistent-coherent ring

Generalizes Tier-2. The proven template is the mech batcher; the static-prop batcher
is the same shape; the water thin-ring is the same shape with a heap (not
persistent-mapped) backing. All four (mech, static-prop, water-thin, solid-thin)
must be expressible as one type.

### 2.1 The template, extracted from current code

Mech batcher (gos_mech_batcher.cpp), canonical lifecycle:

1. Create (:701-717): glBufferStorage sized RING_FRAMES * perSlotCap * stride, flags
   GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT, then
   glMapBufferRange over the whole thing (one persistent map).
2. Per-slot align (:695-699): round perSlotCap (in ELEMENTS) up to
   gpuSsboOffsetAlignment() via gpuAlignUp so every slot*cap*stride bind offset is
   aligned -- NVIDIA rejects misaligned glBindBufferRange (gos_gpu_sync.h:46-54).
3. Advance + wait (:1699-1704): s_frameSlot = (s_frameSlot+1) % N; if a fence guards
   that slot, glClientWaitSync(...) then glDeleteSync.
4. Write (:1706-1707): dst = map + frameSlot * perSlotCap -- memcpy, no GL call
   (coherent map).
5. Bind (gpuBindSsboRange, the checked range bind).
6. Draw.
7. Fence after draw (:2273): s_fence[s_frameSlot] =
   glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0).
8. Teardown (:671-675, :799-803, :1443-1447): drain all N fences with glClientWaitSync
   + glDeleteSync before delete.

MECH_RING_FRAMES == STATIC_PROP_RING_FRAMES is static_assert-enforced
(gos_mech_batcher.cpp:44) because a parity SSBO is shared -- the wrapper makes the
depth a template parameter so this coupling is expressed once.

Water thin-ring (gos_terrain_water_stream.cpp) is the NON-persistent-mapped variant:
heap-staged then glBufferData per slot, fence created in EndThinRingFrameFence()
(:2154-2168), waited in WaitAndClearThinFenceForCurrentSlot() (:105-121) with a 10ms
timeout (vs mech GL_TIMEOUT_IGNORED). The wrapper must support both backing
strategies and both wait policies (OD-2).

### 2.2 API sketch

    template <uint32_t N>
    class GpuRingBuffer {
    public:
        struct Desc {
            GpuBufferKind kind;        // Storage (typical) or Vertex
            GLsizeiptr    stride;      // element size in bytes
            size_t        perSlotCap;  // elements per frame slot (rounded internally)
            BackingMode   backing;     // PersistentCoherent | HeapStagedBufferData
            WaitPolicy    wait;        // BlockIgnoreTimeout | Timeout(ns)
            const char*   debugTag;
        };
        explicit GpuRingBuffer(const Desc&); // align cap, alloc N*cap*stride, map if coherent
        ~GpuRingBuffer();                    // drainAllFences() then delete
        void  beginFrame();                  // advance slot + wait/clear that slot fence (step 3)
        void* slotPtr();                     // coherent: map + slot*cap*stride ; heap: staging ptr
        void  commitSlot(size_t usedElems);  // heap: glBufferData(slot range); coherent: no-op
        void  bind(BindingSlot s);           // gpuBindSsboRange at slot*cap*stride
        void  endFrameFence();               // glFenceSync into s_fence[slot] (step 7)
        bool  ensureCapacity(size_t elems);  // grow perSlotCap (drain fences, realloc, remap)
        void  drainAllFences();              // teardown / mission-unload
        uint32_t currentSlot() const; GLsizeiptr perSlotBytesAligned() const;
    };

ensureCapacity mirrors ensureRingCapacity (gos_mech_batcher.cpp:1692): must drain
fences before reallocating storage, then re-map. Growing a persistent-mapped
immutable buffer == delete+recreate+remap (GL immutable storage cannot resize).

### 2.3 What the wrapper enforces that hand-code forgets

- Fence-after-draw is not optional. The water ring shipped UNFENCED (recon finding
  #2) until WATER-THINRING-FENCE-1. A GpuRingBuffer exposing
  beginFrame()/endFrameFence() makes the missing fence a missing CALL, not a missing
  CONCEPT -- debug builds can assert endFrameFence() once per beginFrame().
- Align-in-elements (step 2) happens in the ctor, not at each call site.
- Teardown drains fences -- three separate drain loops exist in the mech file alone
  (:671, :799, :1443); the dtor centralizes them.

## 3. Binding-slot registry (recon finding #3)

> **⚠ SUPERSEDED (corrected 2026-06-22 RENDER-CONTRACT-INDEX-1).** The flat
> `GpuBindingSlots.h` enum proposed in §3.2 below is SUPERSEDED by the
> **multiplexed-per-pass** model from GPU-BINDING-SLOTS-LOCKSTEP-1. A flat
> one-value-per-slot enum encodes a FALSE model: GPU base-binding slots are
> intentionally multiplexed per pass (slot 0 = 7 buffers, slot 2 = 6+, slot 7 =
> 4 across mech/static-prop/cull/terrain/particle/gpu-driven), so a slot number
> is semantic ONLY inside a pass/pipeline. The flat enum was NOT built. The
> shipped solution is check-time only:
> - `scripts/check-binding-slots.py` (preprocessor-branch-aware C++↔GLSL lockstep + same-pass collision check)
> - `docs/render-backend-seams/binding-slot-occupancy.{md,json}` (the occupancy map)
>
> The OD-1 "enum + per-pass occupancy table" recommendation resolved toward the
> occupancy table alone (no flat enum). §4 below (per-subsystem GpuBuffer /
> GpuRing adoption) is unaffected and remains valid future work. Read §3.1–3.3
> as historical design rationale only.

### 3.1 The problem, from current code

Slot assignment is scattered across per-subsystem headers with no cross-check:

- kWaterRecipeSsboBinding = 5, kWaterThinSsboBinding = 6 (gos_terrain_water_stream.h:108-109)
- TERRAIN_HEIGHT_SSBO_BINDING = 23, TERRAIN_CEMENT_SSBO_BINDING = 25 (gos_terrain_lod_chunk.h:16,18)
- kMaskSolidSsboBinding = 17, kMaskWaterSsboBinding = 18, kMaskSolidRecipeBinding = 19 (gos_terrain_mask_dispatch.h:16-18)
- LIGHT_DATA_SSBO_BINDING 20 defined TWICE -- GameOS/include/gameos.hpp:2880 (C++) and
  shaders/include/lighting.hglsl:15 (GLSL), kept in lockstep by a comment
  (lighting.hglsl:14), not by tooling.
- kMechMaterialTableBinding = 7 (gos_materials.cpp:63), kViewUniformsBinding = 3
  (RenderCore/ViewUniforms.h:38), READBACK_SSBO_BINDING = 14 (gpu_cull_readback.h:18).

And mode-dependent reuse (finding #3): slot 0 = every instance SSBO; slot 9 =
cull-debug (C1a) vs visibleIds (C1b); slot 11 = bucket-caps vs indirect-cmd patch;
slot 2 = terrain thin-range vs per-type hot-color vs cull frustum UBO in different
subsystems. None of this is visible at compile time today.

### 3.2 Design: one header, typed slot, compile-visible collisions

A single GpuBindingSlots.h enumerates every SSBO/UBO base binding as a typed
constant, grouped by exclusivity domain (which slots can coexist in one draw vs which
are mode-alternates). Make the slot an enum-class (BindingSlot) so bindBase/bindRange
cannot take a raw int -- every bind names its slot from the registry. The existing
scattered constants (kWaterThinSsboBinding, TERRAIN_HEIGHT_SSBO_BINDING,
LIGHT_DATA_SSBO_BINDING, etc.) are deleted from their headers and re-exported from
this one, so there is exactly one definition site.

    enum class BindingSlot : uint32_t {
        // Per-draw instance domain (slot 0 family: MUTUALLY EXCLUSIVE per pass)
        InstanceData      = 0,   // mech / static-prop legacy / coalesce / popsplit / shadow
        BoneData          = 1,
        ViewUniforms      = 3,   // RenderCore/ViewUniforms.h:38
        MechMaterialTable = 7,   // gos_materials.cpp:63
        LightData         = 20,  // gameos.hpp:2880 + lighting.hglsl:15 (SINGLE SOURCE)
        WaterRecipe       = 5,   // gos_terrain_water_stream.h:108
        WaterThin         = 6,   // gos_terrain_water_stream.h:109
        MaskSolid         = 17,  // gos_terrain_mask_dispatch.h:16
        MaskWater         = 18,  // gos_terrain_mask_dispatch.h:17
        MaskSolidRecipe   = 19,  // gos_terrain_mask_dispatch.h:18
        TerrainHeight     = 23,  // gos_terrain_lod_chunk.h:16
        TerrainCement     = 25,  // gos_terrain_lod_chunk.h:18
        Readback          = 14,  // gpu_cull_readback.h:18
        // ... (full census mapped from recon master inventory) ...
    };

Mode-dependent slots (9, 11, 2) are the hard part. Two representable options:

- Encode them as alternates within a named domain, e.g. CullSlot9 { DebugC1a = 9,
  VisibleIdsC1b = 9 } -- same value, distinct names, so the call site documents which
  mode, and a static_assert confirms the shared value is intentional.
- Provide a per-pass occupancy table (a constexpr array of which slots a pass binds)
  plus a compile-time / startup check that no pass double-binds a slot. This catches
  a REAL collision (two live buffers fighting for one slot in one pass) while
  permitting INTENTIONAL cross-pass reuse.

Recommend BOTH: the enum is the naming source of truth; the per-pass occupancy table
is the collision detector. The enum alone cannot distinguish "slot 9 is debug here,
visibleIds there" (legal) from "two buffers both want slot 9 in this pass" (bug) --
only the occupancy table can. See OD-1.

### 3.3 GLSL lockstep

LIGHT_DATA_SSBO_BINDING is defined in both C++ and GLSL today (gameos.hpp:2880,
lighting.hglsl:15). The registry should be the C++ source and EMIT the GLSL define
block (generated header included by shaders, or a scripts/check-*.py that diffs the
two). Do NOT hand-maintain two copies -- that is the exact lockstep-by-comment
pattern the recon flags as fragile.

## 4. Migration / adoption order

Each step is its own future slice: behavior-preserving, smoke-gated (tier1 =
mc2_01/03/10/17/24), one subsystem at a time. Lowest-risk-first.

| # | Slice | Target | Why this order |
|---|---|---|---|
| A | TIER0-LODCHUNK | gos_terrain_lod_chunk.cpp 9 static buffers (ps.vbo/ibo/skirt*, s_heightSsbo, s_typeSsbo, s_cementSsbo) | Recon: cleanest target. All Immutable except height (DynamicMutable dirty SubData). No ring, no fence, no existing wrapper to reconcile. Proves GpuBuffer + slot registry end-to-end. |
| B | TIER0-HUD | gameos_graphics.cpp 6 gosMesh VBO/IBO pairs (~11 buffers) | All DynamicMutable allowGrow per-batch glBufferData. Proves orphan-realloc + hitch accounting parity. |
| C | TIER0-POSTPROCESS | gos_postprocess.cpp quadVBO_ + gameos_graphics.cpp surface VB/IB/TB + light SSBO | Mixes Immutable quad + epoch-static surface + growable light SSBO. Last Tier-0 family. |
| D | (defer) TIER1 | gos_terrain_indirect.cpp, water-stream non-ring buffers | Already has working hitch macro; convert only after GpuBuffer.upload() proves accounting parity in A-C. |
| E | (defer) TIER2 | mech / static-prop / water-thin / solid-thin rings | Already have working fence/ring logic. Convert to GpuRingBuffer<N> ONLY after the wrapper demonstrably subsumes all four lifecycles in a parity probe. Highest blast radius (the shipped GPU-driven paths). |

Gate per slice: dual-run where feasible (old vs wrapper path, compare draw output),
tier1 5/5 unset + gate-on, then default-on. The Tier-2 conversions should ride an
MC2_GPUBUF_RING gate and soak before the planned flip to default-ON (reader default is currently OFF), because a regression there =
invisible/garbled mechs or props (the exact failure class in gos_gpu_sync.h:8-12).

## 5. Diagnostics / residency hook

- Every GpuBuffer/GpuRingBuffer registers in a process-global intrusive list at ctor,
  deregisters at dtor (RAII -- no leak in the report).
- MC2_GPUBUF_RESIDENCY=1 dumps: tag, kind, update mode, bytes (ring: N *
  perSlotBytesAligned), live fence count, last-upload frame. One line per buffer,
  sorted by bytes.
- Exclude the 4 diagnostic buffers the recon flagged (finding #4): g_thinCanarySSBO
  (slot 7, never read), g_solidBucketHeaderSsbo, water s_spikeThin / s_spikeHeader.
  Mechanism: GpuBufferDesc::diagnostic excludes the buffer from residency totals
  (still listed, flagged [diag], not summed) so accounting is honest. These never
  migrate in the adoption plan; if they ever do, the flag keeps them out of the real
  total.
- Reuse the existing hitch accumulator (mc2_hitch_trace.h) for upload byte/call
  counts rather than a parallel counter.

## 6. Vulkan-forward note (mapping only -- NOT an implementation)

| GL wrapper element | Vulkan target |
|---|---|
| GpuBuffer (Immutable) | VkBuffer + VkDeviceMemory, DEVICE_LOCAL, usage from GpuBufferKind (STORAGE/VERTEX/INDEX/UNIFORM/INDIRECT) |
| GpuBuffer (DynamicMutable persistent-coherent) | HOST_VISIBLE + HOST_COHERENT VkBuffer, persistently mapped |
| GpuRingBuffer<N> | N frames-in-flight; one VkBuffer sized N * perSlotCap, per-frame dynamic descriptor offset slot*cap*stride |
| per-slot GLsync fence | per-frame VkFence (acquire/submit), waited at beginFrame() |
| glMemoryBarrier (via gpuSyncBarrier) | vkCmdPipelineBarrier compute->indirect/vertex (already typed in gos_gpu_sync.h) |
| BindingSlot registry | descriptor-set layout binding numbers (the registry IS the proto descriptor layout) |
| SSBO offset alignment (gpuSsboOffsetAlignment) | minStorageBufferOffsetAlignment |

The cull/indirect paths are already the most Vulkan-ready (explicit barriers); the
slot registry is the single most valuable Vulkan-prep artifact because a descriptor
set layout REQUIRES exactly the dense, collision-free slot namespace the registry
enforces.

## 7. Open decisions for the human

- OD-1 -- collision detection mechanism. Enum-only naming vs enum + per-pass
  occupancy table. Mode-dependent reuse (slots 9/11/2) means the enum alone cannot
  tell legal cross-pass reuse from an illegal same-pass collision. Recommend enum
  (naming) PLUS a constexpr per-pass occupancy table (collision check). Cost: each
  pass declares its slot set. Decide before TIER0-LODCHUNK (the registry ships with
  that slice).
- OD-2 -- ring backing strategy is ambiguous in current code. Mech/static-prop use
  persistent-coherent map (gos_mech_batcher.cpp:708); water-thin uses heap-stage +
  glBufferData (gos_terrain_water_stream.cpp:730). Wait policy also differs: mech
  GL_TIMEOUT_IGNORED (:1701) vs water 10ms (gos_terrain_water_stream.cpp:110). Do NOT
  force one strategy. GpuRingBuffer parameterizes both BackingMode and WaitPolicy; the
  human decides whether to unify during Tier-2 conversion (slice E) or keep
  per-call-site.
- OD-3 -- DSA adoption scope. Recon finding #5: no DSA anywhere. Adopt
  glCreateBuffers/named-buffer DSA in the wrapper from day one (cleaner,
  Vulkan-closer), or keep bind-to-edit for bit-identical behavior in early slices?
  Recommend DSA in the wrapper but verify on the AMD + NVIDIA pair before default-on
  (changes the GL call stream -- RenderDoc captures differ).
- OD-4 -- MC2_GL_BufferData macro vs wrapper. Keep calling the macro from inside
  upload() (preserves target-form accounting exactly) or bump g_mc2HitchAccum directly
  (required for DSA named-buffer form)? Recommend direct accumulator bump; verify hitch
  trace byte/call totals match pre-migration on tier1.
- OD-5 -- Tier-2 conversion gating. These are the shipped GPU-driven paths; conversion
  blast radius is invisible/garbled mechs+props. Confirm the gate name (MC2_GPUBUF_RING?)
  and the soak duration before default-on.
