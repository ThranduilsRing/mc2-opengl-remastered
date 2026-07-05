# SKYBOX-OOB-FOG-RECON-1

RECON ONLY. No source changes made. Worktree: `A:/Games/mc2-controlmap-sample-1` (branch `claude/controlmap-sample-1`, pre-existing unrelated dirty terrain-controlmap work — untouched).

## 1. OOB fog (`shaders/fog_oob.frag`)

`fog_oob.frag:53-54`:
```glsl
float rawDepth = texture(depthTex, TexCoord).r;
if (rawDepth > 0.0001) { outFog = vec4(0.0); return; }
```
Engine is reverse-Z with `glClearDepth(0.0f)` (`GameOS/gameos/gos_postprocess.cpp:3999`, `:4029`, `:4409`, `:4458`, `:4551` all restore scene default `glClearDepth(0.0f)`). So `rawDepth ~ 0` = "far/unwritten", which this shader treats as void/sky-candidate. The ONLY gate keeping fog off real sky is the ray-direction cutoff at line 62: `if (worldDir.z < -0.22) { discard; }`, fading in from `smoothstep(-0.22, -0.01, worldDir.z)` (line 65) — i.e. fog is allowed starting ~22% of the way from horizon toward zenith and ramps to full opacity by ~1% below horizon. This is a fixed screen-space-independent world-ray threshold, not a depth/stencil discriminator — it doesn't know where the skybox quad was actually drawn, it just guesses "close to horizon = fog band, allow a chunk of visible sky above the true horizon too."

Root cause of "floats up": **fog_oob keys on depth==far/unwritten, and the sky draw never writes depth, so sky pixels are indistinguishable from true off-map-void pixels in `sceneDepthTex_`.** The shader's only defense is the world-space `worldDir.z` band, which is deliberately wide (line 61-65 comment: "Fade in from just-above-horizon... fill the full void") so it reads as clouds bleeding upward into the sky rather than stopping exactly at the horizon.

`runFogOob()` (`GameOS/gameos/gos_postprocess.cpp:2544-2623`) binds `sceneFBO_`, samples `sceneDepthTex_` (`:2613`), and is a straight fullscreen quad draw (`glDrawArrays(GL_TRIANGLES,0,6)` at `:2616`) with no scissor rect set anywhere in the function or its callers (`grep -n scissor gos_postprocess.cpp` hits only an unrelated letterbox-clear helper at `:3029-3047`, nothing near FogOob/EdgeFog). So yes — same "unscissored fullscreen pass" shape as the flicker follow-up, but that's a symptom of how it's invoked, not the reason it climbs above the horizon; the climb is the depth-value collision + wide world-space band.

## 2. Edge fog (`shaders/edge_fog.frag`)

Same structural exposure but smaller practical impact: `edge_fog.frag:47-51` treats void pixels (`rawDepth < 0.0001`) as sitting at `u_fogHeight` (world Z, default 50 per header comment) for the height fade, then intersects the camera ray with that height plane for XY (`:62-69`) — it does NOT branch on "is this the sky" at all, it only cares about world-space geometry Z / ray-plane intersection. Practically this is bounded by `u_fogHeight`/`heightFade` (`:59-60`) so it self-limits to a modest world-Z band near the map edge rather than climbing indefinitely — lower risk of visibly hiding the skybox than `fog_oob`, but it shares the same "sky pixels look like void pixels" ambiguity at the depth-sample level (line 35, 48).

## 3. Skybox draw order / depth state

Sky is drawn **first**, before terrain, at `code/gamecam.cpp:450-465` ("GameCamera::render sky", `ZoneScopedN`), via `GameAdapters::Sky::renderHdri()` → `SkyRenderAdapter.cpp:23-30` → `gosPostProcess::renderHdriSkybox()` (`gos_postprocess.cpp:3306`). That function explicitly disables depth at `:3368-3369`:
```cpp
glDepthMask(GL_FALSE);
glDisable(GL_DEPTH_TEST);
```
(legacy `renderSkybox()` at `:3282-3283` does the identical thing). So **sky never writes depth** — confirmed root cause. `runFogOob`/`runEdgeFog` run later as post-process passes over the composited `sceneFBO_` (wired via `runFogRegionGL()` at `:2634-2642`, called from the executor/endScene sites), sampling `sceneDepthTex_` which still holds the reverse-Z clear value (0) wherever only the depth-less sky quad drew.

## 4. Smallest fix + Vulkan interplay

Two independently-composable options, GL-side only (do NOT touch `GameOS/gameos/vulkan_postprocess_subgraph.cpp` — foreign in-flight Vulkan WIP per instructions):

- **(a) Sky/void discriminator.** Cheapest: write a sentinel into an existing unused channel (e.g. stencil, or the already-present ObjectID attachment gated by `MC2_OBJECT_ID_BUFFER`, or a dedicated 1-bit "is-sky" mask) when the sky quad draws, and have `fog_oob.frag`/`edge_fog.frag` sample it to hard-exclude true sky pixels regardless of `worldDir.z`. Requires a real write path since sky currently writes nothing (color-only, depth off) — likely the least invasive is a stencil write during the sky draw (`glStencilFunc`/`glStencilOp` around `renderHdriSkybox`/`renderSkybox`) then `glStencilFunc(GL_NOTEQUAL, ...)` in the fog passes' pipeline state, OR promote sky to write depth exactly at far (trivial with reverse-Z: enable depth write only, `GL_ALWAYS` test, output frag depth = 0.0) which the fog shaders already read — this reuses the existing `rawDepth` sample with no new binding, just needs sky's depth write turned back on at the clear value it already sits at (no visible depth-state change for anything else, since sky already sits at the clear value implicitly).
- **(b) Horizon clamp tightened in world-ray space.** Narrow/reshape the `worldDir.z < -0.22` / `smoothstep(-0.22,-0.01,...)` band in `fog_oob.frag:62,65` so it only ever reads at-or-below the true geometric horizon for the current camera, rather than a fixed generous cutoff. Cheaper (shader-only, no new GPU state) but heuristic — still can't perfectly separate "real sky pixel" from "void pixel" since it never queries where geometry actually is; it only reduces the visible overlap band. Does not fully solve at grazing/low camera angles where the true horizon can sit anywhere in the `[-0.22,-0.01]` z-range depending on FOV/pitch.
- **(c) Scissor.** Orthogonal — scissoring the fullscreen quad to the sub-horizon screen region (once a horizon line is known) would help perf/flicker but doesn't itself fix the misclassification; it's the same fix as (b) applied as a scissor rect instead of a per-pixel shader branch, so it's a variant of (b) not a substitute for (a).

Recommendation: **(a)** is the correct fix (removes the ambiguity at its source) but touches sky-draw GL state (adds a depth or stencil write) — needs a byte-identity-OFF gate since it changes the render contract for the sky pass, not just the fog pass. **(b)** is the safe minimal patch if a same-day mitigation is wanted without touching the sky draw, at the cost of being a heuristic tightening rather than a real fix.

**Vulkan interplay:** `GameOS/gameos/vulkan_postprocess_subgraph.cpp` (module header `:2,:21-22,:27`) fuses EdgeFog+OobFog into one native Vulkan render pass, 2 pipelines, 2 draws, with UBO params "pulled EXACTLY from GL runEdgeFog()/runFogOob()" (`:27,:910-931`) and loads `shaders/vulkan/fog_oob.frag` / `shaders/vulkan/edge_fog.frag` (separate GLSL sources under `shaders/vulkan/`, not the same file as `shaders/fog_oob.frag`/`shaders/edge_fog.frag` — currently believed logically identical per the module's "parity-proven" framing but are physically duplicate files). **Any shader-logic fix (option b, or the fragment-shader half of option a) must be ported to BOTH `shaders/fog_oob.frag`+`shaders/edge_fog.frag` (GL) AND `shaders/vulkan/fog_oob.frag`+`shaders/vulkan/edge_fog.frag` (Vulkan island), or the two paths silently diverge and break the existing pixel-parity proof.** Per task constraint, no edits designed against `vulkan_postprocess_subgraph.cpp` itself here — only the shared `.frag` shader text would need mirroring, and the Vulkan island's parity-proof harness (mentioned in MEMORY as pixel-parity-proven for EdgeFog/OOB-fog) would need re-running after any shader change on either side.

## 5. Gate + acceptance (design only, not implemented)

- New gate suggestion: `MC2_OOB_FOG_SKY_EXCLUDE` (default OFF) wrapping option (a)/(b), consistent with existing `MC2_OOB_FOG`/`MC2_EDGE_FOG` default-ON convention for the passes themselves — this sub-behavior should default OFF until proven, then flip.
- Acceptance: static-camera screenshot at a gameplay angle with visible sky above terrain horizon (e.g. low-pitch camera looking toward map edge where OOB fog is active) — with gate OFF, capture shows fog visibly overlapping sky region (current bug, byte-identical to today); with gate ON, sky region above the true horizon is clear of fog coloring/opacity while the OOB cloud band at/below horizon is unchanged. Use existing tier1 smoke static-cam capture tooling (`mc2-render-state` `run_capture_baseline`/`summarize_latest_capture`) rather than a new harness.

## Open rulings (need a decision before coding)

1. Is a sky depth/stencil write (option a) acceptable given `renderSkybox`/`renderHdriSkybox` currently explicitly disable both (`:3282-3283`, `:3368-3369`) — any downstream code relying on "sky never touches depth/stencil" needs an audit before flipping this.
2. Which shader owns the fix — reshaping `fog_oob.frag`'s existing `worldDir.z` band (b) is lower-risk/no-new-state but is a heuristic, not a real fix; needs a call on which risk profile is preferred for this slice.
3. Vulkan shader mirroring is mandatory once a GLSL-side fix lands (see section 4) — should be scoped as a same-slice follow-up, not deferred, to avoid parity drift between `shaders/*.frag` and `shaders/vulkan/*.frag`.
