# Visual Bug-Fix Batch — 2026-04-24 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the highest-priority visual and harness bugs from `docs/observations/2026-04-24-tier2-visual-observations.md`.

**Architecture:** Three tiers. Tier 1 (Safe Now) is the execution target: two C++ bug fixes plus one manifest line. Tier 2 (Needs Revision) is experimental shader tuning — land these only after screenshot comparison, one at a time, not bundled with the C++ fixes. Tier 3 (Deferred) needs more artifacts.

**Tech Stack:** C++17 (MSVC), GLSL shaders (hot-reload via deploy), Python 3 smoke harness.

---

## Key Paths
- **Worktree source:** `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`
- **Deploy:** `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`
- **Build command:** `cmake --build build64 --config RelWithDebInfo`
- **Deploy skill:** `/mc2-deploy` (or `.claude/skills/mc2-deploy.md`)

---

## Tier 1 — Safe Now (execute in order)

### Task 1: Vehicle paint regression fix

**Root cause:** `GVAppearance::setPaintScheme()` in `mclib/gvactor.cpp` uses the OLD strict-zero classifier: a pixel is only team-coloured if its two non-dominant channels are exactly zero (`if (!baseColorGreen && !baseColorBlue)`). For upscaled vehicle textures, bilinear interpolation produces near-zero but non-zero values, so every pixel fails the check and stays at its raw texture colour. Vehicle paint masks are typically the blue slot, so every vehicle shows raw blue. The fix is already proven in `mech3d.cpp` (the dominant-channel classifier, lines 1639–1730) — port it verbatim to `gvactor.cpp`.

There is also a pre-existing loop bug in the old code: the inner `for` loop iterates `j < textureData.Height` instead of `j < textureData.Width`. This means non-square textures are walked incorrectly. Fix this while replacing the classifier.

**Files:**
- Modify: `mclib/gvactor.cpp` (the no-arg `setPaintScheme` function body, lines ~1130–1231)

- [ ] **Step 1: Read both paint functions**

Read `mclib/gvactor.cpp` lines 1130–1231 and `mclib/mech3d.cpp` lines 1635–1735 side by side to confirm the two loops differ only in classifier logic and the Width/Height bug.

- [ ] **Step 2: Replace the pixel loop in GVAppearance::setPaintScheme(void)**

Replace everything from `DWORD *textureMemory = textureData.pTexture;` to (but not including) `gos_UnLockTexture` with the new loop. The surrounding `gos_LockTexture` / `gos_UnLockTexture` frame stays.

```cpp
		DWORD *textureMemory = textureData.pTexture;
		for (long i = 0; i < textureData.Height; i++)
		{
			for (long j = 0; j < textureData.Width; j++)   // was textureData.Height — bug fixed
			{
				DWORD srcColor = *textureMemory;
				DWORD srcA = (srcColor & 0xff000000);
				int   srcR = (int)((srcColor >> 16) & 0xff);
				int   srcG = (int)((srcColor >>  8) & 0xff);
				int   srcB = (int)( srcColor        & 0xff);

				int   slot      = -1;
				DWORD slotColor = 0;
				float shade     = 0.0f;
				float mix_      = 0.0f;
				const int   kMinDom      = 16;
				const float kRatio       = 3.0f;
				const float kRatioMargin = 1.5f;

				int domChan = 0, maxOther = 0;
				if (srcR >= kMinDom && (float)srcR >= kRatio * (float)srcG && (float)srcR >= kRatio * (float)srcB)
				{
					slot = 0; slotColor = psRed;
					domChan = srcR; maxOther = srcG > srcB ? srcG : srcB;
				}
				else if (srcG >= kMinDom && (float)srcG >= kRatio * (float)srcR && (float)srcG >= kRatio * (float)srcB)
				{
					slot = 1; slotColor = psGreen;
					domChan = srcG; maxOther = srcR > srcB ? srcR : srcB;
				}
				else if (srcB >= kMinDom && (float)srcB >= kRatio * (float)srcR && (float)srcB >= kRatio * (float)srcG)
				{
					slot = 2; slotColor = psBlue;
					domChan = srcB; maxOther = srcR > srcG ? srcR : srcG;
				}

				if (slot != -1)
				{
					float d = (float)domChan / 255.0f;
					shade = 1.0f - (1.0f - d) * (1.0f - d);

					float ratio  = (float)domChan / (float)(maxOther + 1);
					float margin = (ratio - kRatio) / (kRatio * (kRatioMargin - 1.0f));
					if (margin > 1.0f) margin = 1.0f;
					if (margin < 0.0f) margin = 0.0f;
					mix_ = margin;
				}

				DWORD newColor = srcColor;
				if (slot != -1 && mix_ > 0.0f)
				{
					float tintR = (float)((slotColor >> 16) & 0xff) * shade;
					float tintG = (float)((slotColor >>  8) & 0xff) * shade;
					float tintB = (float)( slotColor        & 0xff) * shade;

					float outR = tintR * mix_ + (float)srcR * (1.0f - mix_);
					float outG = tintG * mix_ + (float)srcG * (1.0f - mix_);
					float outB = tintB * mix_ + (float)srcB * (1.0f - mix_);

					unsigned char r = (unsigned char)(outR < 0.0f ? 0.0f : outR > 255.0f ? 255.0f : outR);
					unsigned char g = (unsigned char)(outG < 0.0f ? 0.0f : outG > 255.0f ? 255.0f : outG);
					unsigned char b = (unsigned char)(outB < 0.0f ? 0.0f : outB > 255.0f ? 255.0f : outB);

					newColor = srcA | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
				}

				*textureMemory = newColor;
				textureMemory++;
			}
		}
```

- [ ] **Step 3: Build**

```bash
cmake --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | tail -20
```
Expected: `Build succeeded.` 0 errors. If `LNK1201`, close any running mc2.exe first.

- [ ] **Step 4: Commit before deploy**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add mclib/gvactor.cpp
git commit -m "$(cat <<'EOF'
fix: apply dominant-channel paint classifier to GVAppearance

Port the proven mech3d.cpp dominant-channel classifier to
GVAppearance::setPaintScheme(). The old strict-zero check
(!baseColorGreen && !baseColorBlue) missed every upscaled paint-mask
pixel, leaving all vehicles showing their raw blue channel.

Also fix a pre-existing loop bug: inner loop iterated j < Height
instead of j < Width, walking non-square textures out of bounds.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 5: Deploy exe, verify in game**

Close any running mc2.exe, then:
```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

Launch a mission with multiple team factions. Vehicles should now show their faction paint colours instead of uniform blue.

---

### Task 2: ABL runtime error rate-limiter

**Root cause:** `ablFatalCallback` in `code/ablmc2.cpp` calls `STOP(s)` on every invocation. In RelWithDebInfo builds, `STOP` logs and continues (does not abort). Mission `mc2_20` triggers an unimplemented ABL function (`corewait`) on every script tick, producing ~3400 log lines/sec. The smoke harness captured a 135 MB log in 60 seconds and the runner's walltime cap was effectively bypassed. Fix: add a fixed-size, no-heap C table in `ablFatalCallback` that suppresses duplicate error messages after 5 prints. No STL, no heap allocation — a 16-slot static C array of (hash, count) pairs.

**Files:**
- Modify: `code/ablmc2.cpp` (function `ablFatalCallback`, around line 7627)

- [ ] **Step 1: Locate ablFatalCallback**

Find the function — it should be a single-line `STOP((s))` wrapper.

- [ ] **Step 2: Replace with rate-limited version**

Replace:
```cpp
void ablFatalCallback (long code, const char* s) {

	STOP((s));
}
```

With:
```cpp
void ablFatalCallback (long code, const char* s) {
	// Rate-limit repeated identical errors. Fixed-size C table — no STL, no heap.
	// Tracks up to kSlots unique message fingerprints per session.
	static const int kSlots     = 16;
	static const int kMaxPrints = 5;
	struct AblMsgSlot { unsigned int hash; int count; };
	static AblMsgSlot slots[kSlots];
	static int slotCount = 0;
	static bool tableFull = false;

	// Hash first 64 chars of message (djb2-lite, collisions acceptable here)
	unsigned int h = 5381u;
	const char* p = s ? s : "";
	for (int i = 0; *p && i < 64; ++p, ++i)
		h = h * 33u ^ (unsigned char)*p;

	for (int i = 0; i < slotCount; ++i) {
		if (slots[i].hash == h) {
			if (++slots[i].count <= kMaxPrints)
				STOP((s));
			else if (slots[i].count == kMaxPrints + 1)
				STOP(("[ABL] suppressing further identical errors (hash %08x): %s", h, s));
			return;
		}
	}

	if (slotCount < kSlots) {
		slots[slotCount++] = { h, 1 };
		STOP((s));
	} else if (!tableFull) {
		tableFull = true;
		STOP(("[ABL] fatal callback: %d unique errors tracked; further distinct messages suppressed", kSlots));
	}
}
```

- [ ] **Step 3: Build**

```bash
cmake --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" --config RelWithDebInfo 2>&1 | tail -20
```
Expected: 0 errors.

- [ ] **Step 4: Deploy exe + commit**

Close any running mc2.exe:
```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add code/ablmc2.cpp
git commit -m "$(cat <<'EOF'
fix: rate-limit ABL fatal callback spam (max 5 prints per unique message)

Fixed-size 16-slot static C table — no STL, no heap. mc2_20 was
emitting ~3400 identical "Unimplemented feature" log lines/sec,
producing 135 MB logs and defeating the smoke runner walltime cap.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: Liao cutscene smoke duration override

**Observation:** The Liao escort/ambush mission has a cutscene that extends past the default 60s smoke duration, causing the mission to time out before gameplay begins. Fix: add a `duration=80` override in `smoke_missions.txt`.

**Files:**
- Modify: `tests/smoke/smoke_missions.txt`

- [ ] **Step 1: Identify the Liao stem**

```bash
ls "A:/Games/mc2-opengl/mc2-win64-v0.1.1/data/missions/" | grep -i "liao\|escort\|ambush"
```

If that doesn't reveal it, check the tier2 run artifacts:
```bash
ls "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/tests/smoke/artifacts/" | sort | tail -5
```
Then inspect the most recent tier2 artifact directory for a log entry whose mission name contains "liao" or "escort".

- [ ] **Step 2: Add duration override**

Once the stem is known (e.g., `mc2_16`), find its line in `smoke_missions.txt`:
```
tier2 mc2_16 reason="campaign"
```
Change it to:
```
tier2 mc2_16 duration=80 reason="campaign — Liao cutscene; +20s for cutscene to play through"
```

- [ ] **Step 3: Verify manifest parses**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
python -c "
from scripts.smoke_lib import manifest
m = manifest.load('tests/smoke/smoke_missions.txt')
hits = [e for e in m if e.duration and e.duration != 60]
print(hits)
"
```
Expected: prints the modified entry with `duration=80`.

- [ ] **Step 4: Commit**

```bash
git add tests/smoke/smoke_missions.txt
git commit -m "$(cat <<'EOF'
fix: add 80s duration override for Liao escort mission smoke entry

Default 60s is not enough for the opening cutscene to clear.
+20s gives gameplay time without extending the full tier2 matrix.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Tier1 regression gate

After all three tasks above are committed and deployed:

- [ ] **Run tier1 gate**

```bash
py -3 "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/scripts/run_smoke.py" \
   --tier tier1 --with-menu-canary --kill-existing
```
Expected: exit 0. Inspect `tests/smoke/artifacts/<timestamp>/` on failure.

---

## Tier 2 — Visual Tuning Experiments (after tier1 gate passes)

These are visual tuning changes. NOT bundled with the C++ fixes above. Each must be validated with before/after screenshots on multiple biomes. Land as a separate "visual tuning" commit, one experiment at a time.

**Status after deep-dive:** Experiments B and C are deployed to `mc2-win64-v0.1.1` for visual inspection. Experiment A has complete code but is NOT deployed — too risky to ship without feedback.

---

### Experiment A: Terrain tile seam expansion (code-ready, NOT yet deployed)

**Root cause (confirmed):** The original D3D7 MC2 renderer had intentional 1–2px vertex position overlap between adjacent tiles to prevent rasterization coverage gaps. The OpenGL port never had this — the initial commit `305ad87` introduced no overlap. Adjacent patches share the same `Vertex*` pointers from the terrain vertex pool, so their `worldPos` values at shared edges are bitwise identical. Two triangles meeting at an exact edge can leave a sub-pixel strip uncovered by OpenGL's rasterization rules (coverage gap, not a z-fight). This manifests as a seam line, even on concrete (tessLevel=1, no Phong, no displacement) confirming it is not a tessellation artefact.

**Files:** `shaders/gos_terrain.tese`

**Risk:** Z-fighting or texture swim if expansion is too large, or if interior tessellated points are also expanded. The edge mask below prevents interior expansion.

**How to apply:**

In `gos_terrain.tese`, after `vec3 bary = gl_TessCoord;` and barycentric interpolation of worldPos (~line 40), and AFTER the Phong and displacement blocks (~lines 73–94), add this block immediately before `WorldNorm = worldNorm; WorldPos = worldPos;`:

```glsl
    // Restore D3D7-era tile-edge overlap to close rasterization coverage seams.
    // Expand edge/corner vertices outward from the patch centroid. The edgeMask
    // gates this to bary-edge vertices only (min(bary) ≈ 0) so interior tessellated
    // points are not disturbed. At tessLevel=1 (concrete) every vertex is an edge
    // vertex — full expansion applies, matching the original D3D7 behaviour.
    {
        float edgeDist = min(min(bary.x, bary.y), bary.z);
        float edgeMask = 1.0 - smoothstep(0.0, 0.08, edgeDist);
        if (edgeMask > 0.001) {
            vec2 patchCentXY = (tcs_WorldPos[0].xy + tcs_WorldPos[1].xy + tcs_WorldPos[2].xy) / 3.0;
            vec2 seamDir = worldPos.xy - patchCentXY;
            float seamLen = length(seamDir);
            if (seamLen > 0.01)
                worldPos.xy += (seamDir / seamLen) * 1.5 * edgeMask;
        }
    }
```

**Validation checklist before committing:**
- [ ] Concrete aprons (tessLevel=1): seams gone, no new stripe artefacts
- [ ] Dirt terrain (tessLevel > 1): seams gone, no z-fighting at tile edges
- [ ] Rock at Wolfman zoom: no edge bulging or texture swim
- [ ] Grass: no visible inflation at tile boundaries

---

### Experiment B: Terrain grey-lift — luminance-adaptive tint strength (DEPLOYED)

**Root cause:** `tintStrength = mix(0.45, 0.85, snowWeight)` — the 0.45 floor pulls every fragment 45% toward the material tint colour (rock: `vec3(0.36, 0.37, 0.40)`, which is mid-grey). On dark/night colormaps (fragment luminance ≈ 0.05), a 45% blend with mid-grey lifts the output to a visible grey instead of near-black. The fix adapts tintStrength to the pixel's own luminance so dark pixels get far less tint pull.

**File:** `shaders/gos_terrain.frag`, line 410.

**Change applied:**

```glsl
// BEFORE:
PREC float tintStrength = mix(0.45, 0.85, snowWeight);

// AFTER (deployed):
PREC float colLum = dot(texColor.rgb, kLumaWeights);
PREC float tintBase = mix(0.18, 0.50, smoothstep(0.1, 0.6, colLum));
PREC float tintStrength = mix(tintBase, 0.85, snowWeight);
```

**What this does:**
- `colLum ≈ 0.05` (black/night map): `tintBase ≈ 0.18` → only 18% pulled toward material tint → output stays near-black ✓
- `colLum ≈ 0.35` (typical mid-tone terrain): `tintBase ≈ 0.34` → similar to old 0.45 for well-lit tiles
- `colLum ≈ 0.6+` (bright desert/sand): `tintBase → 0.50` → bright areas get full colour blend ✓
- Snow (`snowWeight → 1`): `tintStrength → 0.85` regardless — snow pop is unchanged ✓

**Validation checklist before committing:**
- [ ] Night/black map: terrain reads as dark/black, not grey
- [ ] Desert mission (mc2_01): tone not washed out
- [ ] Grass/dirt reference: dirt still "looks fantastic"
- [ ] Snow: sparkle + cool white still present

---

### Experiment C: Rock/grass normalBoost reduction (DEPLOYED)

**Root cause:** `const PREC vec4 normalBoost = vec4(1.3, 1.5, 1.1, 2.5)` — rock (x=1.3) and grass (y=1.5) are over-boosted relative to dirt (z=1.1, the reference "looks fantastic" material). At RTS zoom the overshoot creates a sharp high-frequency grain pattern that reads as noise rather than surface texture. The fix brings rock and grass down to or just above dirt's reference level.

**File:** `shaders/gos_terrain.frag`, line 340.

**Change applied:**

```glsl
// BEFORE:
const PREC vec4 normalBoost = vec4(1.3, 1.5, 1.1, 2.5);

// AFTER (deployed):
const PREC vec4 normalBoost = vec4(0.9, 1.1, 1.1, 2.5);
//  rock:     1.3 → 0.9  (below dirt — risk: may read flat; increase to 1.0 if so)
//  grass:    1.5 → 1.1  (matches dirt — the "feels right" reference)
//  dirt:     1.1 → 1.1  (UNCHANGED — this is the reference; leave it alone)
//  concrete: 2.5 → 2.5  (UNCHANGED — flat concrete benefits from strong normal detail)
```

**Validation checklist before committing:**
- [ ] Rock biome: grain/noise reduced, surface still reads as textured (not plastic-flat)
- [ ] Grass: subtle detail visible, not buzzy
- [ ] Dirt (mc2_01): unchanged — "looks fantastic" still applies
- [ ] Concrete apron: still reads as flat/hard (concrete boost unchanged)
- [ ] If rock reads flat: bump x from 0.9 → 1.0 before committing

---

## Tier 3 — Full Root-Cause Analysis + Implementation Specs

These are confirmed or suspected root causes from the deep-dive. Each has enough detail to implement; they are deferred because they need visual validation artifacts or carry more risk.

---

### Tier 3A: Trees have no dynamic shadows

**Root cause (confirmed by code trace):**

In `mclib/tgl.cpp` line 2725–2730:
```cpp
const bool eligibleForDynamicShadow =
    !isSpotlight && !isWindow && !isHudElement && !isClamped &&
    !theShape->alphaTestOn && (alphaValue == 0xff) && shapeToWorld;
if (eligibleForDynamicShadow)
    addShadowShape(theShape->vb_, theShape->ib_, theShape->vdecl_, shapeToWorld->entries);
```

In `mclib/bdactor.cpp` lines 3129, 3152:
```cpp
treeShape[i]->SetAlphaTest(true);  // all LODs
treeShape[0]->SetAlphaTest(true);
```

Trees have `alphaTestOn = true` because their textures use alpha cutouts for foliage. This disqualifies them from `addShadowShape`. Additionally, all generic static props (in `genactor.cpp` line 1159) call `genShape->SetIsClamped(true)`, which also blocks `eligibleForDynamicShadow` via `!isClamped`.

**Two separate blockers:**
1. `alphaTestOn = true` → tree foliage shapes never enter shadow collection
2. `isClamped = true` (all generic props) → static prop buildings never enter shadow collection

**What already works:** Buildings (non-clamped? verify) and mechs/vehicles cast shadows correctly because they pass the `eligibleForDynamicShadow` check.

**Fix design for alpha-test shadows:**

The current `shadow_object.frag` is depth-only with no alpha sampling:
```glsl
void main() { gl_FragDepth = gl_FragCoord.z; }
```

For alpha-test shapes, we need a second shadow shader that discards based on the texture alpha:

**New file: `shaders/shadow_object_alphatest.vert`**
```glsl
//#version 420 (provided by material prefix)
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 texcoord;  // UV channel — must be location 1 in VBO

uniform mat4 lightSpaceMatrix;
uniform mat4 worldMatrix;
uniform vec3 lightOffset;

out vec2 vTexcoord;

void main() {
    vec4 stuffPos = worldMatrix * vec4(position, 1.0);
    vec3 mc2Pos = vec3(-stuffPos.x, stuffPos.z, stuffPos.y) + lightOffset;
    gl_Position = lightSpaceMatrix * vec4(mc2Pos, 1.0);
    vTexcoord = texcoord;
}
```

**New file: `shaders/shadow_object_alphatest.frag`**
```glsl
//#version 420 (provided by material prefix)
uniform sampler2D tex0;  // diffuse with alpha channel

in vec2 vTexcoord;

void main() {
    float alpha = texture(tex0, vTexcoord).a;
    if (alpha < 0.5) discard;
    gl_FragDepth = gl_FragCoord.z;
}
```

**C++ changes required:**

1. `mclib/txmmgr.h` — add texture handle to `ShadowShapeEntry`:
```cpp
struct ShadowShapeEntry {
    HGOSBUFFER vb;
    HGOSBUFFER ib;
    HGOSVERTEXDECLARATION vdecl;
    float worldEntries[16];
    bool alphaTest;          // new
    DWORD gosTextureHandle;  // new — 0xffffffff if not alpha-test
};
```

2. `mclib/txmmgr.cpp` — update `addShadowShape` signature:
```cpp
void addShadowShape(HGOSBUFFER vb, HGOSBUFFER ib, HGOSVERTEXDECLARATION vdecl,
                    const float* worldEntries16, bool alphaTest = false,
                    DWORD gosTexHandle = 0xffffffff);
```

3. `mclib/tgl.cpp` — relax the shadow eligibility check to allow alpha-test shapes from static props:
```cpp
// Change: !theShape->alphaTestOn → allow alpha-test shapes with a texture
const bool eligibleForDynamicShadow =
    !isSpotlight && !isWindow && !isHudElement && !isClamped &&
    (alphaValue == 0xff) && shapeToWorld;  // removed !alphaTestOn
if (eligibleForDynamicShadow) {
    DWORD texHandle = 0xffffffff;
    if (theShape->alphaTestOn && theShape->numTextures > 0)
        texHandle = theShape->listOfTextures[0].gosTextureHandle;
    addShadowShape(theShape->vb_, theShape->ib_, theShape->vdecl_,
                   shapeToWorld->entries, theShape->alphaTestOn, texHandle);
}
```

4. `GameOS/gameos/gos_postprocess.cpp` — in the shadow object draw loop, branch on `alphaTest`:
```cpp
for (int i = 0; i < g_numShadowShapes; i++) {
    ShadowShapeEntry& ss = g_shadowShapes[i];
    GLuint prog = ss.alphaTest ? s_shadowObjectAlphaTestProgram : s_shadowObjectProgram;
    glUseProgram(prog);
    // ... bind UBOs, draw ...
    if (ss.alphaTest && ss.gosTextureHandle != 0xffffffff) {
        glActiveTexture(GL_TEXTURE0);
        gos_GetTextureGL(ss.gosTextureHandle, ...);  // bind diffuse
        glUniform1i(glGetUniformLocation(prog, "tex0"), 0);
    }
}
```

**⚠ Before implementing:** Verify `isClamped` semantics for trees vs buildings. Trees go through `bdactor.cpp::render()` → `treeShape->Render()` → `TG_MultiShape::Render()` without calling `SetIsClamped`. So `isClamped = false` for trees. The `alphaTestOn` blocker is the only issue for trees. The `isClamped` blocker is for generic props only.

**Deploy + validate:**
- Trees in mc2_01 should cast alpha-clipped shadows on the terrain
- Verify no GPU feedback loop (shadow map reading while writing — bind shadow FBO before sampling depth)

---

### Tier 3B: Power generator animated mesh renders below terrain

**Root cause (suspected, needs verification):**

In `mclib/bdactor.cpp` line 1564–1571:
```cpp
if (appearType->spinMe)
    bldgShape->Render(false, 0.00001f);   // spinning → force near z
else if (!depthFixup)
    bldgShape->Render();                   // normal depth
else if (depthFixup > 0)
    bldgShape->Render(false, 0.9999999f); // push to back
else if (depthFixup < 0)
    bldgShape->Render(false, 0.00001f);   // force near z
```

The power generator is a building. Its base elevation is set via:
```cpp
xlatPosition.y = land->getTerrainElevation(position);  // bdactor.cpp:474
```

The terrain visual surface is displaced by both:
1. **Tessellation Phong smoothing** — changes the Z of the visual surface vs the mathematical surface
2. **Per-material displacement** (dirt only) — additional Z offset via `worldPos += worldNorm * (disp - 0.5) * displaceScale`

The terrain writes depth via:
```glsl
// gos_terrain.frag:567
gl_FragDepth = clamp(max(UndisplacedDepth, gl_FragCoord.z) + 0.0005, 0.0, 1.0);
```

`UndisplacedDepth` is the clip-space depth of the pre-displacement terrain surface. The `max()` ensures the written depth is always at or deeper than the undisplaced surface. **This means terrain occupies screen-space depth in the range `[rasterized_displaced_z, undisplaced_z + 0.0005]`.**

A building placed at `getTerrainElevation()` (the undisplaced CPU height field) renders at approximately `undisplaced_z`. The terrain depth-writes `undisplaced_z + 0.0005` — meaning the terrain fragment's depth value is *slightly deeper* than the building vertex's screen-space depth. With `GL_LEQUAL`, the terrain wins → building is clipped by terrain → appears to be "below the terrain."

**Diagnosis steps to confirm:**

1. Enable `MC2_WALL_SHADOW_TRACE` (or add a new env var `MC2_BLDG_Z_TRACE=1`) to log the building's world Y and the terrain elevation at that position for the power generator object type.

2. Compare the building's clip-space Z to the terrain's `UndisplacedDepth` on the same pixel.

**Proposed fix options:**

Option A — Reduce terrain depth bias:
```glsl
// gos_terrain.frag:567 — reduce from 0.0005 to 0.0001
gl_FragDepth = clamp(max(UndisplacedDepth, gl_FragCoord.z) + 0.0001, 0.0, 1.0);
```
Risk: might reintroduce overlay depth issues the 0.0005 was added to prevent.

Option B — Buildings get a small upward position bias:
In `mclib/bdactor.cpp`, after `xlatPosition.y = land->getTerrainElevation(position)`, add a bias:
```cpp
xlatPosition.y += 2.0f;  // lift above terrain depth window
```
Risk: buildings appear to float slightly. May need per-building-type tuning.

Option C — Animated buildings with `spinMe` already use `forceZ = 0.00001f`. If the power generator was classified as `spinMe` it would render in front of everything. But this conflicts with normal depth ordering for buildings on slopes.

**Recommended:** First enable trace logging to confirm the hypothesis, then apply Option A with regression test on overlay depth.

---

### Tier 3C: Textureless mission + missing overlay mission (needs stem identification)

**Root cause (unknown — needs stem):**

The tier2 run produced two mission-specific failures:
1. A mission after the moon mission has no terrain textures
2. A mission with dark green + brownish rock has no overlay texture

**Investigation steps:**

```bash
# Find the tier2 artifacts from the most recent run
ls "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/tests/smoke/artifacts/" | sort | tail -3

# Check which missions had unusual log content
grep -l "texture\|missing\|null\|0xffff" \
  "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/tests/smoke/artifacts/<latest>/*.log"
```

Cross-reference with the FST/loose-file resolve order:
- Loose files in `data/art/` override FST — if an upscaled 4x version of a terrain texture was omitted from the override set, the fallback FST read could return a wrong path.
- Check `AssetScale::dumpCountersTo(stdout)` in the log for `unknown_asset` events on the failing missions.

---

### Tier 3D: GPU load saturation on early-campaign missions

**Observation:** GPU ~100% on first few missions, normalises on moon map. Spikes again after moon.

**Investigation approach:**

1. Connect Tracy GUI during a tier2 run: `tracy-profiler.exe` → connect to localhost.
2. Capture a flame chart for mc2_01 vs mc2_14 (moon) and compare:
   - Shadow.StaticAccum zone width
   - GPU grass geometry shader (if enabled via RAlt+5)
   - 3D object draw zone
3. If static shadow accumulation is frequent on early missions (terrain newly revealed every 100 units), the camera movement pattern could explain it.
4. Use AMD RGP via Radeon Developer Panel for shader-level occupancy comparison between an early mission and the moon map.

**Most likely suspects:** More static props + denser terrain vertex pool on lush early biomes vs sparse moon. Moon has fewer geometry shader invocations (grass disabled by biome? or fewer grass-classified pixels).

---

### Tier 3E: Smoke harness hardening (defensive improvements)

Two harness-side observations from the mc2_20 run:

**1. ABL VM should rate-limit or abort on first "Unimplemented feature" (DONE — Task 2 above)**

**2. Stdout pressure can defeat the walltime cap:**

The runner's `queue.get(timeout=0.1)` loop processes one line per iteration. At millions of lines/sec, the `time.monotonic()` cap check runs only once per 0.1s of processing, not wall-clock time. The cap check IS before `queue.get` (already correct in the current code), but the queue fill rate can exceed drain rate, making the queue unboundedly large before any cap check fires.

**Defensive fix in `scripts/smoke_lib/runner.py`:**

Find the stdout-drain loop and add a secondary wall-clock check inside the inner loop:

```python
# In run_one(), the drain loop — add a wall-clock guard INSIDE the queue.get loop:
DRAIN_WALLTIME_HEADROOM_S = 10  # abort drain if we're this many seconds past cap
while True:
    elapsed = time.monotonic() - start_wall
    if elapsed > cap + DRAIN_WALLTIME_HEADROOM_S:
        result_lines.append("[runner] hard walltime abort: stdout drain exceeded cap + headroom")
        proc.kill()
        break
    try:
        line = q.get(timeout=0.1)
    except queue.Empty:
        if proc.poll() is not None:
            break
        continue
    result_lines.append(line)
```

This caps the total drain time regardless of stdout pressure.
