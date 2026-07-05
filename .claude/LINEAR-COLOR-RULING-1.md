# LINEAR-COLOR-RULING-1 — sRGB-approx vs linear: what to rule on

**Slice:** LIGHTING-STAGE1-TRIO deliverable 3 (`LINEAR-COLOR-RULING-1` in
`.claude/LIGHTING-MODERNIZATION-PROPOSAL-1.md` §7 table, row 1.3).
**Status: ANALYSIS ONLY.** No source touched by this doc. Builds on the
already-shipped `LINEAR-COLOR-AUDIT-1` runtime probe (`.claude/LINEAR-COLOR-AUDIT-1.md`,
gate `MC2_LIGHTING_LINEAR_AUDIT=1`, `gos_postprocess.cpp:898-923`), re-verified
against this worktree's current tree.

## The one-line verdict

**MC2 today is sRGB-approx everywhere, with one live, undocumented linear
exception (opportunistic BC7 sidecar sRGB decode) that nothing downstream
accounts for.** It is not "linear vs sRGB", it is "gamma-space math with an
accidental, partial, silent linear leak."

## Where the pipeline actually sits (verified this worktree)

| Stage | Format / behavior | Space |
|---|---|---|
| Terrain colormap (`.tga`/`.burnin.tga`/`.burnin.jpg`, `mclib/terrain.cpp:652-663`) | Loaded via standard (non-sRGB) texture path; **pre-baked-lit** — offline lighting already multiplied into pixels, not a raw albedo | Gamma container, treated as final radiance, no decode |
| Static-prop / mech / most albedo textures | Loaded RGBA8 UNORM via `mclib/txmmgr.cpp`, no `GL_SRGB8_ALPHA8` internal format requested anywhere in the runtime upload path | Gamma, sampled as-is (no decode) |
| **BC7 `.ktx2` sidecar opportunistic upload** (`mclib/txmmgr.cpp:4639-4653`, gate `MC2_TEXMGR_COMPRESSED_UPLOAD`, **default ON**) | When a sidecar's `vkFormat==146` (sRGB variant), upload uses `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM` — **GL hardware-decodes this texture's samples to linear at fetch time** | **Linear** (the one real exception) — applies to static shared textures: terrain, trees, buildings |
| ORM (roughness/metal/AO), static-prop path | Non-sRGB BC7, correctly treated as linear data | Linear (correct by construction) |
| Lighting/shadow/ambient combine (terrain frag, static-prop frag, mech) | Straight multiply/add on whatever the sampler returned | Gamma math on (mostly gamma, sometimes silently-linear) inputs — **inconsistent per-texture**, not just "wrong in one direction" |
| Scene FBO | `GL_RGBA16F`, holds whatever the shading math produced | Container is HDR-capable; contents are gamma-ish values, not radiometric linear |
| Output / present | **No `GL_FRAMEBUFFER_SRGB` enabled anywhere** (`grep` of the full tree: zero call sites in engine code; only `tools/asset_viewer` — an offline preview tool, not the game — requests `GL_SRGB8_ALPHA8`) | No encode step; RGBA16F values sent to present as-is |
| Runtime probe verdict (`MC2_LIGHTING_LINEAR_AUDIT=1`, prior audit) | `framebuffer_srgb=0 scene_color_internalformat=0x881A(RGBA16F) verdict=GAMMA_SPACE_MATH` | Confirms table above at runtime, not just from source |

## Why this is worse than plain "sRGB-approx" (the new finding this ruling adds)

The prior audit (`LINEAR-COLOR-AUDIT-1`) established the binary fact: no
`GL_FRAMEBUFFER_SRGB`, gamma-space math throughout. This ruling adds the
finding that **it isn't uniformly gamma** — the BC7-sidecar opportunistic
compression path (`MC2_TEXMGR_COMPRESSED_UPLOAD`, default ON, applies to
*any* static shared texture with a `.ktx2` sidecar marked sRGB) silently
flips a subset of terrain/tree/building textures to linear-decoded samples,
while:
- the *rest* of that same material's inputs (ORM already linear, but the
  colormap/albedo on non-BC7-sidecar assets stays gamma) and
- every downstream consumer (lighting combine, tonemap-if-any, output encode)

remain unaware of the split. In practice today this is likely low-visible-impact
(BC7 sidecars are opt-in per-asset and the terrain colormap itself — the
dominant visual surface — is not on this path per `terrain.cpp:652-663`
loading `.tga`/`.jpg` through the ordinary path), but it means **the "gamma
throughout" mental model the rest of this proposal relies on (§2, §6) is not
quite true today**, and any future asset that ships a BC7 sRGB sidecar for a
prop/building silently gets different (more correct, but inconsistent)
treatment than its neighbors with no code change and no log line marking it.

## What a full audit/remediation would cost

Per the existing audit's own remediation order (`LINEAR-COLOR-AUDIT-1` §"What
linear correctness requires"), a full fix is **not** a probe-and-done; it is
a coordinated, mutually-dependent 3-part change:

1. **Decode albedo → linear on sample.** Terrain colormap is the hard case:
   it's pre-baked-lit, so "decoding" it changes what the bake actually means
   (is it stored gamma-encoded radiance, or gamma-encoded albedo-times-light?
   nobody has audited the bake tool's convention — that's its own investigation,
   not assumed here). Static-prop/mech albedo is the easier case (switch
   internal format to `GL_SRGB8_ALPHA8` where not already BC7-sidecar-linear).
2. **Encode linear → sRGB on output.** Either `glEnable(GL_FRAMEBUFFER_SRGB)`
   on the present target, or an explicit encode in the final post/blit pass.
   This is a **global look change** — every stock mission's screenshot changes,
   full Baseline-A pixel-gate re-verification required per mission.
3. **Reconcile the BC7-sidecar linear leak** — once (1)+(2) land, the
   currently-silent linear decode on BC7 sRGB sidecars stops being an
   inconsistency and becomes simply "already correct"; until then it should
   at minimum be logged (one-shot, like the existing probe) so QA/authoring
   knows which assets are already-linear vs still-gamma.

Rough cost shape (no code written, directional only):
- **Instrumentation-only continuation** (log which live textures hit the
  BC7-sRGB path + confirm/deny the terrain-bake gamma convention with the
  bake tool author/script): **XS**, near-zero risk, no mission re-verification
  needed (probe-only, same class as the shipped audit).
- **Full linearization (steps 1+2 together, mandatory pairing per the existing
  doc's warning against doing them independently):** **M-L** engineering
  effort (touch every sampled-albedo call site + one global GL state change),
  but **the verification cost dominates**: every stock mission needs a fresh
  Baseline-A pixel comparison because the visual result changes everywhere,
  not just where new code runs. This is the same class of risk the tone-pipeline
  section (§6) already flags for its own reasons (sunset-grade relocation) —
  the two would ideally land in the same verification pass rather than two
  separate full-mission A/B cycles.
- **Terrain colormap bake-convention re-audit** (prerequisite sub-task under
  step 1): **S**, mostly archaeology (find/read the offline bake tool,
  determine whether "pre-baked-lit" pixels are gamma-encoded scene-referred
  values or something else) — cheap but blocking, since guessing wrong here
  and decoding anyway would silently re-grade every terrain texture in the
  game incorrectly.

## What this gates (stage 2/5 quality)

- **§2 Material response (GGX/Smith/Schlick convergence):** a physically-based
  BRDF *assumes* linear inputs. Running it on gamma-space albedo produces a
  response that is directionally plausible but **quantitatively wrong**
  (over-dark in shadow falloff, over-bright at grazing angles) — exactly the
  proposal's own §2 caveat ("BRDF ships in current sRGB-approx space and is
  re-graded after the audit ruling"). Every BRDF tuning decision made before
  linearization is provisional and will need re-tuning after.
- **§6 Tone pipeline v2 (tonemap/bloom):** a tonemap operator is defined on
  linear scene-referred values. Feeding it gamma-space RGBA16F content means
  the curve is compensating for a color-space error, not doing tone mapping —
  this is exactly the "v1 crushed because there was nothing HDR/linear
  underneath it" lesson the proposal's §6 already draws from the deleted
  ACES stack. Tonemap v2 tuned pre-linearization would need the same
  re-tune-after treatment as the BRDF.
- **§2.4 Specular env cubemap:** reflections sampled against a gamma-treated
  scene will have visibly wrong roughness-mip falloff (the mip chain is built
  assuming linear radiance blending) — a second-order but real quality cap.

Net: stages 2 and 5-6 can **ship and look better than today** without waiting
on linearization (the proposal's own "coordinate, don't block" framing is
sound), but their *final* quality ceiling is capped until linear lands, and
every BRDF/tonemap tuning pass done before linearization is throwaway work
that gets redone after.

## A/B recommendation

**Option A — Defer linearization past Stage 5, ship stages 1-2/5-6 as
provisional-tuned now, re-tune once after linearization lands as its own
dedicated slice (matches the existing proposal's own "recommend not blocking
S3 on it" stance).**
- Pro: fastest visible progress; matches how this proposal is already staged;
  avoids a full-mission Baseline-A re-verification cycle before the showcase
  work even starts.
- Con: BRDF/tonemap tuning work done in stages 2-6 is explicitly throwaway
  (gets re-tuned once linear lands) — accepted cost, not free.

**Option B — Pull linearization forward, before §2 material convergence,
as its own gated slice with full Baseline-A re-verification.**
- Pro: every subsequent BRDF/tonemap tuning decision is final, not provisional;
  avoids doing the same tuning work twice.
- Con: it is the single highest-risk visual change in the entire arc (global
  look shift, every stock mission's reference screenshot changes), it has an
  open unresolved prerequisite (terrain bake-convention archaeology — an
  unknown until someone reads the bake tool), and it delays the showcase-visible
  wins (Stage 1, already shipping via this same slice) behind an M-L-cost,
  high-verification-cost change.

**Recommendation: Option A.** Ship the already-staged Stage 1 showcase tuning
now (this slice); treat linearization as a dedicated, later, fully-gated slice
of its own — matching the existing proposal's Stage roadmap and the original
audit's own conclusion ("do it as its own dedicated slice ... AFTER
SCENE-LIGHTING-STATE-1, not folded into a feature"). The one actionable
near-term follow-up this ruling surfaces beyond the original audit: **log
the BC7-sidecar sRGB decode** (one-shot probe, same shape as the existing
`MC2_LIGHTING_LINEAR_AUDIT` gate) so the "gamma throughout" assumption
stays honest while stages 2-6 proceed on top of it — small, cheap,
instrument-only, and prevents the inconsistency from being rediscovered
the hard way mid-arc.
