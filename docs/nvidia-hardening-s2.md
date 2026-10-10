<!-- doc-status: current | reviewed:2026-10-11 -->
# NVIDIA Hardening S2 — plan

Branch target: `claude/nvidia-hardening-s2` (off nifty after render-hygiene-s1 lands)

## Status (2026-06-16)

| Item | Status | Commit |
|------|--------|--------|
| 1. gpuBindSsboRange wrapper (24 sites) | DONE | `61b3684b` |
| 2. GPU buffer zero-init audit | CLEAN — no changes needed | `bb550ed4` (comment fix) |
| 3. Render contract assert mode | DONE | `50ee010a` |
| 4. Strict shader/VAO diagnostics | DONE (compile+link warnings) | `594eafe3` |
| 5. PBO unbind discipline | CLEAN — GlPixelStoreGuard + applyPBO already correct | (no commit) |
| 6. Explicit pass-entry state | CLEAN — mech uses applyPipeline, decal explicit, post-process explicit | (no commit) |
| 7. Named memory barriers | DONE — table extended (CpuMappedRead/BufferReadback), cardcloud migrated | `90b6aa5a` |
| 8. NVIDIA validation smoke | PENDING (requires NVIDIA HW) | — |

All items 1-7 COMMITTED. Tier1 smoke gate PENDING (running).

## Priority order (do in this sequence)

### 1. `gpuBindBufferRange` wrapper + alignment asserts  [HIGH]

Replace every raw `glBindBufferRange` call with a helper:

```cpp
// GameOS/gameos/gpu_debug.h (new)
void gpuBindBufferRange(GLenum target, GLuint index, GLuint buffer,
                        GLintptr offset, GLsizeiptr size, const char* tag);
```

Asserts (active in debug + `MC2_RENDER_CONTRACT_ASSERT=1`):
- `buffer != 0`
- `size > 0`
- `target` one of `GL_SHADER_STORAGE_BUFFER` / `GL_UNIFORM_BUFFER` / expected
- `offset % GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT == 0` (for SSBOs)
- `offset % GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT == 0` (for UBOs)

Log form: `[GPU][SSBO/UBO][<tag>] offset=<X> align=<Y> FAIL` → then assert.

Grep targets for callers to migrate:
```
grep -rn "glBindBufferRange" GameOS/ mclib/ code/
```

### 2. GPU buffer zero-init on create  [HIGH]

Every SSBO / indirect / count / range buffer: call `glClearNamedBufferData` or
`glBufferData(target, size, nullptr, usage)` followed immediately by
`glClearBufferData`/`glClearNamedBufferSubData` before first use.

Priority buffers to audit:
- Water lighting/recipe buffers (`renderWaterFastPath`)
- Draw packet / indirect command buffers (`gos_terrain_indirect`)
- Counter buffers (atomic counters for cull)
- Static prop range buffers (`gos_static_prop_batcher`)

Pattern:
```cpp
glNamedBufferData(buf, size, nullptr, GL_DYNAMIC_DRAW);
static const uint32_t kZero = 0;
glClearNamedBufferData(buf, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &kZero);
```

### 3. Render contract assert mode  [HIGH]

Under `MC2_RENDER_CONTRACT_ASSERT=1`, assert at entry of each major pass.

Opaque scene entry:
```cpp
MC2_RENDER_ASSERT(getGLDepthFunc() == GL_GEQUAL);
MC2_RENDER_ASSERT(getGLDepthMask() == GL_TRUE);
MC2_RENDER_ASSERT(!glIsEnabled(GL_BLEND));
MC2_RENDER_ASSERT(currentFBO == s_sceneFBO);
```

Shadow pass entry:
```cpp
MC2_RENDER_ASSERT(getGLDepthFunc() == GL_LESS);
MC2_RENDER_ASSERT(getGLDepthMask() == GL_TRUE);
MC2_RENDER_ASSERT(getGLPolygonOffsetEnabled());
```

These queries are acceptable here (assert mode = dev only, not hot path).
Macro `MC2_RENDER_ASSERT(x)` = no-op unless `MC2_RENDER_CONTRACT_ASSERT=1`.

### 4. Strict shader/VAO diagnostics  [MEDIUM]

**Shader compile/link:**
- On `makeProgram`, log every warning line from `glGetShaderInfoLog` /
  `glGetProgramInfoLog` (currently warnings may be dropped if link succeeds)
- Fail (not just warn) if: required uniform block missing, sampler type
  mismatch, or `gl_` builtin redeclared

**VAO contract:**
- In debug builds, after `glBindVertexArray`, verify:
  - VAO != 0
  - Required attrib locations enabled
  - Stride nonzero
  - Backing buffer nonzero
  - Index buffer bound for indexed draws

### 5. PBO unbind discipline — policy  [MEDIUM]

Already fixed once (reconcile). Enforce as policy:

After every texture upload:
```cpp
glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
```

After every readback:
```cpp
glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
```

Grep for existing raw PBO binds and audit:
```
grep -rn "PIXEL_UNPACK_BUFFER\|PIXEL_PACK_BUFFER" GameOS/ mclib/
```

### 6. Explicit pass-entry state ownership  [MEDIUM]

Hygiene-S1 started this for static props and water. Extend policy:

Every pass must explicitly set before its first draw:
- `GL_DEPTH_TEST` enable/disable + `glDepthFunc` + `glDepthMask`
- `GL_BLEND` enable/disable + `glBlendFunc`
- `GL_CULL_FACE` enable/disable + `glCullFace`
- `GL_COLOR_MASK` if non-default
- FBO bind
- Viewport + scissor
- Polygon offset (if used)

Remaining unguarded passes to audit after S1:
- Bloom / post-process passes
- Mech draw (indirect GPU path)
- Decal draw

### 7. Named memory barriers — no raw `glMemoryBarrier`  [MEDIUM]

Policy: all new code uses `gpuSyncBarrier(tag)` wrapper (already started).
No raw `glMemoryBarrier` in new code.

Existing raw calls: audit and wrap or document why bare call is justified.

```
grep -rn "glMemoryBarrier" GameOS/ mclib/ shaders/
```

### 8. NVIDIA validation smoke checklist  [LOW — pre-release gate]

Not per-commit. Run before releases (or on any NVIDIA hardware available):

```
[ ] default boot — no GL errors on startup
[ ] MC2_TERRAIN_LOD_CHUNK=0 (force dynamic off) — clean render
[ ] water-heavy mission (mc2_24) — no black-water artifacts
[ ] static-prop-heavy mission (mc2_17) — no missing props
[ ] mech preview / weapon test — no geometry corruption
[ ] tacmap click — no crash, pick correct
[ ] visual compare vs baselineA if same-GPU baseline exists
```

Store checklist result as CI artifact when NVIDIA machine is available.

## What S1 already covers (do NOT repeat)

- Static prop: removed 6 glGet* roundtrips, added `gos_InvalidateRenderStateCache()`
- Water: added `invalidateRenderStateCache()` at entry of explicit state block
- TG_Shape (vehicles): `gos_SetRenderState(gos_State_ZWrite, 1)` guard
- Shadow defaults: `MC2_SHADOW_CASTER_LIGHTBOX_CULL` and `MC2_SHADOW_DYNAMIC_PROP_DIRTY_ONLY` default ON

## Gate

Same as S1: tier1 5/5 + visual compare baselineA-head.
Contract assert mode pass: run `MC2_RENDER_CONTRACT_ASSERT=1 ./mc2.exe` on
mc2_01 + mc2_24 with no assertion fires before declaring items 3+4 done.
