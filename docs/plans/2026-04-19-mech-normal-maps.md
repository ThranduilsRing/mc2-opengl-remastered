# Mech Normal Mapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-pixel normal mapping to mech diffuse rendering so armor plates, rivets, and panel gaps catch specular highlights correctly — without the geometry changes that tessellation/displacement would require.

**Architecture:** Tangent-space normal maps, per-chassis `*_N.tga` companion textures loaded via the existing `gosHint_MipmapFilter0` opt-in path from Fix C. Tangents generated at mesh-load time from UV/position derivatives (MikkTSpace-style), stored alongside normals in the MLR vertex buffer, and skinned by the same bone matrices that already skin normals. Shader reads normal map in `gos_tex_vertex.frag` via `USE_NORMAL_MAP` branch, builds TBN from interpolated tangent + normal + handedness, dots sampled-and-renormalized normal against light direction. No changes to the paint-scheme system (normal maps never get paint-edited).

**Tech Stack:** C++, OpenGL 4.2, MLR mesh pipeline (`mclib/mlr/`), GameOS material cache (`gosRenderMaterial`), existing mipmap opt-in flag path (commits `4ddd378` + `2bede1b`).

**Pilot scope:** One chassis (Raven) with a single hand-authored or derived normal map. Validate visually in-game before rolling out to other chassis. If the visual uplift isn't worth the asset-authoring cost, this plan produces a feature flag that can be left off per-chassis.

**Out of scope (separate future plans):**
- Parallax occlusion mapping (POM). Wait until normal mapping is evaluated; POM shares the TBN plumbing so it can build on top of this plan cleanly.
- Specular maps / gloss maps / PBR workflow. Current lighting is Lambertian + ambient; normal mapping enhances that directly. Full PBR is a much larger conversion.
- Normal maps on buildings / ground vehicles / weapon effects. Same plumbing is reusable once validated on mechs.

---

## File Structure

**Modified files:**
- `mclib/mech3d.cpp` — load normal map texture per chassis; bind to second texture unit at draw time.
- `mclib/mech3d.h` — new member `normalTextureHandle` on `Mech3DAppearance` / `Mech3DAppearanceType`.
- `mclib/mlr/<vertex struct header>` — add `float tx, ty, tz, tw` (tangent + handedness) to MLR vertex layout. Exact file is discovery in Task 1.
- `mclib/mlr/<mesh load path>` — compute tangents after position/normal/UV are loaded but before upload. Exact file is discovery in Task 1.
- `mclib/mlr/<mesh submit path>` — add tangent to the VBO attribute list when uploading mesh data.
- `shaders/gos_tex_vertex.vert` — read tangent attribute, pass TBN-relevant outputs to fragment.
- `shaders/gos_tex_vertex.frag` — `USE_NORMAL_MAP` guarded branch that samples `sampler2D normalMap` and computes per-pixel lighting.
- `GameOS/gameos/gameos_graphics.cpp` — wire `gos_State_Texture2` binding for the mech material path (infrastructure already present at line 1896, just not used by mech draws today).

**New files:**
- `mc2srcdata/tgl/128/Raven_N.tga` (pilot chassis only for MVP) — hand-authored OR derived via `pack_mat_normal.py`. NOT committed to git (asset pipeline convention); deployed via loose-file override like the other `*RGB.tga` textures.

---

## Pre-flight

- [ ] **Confirm prior fixes are merged.** This plan assumes commits `9d809e6` (Fix A — dominant-channel paint), `4ddd378` + `2bede1b` (Fix C — mipmap opt-in), and `5727f41` (BGR removal) are all in the working branch. Check with `git log --oneline | head -10`. If any are missing, stop and resolve first — the normal map shader path will fight unpainted mask-boundary pixels from Fix A, will shimmer without Fix C's mipmaps, and will show wrong colors without the BGR fix.

- [ ] **Decide authoring strategy.** Two options:
  1. **Hand-authored normal map** for Raven: download the diffuse, open in Substance/Krita/Photoshop with an nDo-style plugin, paint detail. Higher quality, requires 30-60 min per chassis.
  2. **Derived from diffuse** via `pack_mat_normal.py` in the repo root: height-from-luminance, Sobel-derivative into a normal map. Instant, lower quality but serviceable. **Run it AFTER Fix A is confirmed in-game** so the clean albedo produces clean derived normals without paint-slot-edge ridges.

  Either produces `Raven_N.tga` sized the same as `RavenRGB.tga` (128×128 for stock, 512×512 for the 4x-upscaled path — the loose-file override convention handles both).

---

## Phase 1: Discovery + plumbing (no visual change)

Goal: load a normal map texture per chassis, bind to texture unit 1 at draw time, verify no rendering regression. No shader changes yet.

### Task 1: Map the MLR vertex + mesh-load pipeline

**Files:**
- Read: `mclib/mlr/` (all files) to identify the mech vertex struct, VBO upload path, and where UV/normal/position are finalized per vertex.
- Read: `mclib/msl.cpp` around `LoadTGMultiShapeFromASE` (called from `Mech3DAppearanceType::init`) to find where per-triangle UV and per-vertex position come together.
- Record findings in a short note at the top of this plan document or in a scratch file.

- [ ] **Step 1.1: Identify the MLR vertex struct.**
  Grep: `grep -rn "struct.*[Vv]ertex" mclib/mlr/`. Find the struct that carries position + normal + UV + (bone weight, if present). Note its exact field names and layout. Record path and struct name.

- [ ] **Step 1.2: Identify the tangent injection point.**
  Find where per-vertex data is finalized before VBO upload. Usually a function like `CreateFrom()`, `build()`, or `upload()`. Note the exact function and file:line. This is where `computeTangents()` will be called in Task 3.

- [ ] **Step 1.3: Identify the VBO attribute layout.**
  Find where `glVertexAttribPointer` is called for mech vertex data. Likely in `mclib/mlr/` or `GameOS/gameos/gameos_graphics.cpp`. Note the current attribute indices (0=position, 1=normal, 2=UV, etc.) so the new tangent can take the next free index.

- [ ] **Step 1.4: Identify the mech draw call site.**
  Find where mech triangles get submitted in a form that eventually binds a texture. Likely `TG_Shape::Render` in `mclib/tgl.cpp`, which calls `mcTextureManager->addTriangle` or similar. The second-texture binding needs to go here or in the batch flush.

- [ ] **Step 1.5: Commit discovery notes.**
  ```bash
  git add docs/plans/2026-04-19-mech-normal-maps.md
  git commit -m "docs(plan): record MLR discovery notes for normal-map plan"
  ```

### Task 2: Load and cache the normal map per chassis

**Files:**
- Modify: `mclib/mech3d.h` — add `DWORD normalTextureHandle` to `Mech3DAppearance` (and `Mech3DAppearanceType` if shared).
- Modify: `mclib/mech3d.cpp` — load `<chassisName>_N.tga` alongside the diffuse in `init()` or `resetPaintScheme()`; null-safe if the file doesn't exist.

- [ ] **Step 2.1: Add the handle field.**

  In `mclib/mech3d.h` inside class `Mech3DAppearance` (around where `localTextureHandle` is declared):
  ```cpp
  DWORD normalTextureHandle;  // 0xffffffff if no normal map for this chassis
  ```

  In the Mech3DAppearance constructor, initialize: `normalTextureHandle = 0xffffffff;`

- [ ] **Step 2.2: Load the normal map.**

  In `mclib/mech3d.cpp` inside `Mech3DAppearance::init()` (or wherever `localTextureHandle` is first populated — discovered in Task 1). After the diffuse is loaded, add:

  ```cpp
  // Normal map (optional, opt-in per chassis via presence of <name>_N.tga)
  char normalName[1024];
  mechShape->GetTextureName(0, normalName, 256);
  // Strip extension and append _N.tga
  char* dot = strrchr(normalName, '.');
  if (dot) *dot = '\0';
  strcat(normalName, "_N.tga");

  char texPath[1024];
  sprintf(texPath, "%s%d" PATH_SEPARATOR, tglPath, ObjectTextureSize);
  FullPathFileName normalPath;
  normalPath.init(texPath, normalName, "");

  if (fileExists(normalPath)) {
      const DWORD normalHints = gosHint_MipmapFilter0 | gosHint_DontShrink;
      normalTextureHandle = mcTextureManager->loadTexture(
          normalPath, gos_Texture_Solid, normalHints, 0);
  } else {
      normalTextureHandle = 0xffffffff;
  }
  ```

  Paint scheme does NOT touch the normal map. No changes to `setPaintScheme()` are needed.

- [ ] **Step 2.3: Drop a test normal map into place.**

  Create or copy `Raven_N.tga` to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/tgl/128/Raven_N.tga`. For an initial smoke test, this can even be a flat blue image (128,128,255) which encodes "no displacement, straight up" — rendering should look identical to no normal map. Flat blue = `normalize((0, 0, 1))` in tangent space.

- [ ] **Step 2.4: Build + deploy + verify handle is non-zero.**

  Invoke `/mc2-build` then `/mc2-deploy`. Add a one-off `printf("[NMAP] chassis=%s normalHandle=%u\n", mechName, normalTextureHandle)` in `Mech3DAppearance::init()`, run game with stdout captured, confirm Raven prints a non-`4294967295` handle and other chassis print `4294967295`. Remove the printf.

- [ ] **Step 2.5: Commit.**
  ```bash
  git add mclib/mech3d.h mclib/mech3d.cpp
  git commit -m "feat(mech): load per-chassis normal map via _N.tga convention

  Each Mech3DAppearance now looks for <chassisName>_N.tga alongside the
  diffuse and caches its handle in normalTextureHandle. Missing files
  leave the handle at 0xffffffff (feature off for that chassis).

  Reuses Fix C's gosHint_MipmapFilter0 opt-in so normal maps get mip
  chains + trilinear sampling.

  No rendering change yet -- handle is loaded but not bound."
  ```

### Task 3: Bind the normal map as gos_State_Texture2 for mech draws

**Files:**
- Modify: mech submission/draw site discovered in Task 1.4. Likely somewhere in `mclib/tgl.cpp` or `GameOS/gameos/gameos_graphics.cpp`.

- [ ] **Step 3.1: Add a second-texture binding at the mech draw site.**

  Wherever the mech triangles are submitted with their diffuse handle, ALSO bind `normalTextureHandle` to `gos_State_Texture2`:
  ```cpp
  gos_SetRenderState(gos_State_Texture2,
      normalTextureHandle != 0xffffffff
          ? mcTextureManager->get_gosTextureHandle(normalTextureHandle)
          : 0);
  ```

  If the draw site is inside a static free function in `tgl.cpp` that doesn't see the `Mech3DAppearance`, the handle needs to be threaded through via the texture manager's triangle-add signature, or stored on a per-shape field (`TG_Shape::normalTextureHandle`) populated at `SetTextureHandle` time.

- [ ] **Step 3.2: Verify unit 1 actually gets bound.**

  Temporary debug: in `gameos_graphics.cpp`, in the state-flush loop that applies `gos_State_Texture2`, printf the handle value for a few frames. Confirm it matches Raven's `normalTextureHandle` when a Raven is on screen.

- [ ] **Step 3.3: Build + deploy + sanity check.**

  Game should render identically to before (shader doesn't read unit 1 yet). Any visual regression at this step means the diffuse binding got trampled — re-check `gos_State_Texture` vs `gos_State_Texture2` separation.

- [ ] **Step 3.4: Commit.**
  ```bash
  git add <files>
  git commit -m "feat(mech): bind normal map to gos_State_Texture2

  Reuses the existing gos_State_Texture2 render state (already plumbed
  through gameos_graphics state-flush). Mechs with normalTextureHandle
  == 0xffffffff bind 0 (unbinding), so shader can branch on uniform
  != 0 to decide whether to sample.

  Still no shader-side consumption -- next commit wires the sampling."
  ```

---

## Phase 2: Tangent generation + VBO attribute

Goal: compute per-vertex tangents at mesh load and include them in the VBO. Still no visual change because the shader doesn't read tangent yet.

### Task 4: Compute tangents at mesh-load time

**Files:**
- Modify: MLR vertex struct header (discovered in Task 1.1) — add 4 floats.
- Modify: MLR mesh-load function (discovered in Task 1.2) — call `computeTangents()` after position/normal/UV are loaded.
- Create: `mclib/mlr/tangent.h` + `mclib/mlr/tangent.cpp` — tangent computation.

- [ ] **Step 4.1: Create the tangent computation helper.**

  Create `mclib/mlr/tangent.h`:
  ```cpp
  #ifndef MLR_TANGENT_H
  #define MLR_TANGENT_H

  #include <stuff/stuff.hpp>

  // Computes per-vertex tangent (and handedness sign) from per-triangle
  // position/UV derivatives, accumulated across adjacent triangles and
  // orthonormalized against the vertex normal (Gram-Schmidt).
  //
  // Inputs:
  //   positions[numVerts]  -- per-vertex position
  //   normals[numVerts]    -- per-vertex normal (already unit-length)
  //   uvs[numVerts]        -- per-vertex UV
  //   indices[numTris*3]   -- triangle vertex indices
  //
  // Outputs (caller-allocated):
  //   tangents[numVerts]       -- xyz unit tangent
  //   handedness[numVerts]     -- +1 or -1, bitangent = cross(N,T) * handedness
  void computeTangents(
      const Stuff::Vector3D* positions,
      const Stuff::Vector3D* normals,
      const float* uvs,   // interleaved u,v per vertex
      const unsigned int* indices,
      unsigned int numVerts,
      unsigned int numTris,
      Stuff::Vector3D* outTangents,
      float* outHandedness);

  #endif
  ```

- [ ] **Step 4.2: Implement `computeTangents()`.**

  Create `mclib/mlr/tangent.cpp` with the standard algorithm (Lengyel 2001 / MikkTSpace-simplified):
  ```cpp
  #include "tangent.h"
  #include <vector>
  #include <cmath>

  void computeTangents(
      const Stuff::Vector3D* positions,
      const Stuff::Vector3D* normals,
      const float* uvs,
      const unsigned int* indices,
      unsigned int numVerts,
      unsigned int numTris,
      Stuff::Vector3D* outTangents,
      float* outHandedness)
  {
      std::vector<Stuff::Vector3D> tan1(numVerts, {0,0,0});
      std::vector<Stuff::Vector3D> tan2(numVerts, {0,0,0});

      for (unsigned int t = 0; t < numTris; ++t) {
          unsigned int i0 = indices[t*3 + 0];
          unsigned int i1 = indices[t*3 + 1];
          unsigned int i2 = indices[t*3 + 2];

          const Stuff::Vector3D& p0 = positions[i0];
          const Stuff::Vector3D& p1 = positions[i1];
          const Stuff::Vector3D& p2 = positions[i2];

          float u0 = uvs[i0*2], v0 = uvs[i0*2+1];
          float u1 = uvs[i1*2], v1 = uvs[i1*2+1];
          float u2 = uvs[i2*2], v2 = uvs[i2*2+1];

          float x1 = p1.x - p0.x, x2 = p2.x - p0.x;
          float y1 = p1.y - p0.y, y2 = p2.y - p0.y;
          float z1 = p1.z - p0.z, z2 = p2.z - p0.z;
          float s1 = u1 - u0,     s2 = u2 - u0;
          float t1 = v1 - v0,     t2 = v2 - v0;

          float denom = s1*t2 - s2*t1;
          if (fabsf(denom) < 1e-8f) continue;
          float r = 1.0f / denom;

          Stuff::Vector3D sdir = {
              (t2*x1 - t1*x2) * r,
              (t2*y1 - t1*y2) * r,
              (t2*z1 - t1*z2) * r };
          Stuff::Vector3D tdir = {
              (s1*x2 - s2*x1) * r,
              (s1*y2 - s2*y1) * r,
              (s1*z2 - s2*z1) * r };

          tan1[i0].x += sdir.x; tan1[i0].y += sdir.y; tan1[i0].z += sdir.z;
          tan1[i1].x += sdir.x; tan1[i1].y += sdir.y; tan1[i1].z += sdir.z;
          tan1[i2].x += sdir.x; tan1[i2].y += sdir.y; tan1[i2].z += sdir.z;
          tan2[i0].x += tdir.x; tan2[i0].y += tdir.y; tan2[i0].z += tdir.z;
          tan2[i1].x += tdir.x; tan2[i1].y += tdir.y; tan2[i1].z += tdir.z;
          tan2[i2].x += tdir.x; tan2[i2].y += tdir.y; tan2[i2].z += tdir.z;
      }

      for (unsigned int v = 0; v < numVerts; ++v) {
          const Stuff::Vector3D& n = normals[v];
          Stuff::Vector3D t = tan1[v];

          // Gram-Schmidt orthonormalize: T = normalize(T - N * dot(N, T))
          float ndott = n.x*t.x + n.y*t.y + n.z*t.z;
          Stuff::Vector3D tangent = {
              t.x - n.x*ndott,
              t.y - n.y*ndott,
              t.z - n.z*ndott };
          float len = sqrtf(tangent.x*tangent.x + tangent.y*tangent.y + tangent.z*tangent.z);
          if (len > 1e-8f) {
              tangent.x /= len; tangent.y /= len; tangent.z /= len;
          } else {
              // Degenerate UVs -- pick an arbitrary perpendicular
              tangent = (fabsf(n.x) < 0.9f) ? Stuff::Vector3D{1,0,0}
                                            : Stuff::Vector3D{0,1,0};
          }
          outTangents[v] = tangent;

          // Handedness: sign of dot(cross(N, T), tan2[v])
          Stuff::Vector3D cross = {
              n.y*tangent.z - n.z*tangent.y,
              n.z*tangent.x - n.x*tangent.z,
              n.x*tangent.y - n.y*tangent.x };
          float sign = (cross.x*tan2[v].x + cross.y*tan2[v].y + cross.z*tan2[v].z) < 0.0f
                       ? -1.0f : 1.0f;
          outHandedness[v] = sign;
      }
  }
  ```

- [ ] **Step 4.3: Add tangent fields to the MLR vertex struct.**

  In the struct file discovered in Task 1.1, add next to the existing normal/UV fields:
  ```cpp
  float tangentX, tangentY, tangentZ;   // unit tangent
  float handedness;                     // +1 or -1 for bitangent sign
  ```

  Update the constructor / memset defaults to zero these out.

- [ ] **Step 4.4: Call `computeTangents()` in the mesh-load path.**

  In the function discovered in Task 1.2, after positions/normals/UVs are assigned, call `computeTangents()` and copy the results into the vertex struct's new fields. Exact call will look like:
  ```cpp
  std::vector<Stuff::Vector3D> tangents(numVerts);
  std::vector<float> handedness(numVerts);
  computeTangents(
      positionsArray, normalsArray, uvArray,
      indicesArray, numVerts, numTris,
      tangents.data(), handedness.data());
  for (unsigned int v = 0; v < numVerts; ++v) {
      vertices[v].tangentX = tangents[v].x;
      vertices[v].tangentY = tangents[v].y;
      vertices[v].tangentZ = tangents[v].z;
      vertices[v].handedness = handedness[v];
  }
  ```

- [ ] **Step 4.5: Build + verify no crash.**

  Game should still render mechs identically. Tangents are computed and stored but nothing reads them yet. If the build fails on an unrelated `Stuff::Vector3D` aggregate-initializer syntax, use `Stuff::Vector3D x; x.x=...; x.y=...; x.z=...;` instead.

- [ ] **Step 4.6: Commit.**
  ```bash
  git add mclib/mlr/tangent.h mclib/mlr/tangent.cpp <vertex struct file> <mesh load file>
  git commit -m "feat(mlr): compute per-vertex tangents at mesh load

  Adds tangentX/Y/Z + handedness to the MLR vertex struct and
  computes them from UV/position derivatives using the standard
  Lengyel algorithm with Gram-Schmidt orthonormalization against
  the existing per-vertex normal. Degenerate UVs fall back to an
  arbitrary perpendicular.

  Still no visual change -- these fields are populated but not yet
  included in the VBO attribute list or read by any shader."
  ```

### Task 5: Wire the tangent into the VBO

**Files:**
- Modify: file from Task 1.3 that contains the `glVertexAttribPointer` setup for mech vertices.

- [ ] **Step 5.1: Add the tangent attribute.**

  Wherever the existing `glVertexAttribPointer` calls configure position (loc 0), normal (loc 1), UV (loc 2) for mech vertices, add:
  ```cpp
  glEnableVertexAttribArray(TANGENT_LOC);  // next free location, e.g. 3
  glVertexAttribPointer(TANGENT_LOC, 4, GL_FLOAT, GL_FALSE,
                        sizeof(MLRVertex),
                        (void*)offsetof(MLRVertex, tangentX));
  ```

  Replace `MLRVertex` with the actual struct name from Task 1.1. Handedness is packed into `.w` of the tangent attribute (hence 4 floats, not 3).

- [ ] **Step 5.2: Update the vertex shader to declare the attribute.**

  In `shaders/gos_tex_vertex.vert`, add:
  ```glsl
  layout(location = 3) in vec4 aTangent;  // xyz = tangent, w = handedness
  ```

  Replace `3` with whatever `TANGENT_LOC` ended up being.

  Don't USE it yet — just declare so the driver doesn't complain about a dangling attribute pointer. Pass through to a varying for Task 6:
  ```glsl
  out vec4 vTangent;
  // ... in main():
  vTangent = aTangent;
  ```

- [ ] **Step 5.3: Build + deploy + verify.**

  Mechs should still render identically. Any corruption here means the stride/offset is wrong. Check with RenderDoc / AMD RGP that the tangent attribute is actually reaching the shader with sensible values (unit length, roughly perpendicular to the normal).

- [ ] **Step 5.4: Commit.**
  ```bash
  git add <files>
  git commit -m "feat(mech): pass tangent vec4 through to vertex shader

  Location 3 (or next free) attribute carries tangent.xyz + handedness
  as .w. Vertex shader declares and forwards to varying vTangent for
  the fragment shader to consume once normal-map sampling lands."
  ```

---

## Phase 3: Shader normal-map sampling

Goal: actual visual change. Fragment shader samples the normal map, builds TBN, lights per-pixel.

### Task 6: Normal-map branch in the mech shaders

**Files:**
- Modify: `shaders/gos_tex_vertex.vert` — compute world-space normal + tangent, pass TBN basis.
- Modify: `shaders/gos_tex_vertex.frag` — `USE_NORMAL_MAP` branch that samples `normalMap`, renormalizes, does per-pixel lighting.

- [ ] **Step 6.1: Read the current vertex shader's lighting path.**

  Open `shaders/gos_tex_vertex.vert`. Note what it currently outputs: likely interpolated vertex color (Lambert done per-vertex) or a world-space normal. Note how it handles the MLR CPU-skinned normal (is it already skinned by the time it arrives? almost certainly yes — CPU MLR does skin).

- [ ] **Step 6.2: Output world-space normal + tangent + bitangent from vertex shader.**

  In `shaders/gos_tex_vertex.vert`, add outputs:
  ```glsl
  out vec3 vWorldNormal;
  out vec3 vWorldTangent;
  out vec3 vWorldBitangent;
  ```

  In main, after the existing normal computation:
  ```glsl
  vWorldNormal    = normalize(aNormal);          // MLR has already skinned
  vWorldTangent   = normalize(aTangent.xyz);
  vWorldBitangent = normalize(cross(vWorldNormal, vWorldTangent) * aTangent.w);
  ```

  If `aNormal` is in view space instead of world space in this shader, use whatever matrix is already being applied (check the existing position transform). The TBN basis must match the coordinate space the light direction is in.

- [ ] **Step 6.3: Fragment shader — declare normalMap sampler + branch.**

  In `shaders/gos_tex_vertex.frag`, add at top:
  ```glsl
  layout(binding = 1) uniform sampler2D normalMap;
  uniform int useNormalMap;  // 0 or 1

  in vec3 vWorldNormal;
  in vec3 vWorldTangent;
  in vec3 vWorldBitangent;
  ```

  Replace the existing normal used for lighting with:
  ```glsl
  vec3 N = normalize(vWorldNormal);
  if (useNormalMap != 0) {
      vec3 nm = texture(normalMap, vUV).xyz * 2.0 - 1.0;  // decode [-1,1]
      mat3 TBN = mat3(
          normalize(vWorldTangent),
          normalize(vWorldBitangent),
          N);
      N = normalize(TBN * nm);
  }
  // N is now per-pixel normal, use wherever the old vertex-interpolated
  // normal was used (dot with light direction, etc.)
  ```

- [ ] **Step 6.4: Set the `useNormalMap` uniform + sampler binding from the draw-call site.**

  In the CPU-side code that sets up the mech shader's uniforms (same place that currently sets other mech uniforms like lighting params), add:
  ```cpp
  int useNormalMap = (normalTextureHandle != 0xffffffff) ? 1 : 0;
  shader->setInt("useNormalMap", useNormalMap);
  // binding=1 is set via layout qualifier in GLSL; just ensure
  // gos_State_Texture2 maps to GL_TEXTURE1.
  ```

  Verify `gos_State_Texture2` binds to `GL_TEXTURE1` (not `GL_TEXTURE2`) — check `gameos_graphics.cpp` around line 2051 where the tex_states array maps GOS states to GL texture units.

- [ ] **Step 6.5: Build + deploy + FIRST VISUAL CHECK.**

  Drop a flat-blue normal map (`(128, 128, 255, 255)` RGBA, encoding "no perturbation") at `data/tgl/128/Raven_N.tga`. Game should render Raven identically to before (flat blue decodes to `(0,0,1)` in tangent space = plain vertex normal).

  If Raven looks WRONG with a flat blue normal map, the TBN construction is wrong. Common mistakes: normal not in world space, tangent not in world space, handedness sign flipped, sampler binding to wrong texture unit.

- [ ] **Step 6.6: Replace flat-blue with a real normal map.**

  Either hand-author or run `pack_mat_normal.py data/tgl/128/RavenRGB.tga data/tgl/128/Raven_N.tga`. Copy to the deploy path. Launch game. Orbit camera around a Raven. Armor panel lines should now catch specular highlights that weren't there before.

- [ ] **Step 6.7: Verify at zoom and under animation.**

  - Zoom all the way out: mech should still look clean (mipmaps handle this via Fix C).
  - Rotate torso / move arms: bumps should NOT shimmer or shear. If they do, the tangent is not being skinned correctly — go back and verify `aTangent` is transformed by the same bone matrix as `aNormal` in the MLR CPU skin path. This is the single most likely failure mode.

- [ ] **Step 6.8: Commit.**
  ```bash
  git add shaders/gos_tex_vertex.vert shaders/gos_tex_vertex.frag <CPU uniform setup>
  git commit -m "feat(mech): tangent-space normal mapping in fragment shader

  USE_NORMAL_MAP branch samples a per-chassis normal map bound to
  gos_State_Texture2 and does per-pixel lighting using a TBN basis
  built from the new per-vertex tangent + handedness (Phase 2).

  Falls back cleanly to the existing vertex-normal Lambertian path
  when normalTextureHandle is 0xffffffff (no map for this chassis).

  Pilot chassis: Raven. Rollout to other chassis is asset-authoring
  only; no further code changes needed."
  ```

---

## Phase 4: Validate + decide on rollout

### Task 7: In-game evaluation

- [ ] **Step 7.1: Side-by-side comparison.**

  Launch a mission with both a Raven (has normal map) and a Bushwacker (no normal map). At several zoom levels, screenshot both. Compare:
  - Raven should have visibly more detail in armor plates, rivets, panel gaps.
  - Bushwacker should look identical to pre-plan state.

- [ ] **Step 7.2: Motion test.**

  Watch a Raven walk, turn its torso, and fire. Normal map should track the geometry smoothly. No shimmer, no warping at joint rotations.

- [ ] **Step 7.3: Decide on rollout.**

  Honest question: is the visual uplift worth authoring normal maps for the other ~30 chassis?
  - If **yes**: open a separate asset-production ticket. Code is done; just needs `*_N.tga` files in `mc2srcdata/tgl/128/` for each chassis.
  - If **no**: leave Raven as the only chassis with a normal map. All others continue to work (their `normalTextureHandle == 0xffffffff` keeps the feature disabled per-chassis). Consider this plan complete.
  - If **partial**: pick the 5-10 most-visible chassis (Mad Cat, Thor, Raven, Bushwacker, Starslayer, etc.) and author for those only.

### Task 8: Memory + CLAUDE.md update

- [ ] **Step 8.1: Update the memory note.**

  Append to `~/.claude/projects/A--Games-mc2-opengl-src/memory/mech_paint_and_mipmap_system.md` a new section describing the normal-map opt-in: per-chassis `_N.tga` convention, `gos_State_Texture2` binding, tangent attribute location, and the `useNormalMap` uniform.

- [ ] **Step 8.2: Update `CLAUDE.md` Key Files.**

  Add `mclib/mlr/tangent.cpp` to the Key Files list with a one-line description ("per-vertex tangent generation for normal mapping").

- [ ] **Step 8.3: Commit.**
  ```bash
  git add CLAUDE.md
  git commit -m "docs: note normal-map support and tangent module"
  ```

---

## Risks + mitigations

- **MLR CPU-skinning doesn't currently re-skin tangent.** If Phase 2 lands but tangents aren't transformed by bone matrices during skinning, bumps will shear on any animated joint. Mitigation: inspect the MLR skin function discovered in Task 1 and make sure whatever transforms normal also transforms tangent. If it's hard to reach, a workaround is to move the skin to the GPU (pass bone matrix as uniform, skin in vertex shader) — that's a larger change and would justify its own plan.

- **Tangent attribute location collision.** The MLR vertex shader might already use attribute location 3 for something (bone weight, vertex color). Task 5 must pick a genuinely free location — verify with `glGetProgramResource` or a RenderDoc capture.

- **Normal map compression artifacts.** TGA is uncompressed so fine, but if someone later converts to DXT5, be aware: standard DXT5 degrades normal-map quality significantly. Use BC5 (RG only, reconstruct Z) if you ever switch away from TGA.

- **Paint scheme interaction.** Fix A gives clean albedos, but the paint scheme runs on the DIFFUSE only. Normal map stays unpainted — this is correct. If a future session modifies `setPaintScheme()` to touch more than slot 0, it must explicitly skip `normalTextureHandle`.

- **Lighting direction space mismatch.** The existing fragment shader's light direction might be in view space, world space, or object space. Task 6.2 computes TBN in world space; the light direction must be in the same space. Verify by inspecting whatever uniform holds the light direction and how it's computed CPU-side.

---

## Estimated effort

- **Phase 1 (plumbing):** half a day, mostly discovery.
- **Phase 2 (tangent gen + VBO):** half to full day, algorithm is textbook but integration depends on how clean the MLR code is.
- **Phase 3 (shader):** half a day if the skin-aware tangent turns out to work, up to a full day if it needs the skin path updated.
- **Phase 4 (eval + memory):** an hour.

**Total: 2–3 focused sessions** for a single-chassis MVP. Rollout to other chassis is asset-authoring only, not code.
