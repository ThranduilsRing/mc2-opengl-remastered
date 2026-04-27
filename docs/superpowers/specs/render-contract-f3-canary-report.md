# Render Contract F3 — AMD Canary Report

**Status:** ⏳ **PENDING OBSERVATION** — setup complete, awaiting operator verdict.
**Date:** 2026-04-26 (setup); observation TBD
**Branch:** `claude/f3-amd-canary-temp` (throwaway; do not merge)
**Built and deployed from:** `claude/f3-amd-canary-temp` HEAD `0173a31`

This report is committed in placeholder form by the F3 design v2 sequence. The "Verdict" section below is intentionally empty until the operator runs the test and observes the result. The morning-review handoff (§3) is the operator's first action when picking this work back up.

---

## 1. What was set up

### 1.1 Branch

`git branch claude/f3-amd-canary-temp` was created from `claude/nifty-mendeleev` at HEAD `628694a`. One commit on it (`0173a31`) modifies a single shader.

### 1.2 Shader modification

[`shaders/gos_tex_vertex_lighted.frag`](../../../shaders/gos_tex_vertex_lighted.frag) — the PRIMARY Group II-Opaque path per [pass audit §3.3](render-contract-f3-pass-audit.md). Every mech, building, and vehicle pixel passes through this shader.

Added:

```glsl
#include <include/render_contract.hglsl>
...
layout (location=1) out PREC vec4 GBuffer1;
...
GBuffer1 = rc_gbuffer1_screenShadowEligible(normalize(Normal));
```

This declares attachment 1, includes the registry helper header, and writes the explicit post-shadow-eligible value with the real per-vertex world normal. If the AMD RX 7900 corruption claim at `gos_postprocess.cpp:519-520` is real, this is the exact pattern that was warned against.

### 1.3 Build + deploy

```
cmake --build build64 --config RelWithDebInfo --target mc2  → green
cp -f build64/RelWithDebInfo/mc2.exe                A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe
cp -f shaders/gos_tex_vertex_lighted.frag           A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_tex_vertex_lighted.frag
diff -q (both shader files)                          → identical
```

`shaders/include/render_contract.hglsl` was already deployed from the registry phase 1 work; no additional include deploy needed.

### 1.4 Source-tree state on the F3 branch

The F3 branch (`claude/nifty-mendeleev`) does **not** carry the canary modification. The worktree's `gos_tex_vertex_lighted.frag` is back to the unmodified Group-II-Opaque baseline. The deployed file at `A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/` is the canary version.

**Caution.** If the operator runs `/mc2-build-deploy` from the F3 branch before observing the canary, the deployed `gos_tex_vertex_lighted.frag` will be overwritten with the unmodified baseline and the canary state will be lost. To preserve the canary deploy until observation, **do not run mc2-build-deploy from `claude/nifty-mendeleev` until the canary verdict is recorded.** Re-deploy from `claude/f3-amd-canary-temp` if needed.

---

## 2. Observation protocol (the morning-review action)

### 2.1 Preflight

Verify the deploy is in canary state:

```bash
grep "F3 canary" A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_tex_vertex_lighted.frag
```

Expect 3 matches (header comment, declaration comment, write-site comment). If zero matches, the canary deploy was overwritten — re-deploy from `claude/f3-amd-canary-temp` HEAD before observing.

### 2.2 Run two missions

Two tier1 missions chosen for distinct content profile:

1. **Mech-heavy mission.** Recommended: `mc2_10` or `mc2_17`. Heavy TGL world-object content; if `gos_tex_vertex_lighted.frag` corrupts on AMD, it will be visible across most pixels.
2. **Infrastructure mission.** Recommended: `mc2_03` or `mc2_24`. Buildings + vehicles + mechs together; tests overdraw interaction with terrain, decals, and static props.

For each mission, play 30–60 seconds of active gameplay (camera movement, mech motion, weapon fire) at varied terrain coverage.

### 2.3 What to watch for

**Failure mode 1 — color-channel corruption (the claim's stated symptom).**
- Mechs render with wrong color channels (e.g., red mech appears blue, or RGB swizzle visible).
- Stripe artifacts, banding, or block-noise on mech pixels.
- Color clamps to wrong values (whites become magenta, etc.).

**Failure mode 2 — framebuffer artifacts.**
- Black or white horizontal/vertical bands across the screen at mech-pixel coverage areas.
- Garbage texels visible in the mech region.

**Failure mode 3 — draw drops** (the documented attribute-0 rule's failure mode, in case it generalizes to fragment outputs).
- Mech meshes intermittently disappear and reappear.
- Fragments missing on one frame, present the next.

**Success indicator — post-shadow correctness.**
- Mechs darken when in terrain shadow EXACTLY as they do today.
- No visible difference vs. the mainline build (which is the goal — Option A is supposed to be byte-equivalent).

### 2.4 If inconclusive

Subtle changes that could be either side: capture a side-by-side screenshot of a known mech-in-shadow scene from canary vs. mainline. If the difference is hard to characterize, **default to Option Hybrid** (the safe path) and record the inconclusiveness here.

A RenderDoc capture of a single frame (mech mid-screen, in terrain shadow) would resolve any subtle ambiguity by letting the operator inspect attachment 1 contents directly. RenderDoc capture is optional — only needed if visual observation is inconclusive.

### 2.5 Recording the result

Fill in §3 below with one of three verdict templates and commit this file on the F3 branch (`claude/nifty-mendeleev`).

---

## 3. Verdict (TO BE FILLED)

### 3.A — If canary CLEAN (preferred outcome)

```
**Verdict: CLEAN.** No corruption observed on either mission. Mech post-shadow
correct. AMD RX 7900 (driver <version>) writes location=1 outputs as declared
without artifacts.

Implication: F3 implementation proceeds with **Option A**. The
gos_postprocess.cpp:519-520 comment is stale or mis-remembered; update
docs/amd-driver-rules.md to footnote it as "Tested 2026-MM-DD, not observed."
The canary commit itself is suitable to cherry-pick as the first
shader migration commit on the F3 implementation plan.

Missions tested: <mission1>, <mission2>
Hardware: AMD RX 7900 XTX, driver <version>, Windows 10 build <build>
Test duration: <minutes>
```

### 3.B — If canary CORRUPTS

```
**Verdict: CORRUPT.** Visible <symptom> observed on <mission>. AMD RX 7900
corruption claim is REAL.

Implication: F3 implementation proceeds with **Option Hybrid**. Promote the
gos_postprocess.cpp:519-520 comment to a numbered rule in
docs/amd-driver-rules.md:
  9. Non-terrain shaders writing layout(location=1) corrupt color
     output on AMD RX 7900 XTX (driver <version>). Confirmed by
     canary 2026-MM-DD on <mission>. Group II shaders must be kept
     incomplete and rely on the driver's vec4(0,0,0,0) undeclared-output
     default; bracket their draws with enableMRT/disableMRT scope.

Missions tested: <mission1>, <mission2>
Hardware: AMD RX 7900 XTX, driver <version>, Windows 10 build <build>
Symptoms: <description>
RenderDoc / screenshot capture: <path or N/A>
```

### 3.C — If INCONCLUSIVE

```
**Verdict: INCONCLUSIVE.** <description of subtle difference>. Cannot
distinguish corruption from rendering variance.

Implication: defaults to **Option Hybrid** per F3 design spec v2 §4.1
inconclusive-outcome rule. Document the ambiguity for a future re-run with
RenderDoc inspection.

Missions tested: <mission1>, <mission2>
Hardware: AMD RX 7900 XTX, driver <version>, Windows 10 build <build>
Capture (if any): <path>
```

---

## 4. Cleanup after recording verdict

Regardless of verdict:

```bash
# Record the verdict in this file (§3 above), then commit on the F3 branch:
cd A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev
git checkout claude/nifty-mendeleev
# (edit §3 above with the verdict template that matches)
git add docs/superpowers/specs/render-contract-f3-canary-report.md
git commit -m "spec: F3 canary verdict — <CLEAN|CORRUPT|INCONCLUSIVE>"
```

**If verdict is CLEAN:** keep `claude/f3-amd-canary-temp` until the F3 implementation plan picks up the shader change. Optionally cherry-pick `0173a31` as the first commit of Option A's implementation. Then delete the throwaway branch.

**If verdict is CORRUPT or INCONCLUSIVE:** `git branch -D claude/f3-amd-canary-temp`. Re-deploy from `claude/nifty-mendeleev` to restore the unmodified shader on disk:

```bash
git checkout claude/nifty-mendeleev
cmake --build build64 --config RelWithDebInfo --target mc2
cp -f build64/RelWithDebInfo/mc2.exe A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe
cp -f shaders/gos_tex_vertex_lighted.frag A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_tex_vertex_lighted.frag
diff -q shaders/gos_tex_vertex_lighted.frag A:/Games/mc2-opengl/mc2-win64-v0.2/shaders/gos_tex_vertex_lighted.frag
```

Then proceed with the Option Hybrid implementation plan.

---

## 5. References

- [F3 design spec v2](2026-04-26-render-contract-f3-mrt-completeness-design.md) — §4 canary protocol, §5 architecture under each outcome
- [F3 pass audit](render-contract-f3-pass-audit.md) — §3.3 canary target rationale
- AMD driver rules: [`../../amd-driver-rules.md`](../../amd-driver-rules.md) — to be updated with verdict outcome
- The intent comment under test: [`GameOS/gameos/gos_postprocess.cpp:519-520`](../../../GameOS/gameos/gos_postprocess.cpp)
- The canary commit: `0173a31` on branch `claude/f3-amd-canary-temp`
- Worktree memory `stale_shader_cache_symptom.md` — A/B before reverting if visual symptoms appear after multi-mission session
