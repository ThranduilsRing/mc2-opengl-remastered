<!-- doc-status: current | reviewed:2026-10-11 -->
# GLSTATE-GUARD-EPILOGUE-RECON-1

Recon-only classification of the `gos_InvalidateRenderStateCache()`-terminated GL
state epilogues in the GPU-direct draw passes. **No conversion, no invalidate
removal, no guard invention.** Determines which (if any) are behavior-preservingly
convertible to `gl_state_guard.h` RAII, and what new guard types would be required.

- **Branch/worktree:** `claude/glstate-guard-adoption-2` @ `A:/Games/mc2-glstate-guard-adoption-2` (off nifty `b997200c`)
- **Date:** 2026-06-22 · **Production files changed:** NONE (doc only)
- **Follows:** GLSTATE-GUARD-ADOPTION-2 (`97f122bc`, flushShadow SSBO 0/1 — the one clean target)

## Guard vocabulary today (restore-previous semantics)
| Guard | Restores |
|---|---|
| `GlScopedSsboBinding(slot)` | SHADER_STORAGE_BUFFER base binding @slot |
| `GlScopedCapability(cap,en)` | one of GL_BLEND / GL_DEPTH_TEST / GL_CULL_FACE enable |
| `GlScopedDepthState(mask,func)` | depth write-mask + depth-func |
| `GlScopedTextureUnit(unit)` | active-texture unit + GL_TEXTURE_2D binding on `unit` |
| `GlScopedClipControl` | clip origin + depth-range mode |

**No guard exists for:** `GL_CURRENT_PROGRAM` (program), `GL_VERTEX_ARRAY_BINDING`
(VAO), `GL_ARRAY_BUFFER_BINDING`/`GL_ELEMENT_ARRAY_BUFFER_BINDING`, `glBlendFunc`
src/dst (incl. separate-alpha), `glBindSampler(unit)` sampler binding, `glCullFace`
mode, `glDrawBuffers` MRT list.

---

## Site 1 — `GpuMechBatcher::flush()` (gos_mech_batcher.cpp ~1918–2343)

**Saved (1920,1948–1973):** SSBO 0/1/2 · depth-func · depth-test · depth-mask ·
blend-enable · cull-enable · cull-**mode** · program · VAO · array-buf · elem-buf ·
active-tex · sampler 0 · tex2D units 0–4 · samplers 1–4.
**Restored (2304–2333):** all of the above (reverse order).
**Invalidate timing:** L2343, after `glFenceSync` L2335.
**SSBO semantics:** **restore-previous** (save prev @1920/1960-61, restore prev @2304-06). ✅ convertible.
**Guard-able subset:** SSBO 0/1/2 → `GlScopedSsboBinding` ×3 · depth-mask+func → `GlScopedDepthState` · DEPTH_TEST/BLEND/CULL_FACE enable → `GlScopedCapability` ×3 · tex units 0–4 → `GlScopedTextureUnit` ×5.
**No guard type yet:** program, VAO, array-buf, elem-buf, samplers 0–4 (glBindSampler), cull-**mode** (glCullFace).
**Required block scope:** YES — guards must close **before** L2343 invalidate (wrap body to ~L2334).
**Behavior-preserving?** SSBO 0/1/2: **YES, clean** (identical to ADOPTION-2). Render-state subset: **PARTIAL/low-value** — `GlScopedTextureUnit` restores unit binding but NOT the per-unit `glBindSampler`, and `GlScopedCapability(CULL_FACE)` doesn't restore `glCullFace` mode; converting only the guard-able half leaves the big manual block (program/VAO/buffers/samplers/cull-mode) in place → high churn, half-converted function.
**New guard required for full conversion:** GlScopedProgram, GlScopedVertexArray, GlScopedSampler(unit), GlScopedCullFace(mode) (+ optionally GlScopedBuffer).
**RECOMMENDATION:** **Convert SSBO 0/1/2 only**, block-scoped before the invalidate (ADOPTION-3 target). Leave all render-state manual. This is the one genuinely clean, behavior-preserving win in the whole epilogue set.

---

## Site 2 — particle tube-ribbon **immediate** flush (gos_particle_bridge.cpp 448–556)

**Saved (448–461):** program · VAO · blend src/dst · blend-enable · depth-test · depth-func · depth-mask · sampler 0 · active-tex · tex2D unit0 · cull-enable.
**SSBO 14/15/16:** **bind-then-unbind-to-0** (bind 499–501, zero 537–539) — **NOT restore-previous → NOT convertible** (a restore-previous guard would change semantics; same class as gpu_cull C1B cleanup).
**Restored (543–553):** cull · tex2D0 · sampler0 · active-tex · depth mask/func/test · blend-func · blend · program · VAO.
**Invalidate timing:** L556.
**Guard-able subset:** depth (mask+func) · DEPTH_TEST/BLEND/CULL_FACE enable · tex unit0.
**No guard type yet:** program, VAO, blend-func src/dst, sampler 0. (SSBO not applicable — bind-then-zero.)
**Required block scope:** YES (before L556).
**Behavior-preserving?** No clean SSBO win (bind-then-zero). Convertible render-state is a minority subset; leaves program/VAO/blend-func/sampler manual.
**RECOMMENDATION:** **STOP** — defer until GlScopedProgram/VertexArray/Sampler/BlendFunc exist. Converting the small subset now is churn for ~no risk reduction.

---

## Site 3 — particle tube-ribbon **deferred** flush (gos_particle_bridge.cpp 626–813)

Structurally identical to Site 2, **plus**:
- **MRT draw-buffer** save/restore (save+force-single 671–681, restore 797) — **no guard type** (`glDrawBuffers`).
- gated occlusion-query diagnostic (covOn) — irrelevant to state.
- SSBO 14/15/16 again **bind-then-zero** (753–755 / 791–793) — NOT convertible.
**Invalidate timing:** L813.
**RECOMMENDATION:** **STOP** — same as Site 2 plus the MRT-list save needs a `GlScopedDrawBuffers` that doesn't exist. Lowest priority.

---

## Site 4 — particle **billboard** flush (gos_particle_bridge.cpp 911–1183)

**Saved (911–926):** program · VAO · blend src/dst · blend · depth test/func/mask · sampler 0 · active-tex · tex2D unit0 · cull-enable; **conditional** tex2D unit1 (`savedTex2D1`, soft-particles).
**SSBO 14:** **bind-then-unbind-to-0** (1044-45 / 1150) — NOT convertible.
**Restored (1153–1170):** cull · [soft: unit1] · tex2D0 · sampler0 · active-tex · depth mask/func/test · blend-func · blend · program · VAO.
**Invalidate timing:** L1183.
**⚠ Behavior trap — CULL is NOT safely convertible here.** `glDisable(GL_CULL_FACE)` is asserted **twice on purpose** (L927 and L1042): a sub-pass (`copySceneDepthForParticles`) re-enables cull mid-function, and VFX-CARD-CULL-1 requires the cull-disable to be the *last* state before the draw loop. A `GlScopedCapability(GL_CULL_FACE,false)` set once at construction would **not** re-assert after the sub-pass → **reintroduces the spinning-explosion-card flicker bug**. Cull must stay hand-rolled.
**Also:** blend-func changes **per group** inside the draw loop (1128–1131); fine for restore-at-end but blend-func has no guard.
**Guard-able subset:** depth (mask+func) · DEPTH_TEST/BLEND enable · tex units 0/1. (NOT cull — see trap.)
**RECOMMENDATION:** **STOP** — the cull double-assert makes `GlScopedCapability` unsafe; SSBO is bind-then-zero; remaining subset is tiny. Defer.

---

## Site 5 — `GpuStaticPropBatcher` (gos_static_prop_batcher.cpp) — CLASSIFY ONLY

- The **template** both mech `flush()` and the particle epilogues copied (mech comment: *"Mirrors GameOS/gameos/gos_static_prop_batcher_flush.cpp:11"*). 8910-line TU, largest batcher; multiple flush paths (opaque flush / shadow / substrate); 10 SSBO save/restore sites, 21 state-save sites (grep).
- Same epilogue class as Site 1: big save block + (likely) restore-previous SSBO + render-state + invalidate. SSBO convertibility per-path = **TBD (not inspected — classify-only per scope)**.
- **Largest blast radius** of all sites.
**RECOMMENDATION:** **LAST, own slice, after the guard-vocabulary expansion.** Do NOT touch in ADOPTION-3. A dedicated recon (`...-STATICPROP`) should confirm each path's SSBO is restore-previous (not bind-then-zero) before any conversion.

---

## Cross-cutting conclusion

1. **The guard vocabulary is insufficient for these epilogues.** They are dominated by program/VAO/buffer/sampler/blend-func/cull-mode/MRT state — none of which has a guard. Converting only the depth/blend/cull-enable/texunit subset leaves a large manual block and half-converts the function (churn, no real risk reduction).
2. **The 3 particle SSBOs are bind-then-zero, not restore-previous** → not convertible at all (confirms the ADOPTION-2 recalibration: most GPU-direct SSBO use here is hygiene-zero, not save/restore).
3. **The only clean, behavior-preserving win is mech `flush()` SSBO 0/1/2** (block-scoped before the invalidate) — a direct continuation of ADOPTION-2.
4. **A behavior trap exists** in particle billboard (Site 4): cull must stay manual (double-assert around a sub-pass).

### Recommended slices (in order)
- **ADOPTION-3 (NARROW, low-risk):** mech `flush()` SSBO 0/1/2 → `GlScopedSsboBinding` ×3, body block-scoped to close before `gos_InvalidateRenderStateCache()` @2343. No render-state touched. Acceptance: tier1 5/5, `--gl-debug-fatal` clean, `MC2_GLSTATEGUARD_LOG=1` shows slots 0/1/2 restored identically, Δdestroys +0. Slice-preflight: **required** (hot path, block-scope restructuring).
- **GLSTATE-GUARD-VOCAB-1 (enabler, separate):** add `GlScopedProgram`, `GlScopedVertexArray`, `GlScopedSampler(unit)`, `GlScopedBlendFunc` (incl. separate-alpha), `GlScopedCullFace(mode)`, optionally `GlScopedDrawBuffers`. Header-only, each capturing prev in ctor / restoring in dtor; no adoption in the same slice. **Prerequisite** for any further epilogue conversion.
- **ADOPTION-4+ (after VOCAB-1):** revisit particle billboard / tube epilogues — but cull stays manual in billboard (Site-4 trap), and the tube SSBOs stay manual (bind-then-zero). Even with full vocabulary the net gain is modest; reassess whether it's worth it then.
- **STATICPROP:** own recon + slice, last.

### STOP list (do not convert in ADOPTION-3)
particle immediate (Site 2), particle deferred (Site 3), particle billboard (Site 4), static-prop (Site 5) — all blocked on missing guard types and/or behavior traps.
