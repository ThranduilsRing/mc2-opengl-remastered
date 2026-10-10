<!-- doc-status: current | reviewed:2026-10-11 -->
# VFX 3D Mesh GPU Substrate Recon

**Slice:** VFX-3D-MESH-PARTICLE-SUBSTRATE-RECON-1  
**Date:** 2026-05-31  
**Branch:** `claude/vfx-3d-mesh-substrate-recon-1` (off `claude/nifty-mendeleev` tip `f77346da`)  
**Status:** Read-only analysis. No code changes.

---

## Background

Three gosFX classes render 3D mesh geometry instead of billboard quads:

| Class | ClassID | Spec count (mc2.fx) | Hierarchy | Visual role |
|---|---|---|---|---|
| ShapeCloud | 1319 | ~9 (prev. analysis) | SpinningCloud | N copies of one mesh; per-particle scale/rotation/color |
| Shape | 1323 | ~10 | Singleton | Single mesh instance; scale/color over lifetime |
| DebrisCloud | 1325 | ~34 | Effect (unique) | N different meshes; rigid-body explosion debris |

These are the only gosFX classes still invisible in the GPU-driven OpenGL port.

---

## Critical finding: DrawScalableShape is gated OFF by default

All three classes call `info->m_clipper->DrawScalableShape(&dinfo)` in their `Draw()` method.
`MLRClipper::DrawScalableShape()` begins with:

```cpp
MC2_GOSFX_GATE_EARLY_RETURN();
```

`mlr_gate.cpp` hardcodes `kDefaultDisabled = true`. Unless `MC2_DISABLE_GOSFX=0` is set,
`DrawScalableShape` returns immediately without rendering.

**Consequence:** ShapeCloud, Shape, and DebrisCloud are completely invisible in default stock
play. The legacy gosFX draw path for these three classes is already dead.

**Implication for oracle design:** Gate-OFF for any new GPU oracle path is byte-identical
by construction — we're adding rendering where there is currently none.

---

## Class anatomy

### Shape (ClassID 1323)

Inherits from `Singleton → Effect`. Single effect instance (not a cloud of particles).

**Spec data:**
- `MLRShape* m_shape` — one mesh embedded in spec binary
- `m_radius` — bounding sphere radius (precomputed from mesh vertices)
- Alignment flags: `m_alignZUsingX`, `m_alignZUsingY` (camera-facing modes)
- Curve fields from Singleton: `m_pRed/Green/Blue/Alpha`, `m_scale` (size over lifetime)

**Live state (`Draw()`):**
- `m_localToParent` (LinearMatrix4D, from Singleton) — current world-space transform
- `m_scale`, `m_color` (Singleton fields) — updated by `Animate()` each frame

**Draw call:**
```cpp
dinfo.shape = m_shape;
dinfo.shapeToWorld = &local_to_world;   // m_localToParent * parentToWorld
dinfo.scaling = &scale;                  // uniform: (m_scale, m_scale, m_scale)
dinfo.paintMe = &m_color;               // RGBA (from spec alpha curve)
info->m_clipper->DrawScalableShape(&dinfo);
```

**GPU oracle complexity:** **LOW**. One draw call per active effect. CPU provides full
transform every frame; mesh never changes.

---

### ShapeCloud (ClassID 1319)

Inherits from `SpinningCloud → ParticleCloud → Effect`. Cloud of up to N particles, each
a transformed copy of the same mesh.

**Spec data:**
- `MLRShape* m_shape` — one shared mesh for all particles
- Per-spec: alignment flags, `m_pRed/Green/Blue/Alpha`, `m_pScale`, `m_pRadius` (size)
- Particle size: `m_particleClassSize = sizeof(ShapeCloud::Particle)`

**Per-particle live state:**
```cpp
struct ShapeCloud__Particle : SpinningCloud__Particle {
    // from SpinningCloud__Particle:
    Stuff::Point3D          m_localTranslation;
    Stuff::UnitQuaternion   m_localRotation;
    Stuff::Scalar           m_scale;        // uniform scale
    Stuff::Scalar           m_radius;       // bounding sphere
    Stuff::Scalar           m_age, m_ageRate, m_seed;
    // from ShapeCloud__Particle:
    Stuff::RGBAColor        m_color;        // CPU curve-evaluated RGBA
};
```

**Draw call pattern:**
```cpp
for (unsigned i = 0; i < m_activeParticleCount; i++) {
    // build shape_to_world from particle->m_localTranslation + m_localRotation
    // build scale = (particle->m_scale, particle->m_scale, particle->m_scale)
    dinfo.shapeToWorld = &shape_to_world;
    dinfo.scaling      = &scale;
    dinfo.paintMe      = &particle->m_color;
    info->m_clipper->DrawScalableShape(&dinfo);
}
```

**GPU oracle complexity:** **MEDIUM**. One mesh shared by all particles, but N draw calls
per frame. Instanced rendering (`glDrawArraysInstanced`) + per-particle SSBO is the natural
GPU approach. Matches the billboard particle pattern closely but uses mesh vertices.

---

### DebrisCloud (ClassID 1325)

Inherits directly from `Effect` (NOT SpinningCloud). Unique architecture.

**Spec data:**
- `DynamicArrayOf<MLRShape*> debrisPieces` — N **different** meshes, one per debris piece
- `DynamicArrayOf<Point3D>   debrisPositions` — initial positions
- `DynamicArrayOf<Sphere>    debrisSpheres` — bounding spheres per piece
- Physics curves: drag, acceleration, spin, ether velocity (gravity simulation)

**Per-piece live state:**
```cpp
struct DebrisCloud__Particle {
    Stuff::LinearMatrix4D m_localToParent;  // full 4×4 transform (position + rotation)
    Stuff::Scalar         m_age, m_ageRate, m_seed;
    Stuff::Vector3D       m_angularVelocity;
    Stuff::Vector3D       m_linearVelocity;
    Stuff::Scalar         m_alpha;          // scalar fade (no per-piece color)
};
```

**Physics:** CPU-simulated every frame (gravity, drag, angular velocity). Particles fly out
from an explosion center and tumble.

**Draw call pattern:**
```cpp
for each alive piece i:
    dinfo.shape      = spec->debrisPieces[i];    // DIFFERENT mesh per piece
    dinfo.shapeToWorld = &shape_to_world_i;       // from particle.m_localToParent
    dinfo.scaling    = NULL;                       // no scale — mesh is pre-sized
    dinfo.paintMe    = &color(1,1,1, alpha_i);    // white + scalar alpha only
    info->m_clipper->DrawScalableShape(&dinfo);
```

**GPU oracle complexity:** **HIGH**. Multiple different meshes — can't use simple instancing.
Separate VAO per debris piece per spec. But debris count per spec is small (typically 3-20
pieces). Full physics stays on CPU; only transforms change per frame.

---

## MLRShape → GL extraction API

The extraction path is **not yet implemented** in the codebase. No utility exists in
`GameOS/` or `mclib/` to convert `MLRShape` to GL buffers. Static props and mechs have
their own tightly-coupled mesh upload paths (in `setupTextures`/txmmgr and `mech3d.cpp`
respectively).

**Available MLRShape API (confirmed in headers):**

```cpp
// MLRShape — container of primitives
int             GetNum();           // number of MLRPrimitiveBase* in this shape
MLRPrimitiveBase* Find(int index);  // access primitive i

// MLRPrimitiveBase — single draw-call mesh slice
void GetCoordData(Stuff::Point3D**, int* count);    // vertex positions (3×float)
void GetTexCoordData(Vector2DScalar**, int* count);  // UV coords (2×float)
// GetNormData: virtual, available on lit variants (MLR_I_L_TMesh, etc.)
```

**MLR primitive subclass landscape:**
- `MLR_I_TMesh` — indexed triangle mesh (most common in gosFX; "I"=indexed, "T"=triangle)
- `MLR_I_PMesh` — indexed polygon mesh (quads or larger polys)
- `MLR_I_L_TMesh` — lit indexed triangle mesh (has normals)
- `MLR_I_DT_TMesh` — dual-textured indexed triangle mesh
- `MLR_I_C_TMesh` — colored indexed triangle mesh (per-vertex colors in data)
- Many combinations of the above

GosFX mesh specs likely use unlit or colored variants (no GI/shadowing in particle effects).
Index data is available via `GetIndexData()` on `MLRPrimitive` derivatives.

**Proposed utility structure:**

```
GameOS/gameos/gos_vfx_mesh_bridge.h / .cpp   (new)
  struct GpuMeshPrimitive { GLuint vao, vbo, ebo; int indexCount; }
  struct GpuMeshShape { vector<GpuMeshPrimitive> primitives; }
  GpuMeshShape* extractShape(MLRShape*);      // extract + upload to GL, cache by pointer
  void releaseShape(MLRShape*);               // remove from cache on effect destroy
```

Extraction happens lazily on first `Draw()` call after spec activation.

---

## Per-class GPU oracle design

### Shape oracle (simplest)

```
Shape::Draw() oracle path:
  1. Get/create GpuMeshShape for spec->m_shape
  2. Build shapeToWorld = m_localToParent * parentToWorld
  3. Upload uniforms: u_shapeToWorld (mat4), u_scale (float), u_color (vec4)
  4. For each primitive: glBindVertexArray + glDrawElements
```

Single-draw per effect. Gate: `MC2_VFX_ORACLE_RENDER=1`.

### ShapeCloud oracle (instanced)

```
ShapeCloud::Draw() oracle path:
  1. Get/create GpuMeshShape for spec->m_shape
  2. Build per-particle SSBO: { shapeToWorld (mat4), scale (float), color (vec4) }
  3. For each primitive: glBindVertexArray + glDrawElementsInstanced(N particles)
```

One instanced draw per primitive per frame. Gate: `MC2_VFX_ORACLE_RENDER=1`.

### DebrisCloud oracle (per-piece draw)

```
DebrisCloud::Draw() oracle path:
  1. Get/create GpuMeshShape for each spec->debrisPieces[i]
  2. For each alive piece i:
     - Upload: u_shapeToWorld = particle.m_localToParent * localToWorld (mat4)
     - Upload: u_color = (1,1,1, particle.m_alpha)
     - For each primitive: glBindVertexArray + glDrawElements
```

N draw calls per active effect. Gate: `MC2_VFX_ORACLE_RENDER=1`.

---

## Shader needs

New shader pair: `vfx_mesh.vert` / `vfx_mesh.frag`

```glsl
// vfx_mesh.vert
layout(location=0) in vec3 a_position;
layout(location=1) in vec2 a_uv;        // optional, not all MLR prims have UVs
layout(location=2) in vec4 a_color;     // optional, for colored MLR prims

uniform mat4  u_worldToClipGL;
uniform mat4  u_shapeToWorld;   // set per draw call
uniform float u_scale;          // uniform scale
uniform vec4  u_paintColor;     // RGBA tint from gosFX (dinfo.paintMe)

out vec2 v_uv;
out vec4 v_color;

// ShapeCloud instanced variant:
// layout(std430, binding=15) readonly buffer Instances { mat4 instance_shapeToWorld[]; float instance_scale[]; vec4 instance_color[]; }
```

```glsl
// vfx_mesh.frag — simple: tex * color, discard if alpha < 0.01
```

This shader is simpler than `static_prop.vert` (no PBR, no shadow maps, no array textures).
Reuse of `particle_billboard.frag` is possible for the fragment stage if UV + colorkey behavior matches.

---

## Texture handling

GosFX mesh specs have an `m_state` (MLRState) which includes texture references. The
`MLRState` has a texture handle accessible via the existing `gos_GetGLTextureName()` path
used by the billboard bridge. The same texture binding pattern applies.

---

## Comparison to billboard oracle arc

| Dimension | Billboard (existing) | 3D Mesh (proposed) |
|---|---|---|
| Mesh geometry source | None — quad generated in shader | MLRShape embedded in gosFX spec |
| Vertex data | gl_VertexID-driven (no VBO) | VBO extracted from MLRShape primitives |
| Per-particle data | GpuParticle SSBO (64B, binding=14) | SSBO (transform + alpha) or per-draw uniforms |
| Shader | particle_billboard.vert/frag (existing) | vfx_mesh.vert/frag (new) |
| Texture | Per-group handle, gos_GetGLTextureName | Per-spec from m_state.GetTexture() |
| Instance count | Up to 4096 (batcher budget) | ShapeCloud: ≤ m_maxParticleCount; DebrisCloud: ≤ 20 |
| Gate | MC2_VFX_ORACLE_RENDER | same |
| Gate-OFF state | byte-identical to prior (billboards were routed) | byte-identical to prior (DrawScalableShape was already NOP) |

---

## Implementation scope estimate

| Component | Files | Estimated lines | Dependency |
|---|---|---|---|
| GpuMeshCache (extractor) | `GameOS/gameos/gos_vfx_mesh_bridge.h/.cpp` | ~300 | MLRShape API |
| vfx_mesh.vert | `shaders/vfx_mesh.vert` | ~60 | new shader |
| vfx_mesh.frag | `shaders/vfx_mesh.frag` | ~40 | new shader |
| Shape oracle | `mclib/gosfx/shape.cpp` | ~80 | GpuMeshCache |
| ShapeCloud oracle | `mclib/gosfx/shapecloud.cpp` | ~120 | GpuMeshCache |
| DebrisCloud oracle | `mclib/gosfx/debriscloud.cpp` | ~150 | GpuMeshCache |
| Bridge (GL setup) | `GameOS/gameos/gos_vfx_mesh_bridge.cpp` | ~150 (draw path) | GpuMeshCache + shaders |
| Shader reflect goldens | `tools/shader_reflect/expected/` | ~2 new JSON files | reflect tool |
| **Total** | **~10 files** | **~900 lines** | |

Compare: billboard arc (5 classes) took ~600 lines across ~15 files spread over 8 commits.

---

## Sequencing recommendation

The arc divides cleanly into 3 slices:

1. **VFX-3D-MESH-GPU-SUBSTRATE-1** (blocker for all): `GpuMeshCache` + `vfx_mesh.vert/frag` + bridge setup. Pure GL substrate, no oracle calls yet. Gate-OFF byte-identical.

2. **VFX-SHAPE-ORACLE-1**: Shape + ShapeCloud oracle paths using the substrate. Shape first (simplest: single instance, no instancing machinery needed). ShapeCloud second (adds instanced draw).

3. **VFX-DEBRISCLOUD-ORACLE-1**: DebrisCloud oracle. More complex (per-piece mesh heterogeneity) but highest visual payoff (explosion debris is very visible in combat).

Order within slice 2 is: Shape → ShapeCloud (dependency: Shape validates the non-instanced draw path before adding per-particle SSBO machinery for ShapeCloud).

---

## Open questions (requiring interactive session or spec inspection)

1. **Which MLR primitive subtypes are actually used in mc2.fx?** Need runtime inspection or spec parser to confirm. If specs use only `MLR_I_TMesh` (no lit variants, no dual texture), extraction is simpler (no normal buffer needed).

2. **Do ShapeCloud specs in mc2.fx ever have camera-alignment enabled?** The three alignment modes (`alignZUsingX`, `alignZUsingY`) add CPU-side matrix math. Alignment needs to be preserved in the oracle.

3. **DebrisCloud active in any tier1 mission?** If DebrisCloud specs are only triggered by specific destruction events (building/mech explosion), they won't appear in passive smoke. An interactive combat session is needed to validate.

4. **ShapeCloud particle count range?** With 9 specs and ~maxParticleCount each, understanding typical counts guides whether instancing buffer pre-allocation is needed.

---

## Files read for this recon

- `mclib/gosfx/shape.cpp` + `shape.hpp`
- `mclib/gosfx/shapecloud.cpp` + `shapecloud.hpp`
- `mclib/gosfx/debriscloud.cpp` + `debriscloud.hpp`
- `mclib/mlr/mlrclipper.cpp` — DrawScalableShape implementation
- `mclib/mlr/mlr_gate.h` + `mlr_gate.cpp` — gate default (kDefaultDisabled=true)
- `mclib/mlr/mlrshape.hpp` — GetNum()/Find() API
- `mclib/mlr/mlrprimitivebase.hpp` + `mlrprimitive.hpp` — GetCoordData/GetTexCoordData API
