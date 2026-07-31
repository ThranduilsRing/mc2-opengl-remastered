# PROP-SHADOW-ENABLE-2 — status (2026-07-25)

Lane: PROP-SHADOW-ENABLE-2. Worktree `A:/Games/mc2-main`, branch
`claude/nifty-mendeleev`, HEAD `ce68d5e4`.

**VERDICT: BLOCKED — NO CODE LANDED, NO BUILD, NO CAPTURES.** The mission's
three deliverables (land `PROP-ALPHA-SHADOW-CLASS-1`; flip
`MC2_PROP_SHADOW_RECEIVE` + `MC2_SSAO` default-ON; verify the user's mc2_10
building face) all sit downstream of two live-sibling blockers documented
below. Per the mission's own build-attribution rule — *"if a sibling holds
broken/uncommitted source, WAIT rather than capture unattributable
evidence"* — and preflight's `safe_to_touch=false` hard stop, this lane
**waited**. Zero files were edited outside `.claude/`.

What this lane *did* produce: the `PROP-ALPHA-SHADOW-CLASS-1` design is now
**fully verified read-only against the actual encoding and every consumer**,
and is reduced to a 3-file, ~14-line change with no encoding widening. The
mission asked "if the encoding has no spare bits, say so and propose the
minimal correct alternative" — **the encoding HAS spare room**; details below.
A successor can land it in one shot without re-deriving anything.

## 1. Blockers (both live, both verified this session)

| # | Blocker | Evidence |
|---|---|---|
| B1 | `shaders/static_prop.frag` — the **only** file that can emit the new class — is claimed by live lane **EDGE-LIGHT-EMISSION-1** | `check_conflict` returns the conflict; lane registered `2026-07-25T17:31:34Z`, **0.98 h old**, pid 22264. Its note: *"self-illuminated cap + distance-attenuated glow"* — i.e. it is actively editing the same fragment-shader tail this change touches. |
| B2 | No clean-fingerprint build is possible: **CEMENT-UNIFORM-2** (live, 0.32 h old) holds an **uncommitted** `GameOS/gameos/gos_terrain_indirect.cpp` | `git status` shows exactly one dirty source file, `+13` lines. Any build from this tree embeds `MC2_BUILD_GIT_DIRTY 1`, making every capture unattributable — the precise failure the mission called out. |

Secondary: deploy lane `0.4d-rc1` is contended by three live lanes
(REVIEW-ALLPOSES-1, FORCE-ALL-VISIBLE-1, VEHICLE-SELFLIT-5), so even a clean
exe would have had nowhere uncontended to land.

Ten lanes are registered fresh (all < 1 h, all pid 22264). This is a
peak-concurrency window, not a stale-registry artifact — checked by parsing
`last_touch_utc` directly rather than trusting the age field.

## 2. PROP-ALPHA-SHADOW-CLASS-1 — verified design (READY, NOT LANDED)

### The encoding has spare room — no bit-packing needed

`GBuffer1` is **RGBA16F** (`render_contract.hglsl:3`). The static-prop class is
detected by a *band*, not an equality:

```glsl
bool rc_isStaticPropNoSelfShadow(PREC vec4 gbuffer1) {   // :46-48
    return gbuffer1.a > 0.1 && gbuffer1.a <= 0.5;
}
```

That band `(0.1, 0.5]` is currently occupied by **exactly one value, 0.25**.
A second in-band value is free: it costs no bits, requires no repacking, and
leaves the canonical threshold `rc_pixelHandlesOwnShadow` (`a > 0.5`, :94-96)
untouched. This is why the mission's fallback ("propose the minimal correct
alternative") is not needed.

### Change 1 — `shaders/include/render_contract.hglsl` (+~10 lines)

```glsl
// Alpha-test LATTICE prop (chain-link fence, railing, grating). Same static-
// prop class as 0.25 — still inside the (0.1,0.5] band, so BOTH
// rc_isStaticPropNoSelfShadow and rc_pixelHandlesOwnShadow behave exactly as
// before — but distinguishable, so the PROP-SHADOW-RECEIVE opt-in can skip
// it. Rationale: the shadow depth pass has no alpha discard, so a lattice
// writes its depth into the cascades as a SOLID slab; letting it RECEIVE
// that depth self-shadows the whole lattice uniformly. CASTING is untouched
// (the caster set is CPU-side, mclib/txmmgr.cpp) so fences keep casting onto
// terrain, which they correctly do today.
PREC vec4 rc_gbuffer1_screenShadowEligible_noSelfShadow_alphaTest(PREC vec3 n) {
    return vec4(n * 0.5 + 0.5, 0.375);
}

bool rc_isAlphaTestProp(PREC vec4 gbuffer1) {
    return gbuffer1.a > 0.3 && gbuffer1.a <= 0.5;
}
```

`0.375` is exactly representable in fp16 and sits `0.125` from `0.25` —
roughly three orders of magnitude above the fp16 ulp at that magnitude, so the
margin matches the "comfortable float-precision margin" the existing comment
claims for 0.25.

### Change 2 — `shaders/static_prop.frag:616` (1 line becomes 3) — **B1 blocks this**

The discriminator **already exists in the shader**: `u_materialFlags` bit 0
(`ALPHA_TEST_BIT`, declared :119, const :206, already consumed at :286 and
:406). The batcher already populates it per packet —
`pkt.materialFlags = typeShape->alphaTestOn ? STATIC_PROP_FLAG_ALPHA_TEST : 0`
(`gos_static_prop_batcher.cpp:2666`). So the emit side is a pure shader-side
select: **no new uniform, no new SSBO field, no CPU cost, no batcher change.**

### Change 3 — `shaders/shadow_screen.frag:333`

Gate the RECEIVE opt-in with `&& !rc_isAlphaTestProp(normalData)`.

**Deliberately NOT changed:** the static-shadow-map path at :272/:276. It keys
off `rc_isStaticPropNoSelfShadow`, which still matches both 0.25 and 0.375, so
that path stays byte-identical. Only the dynamic-cascade opt-in narrows.

### Safety properties (each verified, not assumed)

- **Contract census passes unchanged.** `scripts/check-render-contract-gbuffer1.sh`
  enforces only (a) no raw `GBuffer1 = vec4(` outside the helper file, and
  (b) single-shader usage of the two *legacy* escape hatches
  (`legacyTerrainMaterialAlpha`, `legacyDebugSentinel...`). A new `rc_*` helper
  trips neither. **No census-script edit required.**
- **No C++ contract harness is affected.** A grep for `noSelfShadow` /
  `isStaticProp` / the band constants across `RenderCore/`, `GameOS/`,
  `tests/`, `scripts/` finds **no C++ reader** — the band encoding lives purely
  in the GLSL contract header.
- **Default-OFF is a provable no-op.** With `MC2_PROP_SHADOW_RECEIVE` still
  default-OFF, 0.375 and 0.25 are indistinguishable to every existing reader,
  so shipped pixels cannot move. The class change is safe to land *before* any
  default-ON decision — and **should** be, as a separate commit.
- **The cheap fix stays rejected, now with proof.** The caster set is assembled
  CPU-side (`mclib/txmmgr.cpp:2940-2963`, replayed per cascade at :3170-3182)
  and never consults GBuffer1. So this class can only ever change RECEIVE.
  Marking alpha-test recipes `noShadow` would have killed casting too.

## 3. Default-ON flip — NOT DONE, and the gate for it is unchanged

The flip was conditional in the mission: *"if and only if the fence artifact is
gone."* Since the fence fix could not be landed (B1) or built (B2), the
condition was never testable and the flip was **not** made.
`MC2_PROP_SHADOW_RECEIVE` and `MC2_SSAO` remain default-OFF.

**Perf cost: NOT MEASURED — no Tracy capture was taken, because no attributable
exe existed to profile.** Stating this plainly rather than recycling the prior
lane's argued bound. The honest position carried forward:

- Caster side: **zero** — props already render into the cascades unconditionally.
- RECEIVE side: one extra `sampleDynamicShadow()` PCF for prop pixels only,
  inside the already-running fullscreen pass. Small, but unmeasured.
- **SSAO is the real cost** and the real risk: a half-res AO pass plus
  composite, every frame, at full screen coverage — not proportional to the
  0.25–0.73 % prop pixel coverage that makes RECEIVE cheap. Given the
  light-pool cap raise already cost ~34 % at the busiest night pose, **SSAO
  default-ON must not be flipped on an argued bound.** It needs a free-timestep
  Tracy A/B at a night pose before anyone defaults it.

## 4. The user's building-face case — NOT RE-VERIFIED THIS SESSION

No captures were taken (B2). The prior lane's categorical night evidence at the
mc2_10 compound stands unrefuted and is unchanged in
`release_assets/0.5.2/REVIEW/08_prop_self_shadow/`; nothing this session either
strengthens or weakens it. The user's specific complaint — camera-facing wall
lit while the light is behind — remains **reproducible and unfixed in the
shipped default configuration**, because the cure is still behind two
default-OFF gates.

## 5. Recommended successor sequence

1. **Wait for EDGE-LIGHT-EMISSION-1 to release `shaders/static_prop.frag`, and
   for CEMENT-UNIFORM-2 to commit `gos_terrain_indirect.cpp`.** Re-run
   `preflight` until `safe_to_touch=true`.
2. Land changes 1–3 above as **one commit, default-OFF, provably no-op**.
   Run `scripts/check-contracts.sh` (expect 65/65) + the C++ harnesses.
3. Clean build → `deploy --require-clean` → md5 verify → record sha.
4. **Fence evidence first**, on mc2_01 day (clean quantitative arm): fences
   casting onto ground = YES, fences self-shadowing as slabs = NO. This is the
   gate for step 5 and it is cheap to run.
5. Only then consider the default-ON flip — and split it: RECEIVE and SSAO are
   very different perf propositions and should be decided separately, with a
   free-timestep Tracy night A/B for SSAO.

## 6. Lane hygiene

- **No source file edited.** Only `.claude/PROP-SHADOW-ENABLE-2-STATUS.md` and
  `release_assets/0.5.2/REVIEW/08_prop_self_shadow/README.md` written.
- `release_assets/0.5.2/REVIEW/INDEX.html` **deliberately not touched** — it is
  auto-generated and the directory is claimed by live lane REVIEW-ALLPOSES-1.
  The section-8 refresh was made as an additive README instead.
- No build, no deploy, no smoke, no push, no `--amend`. Tree left exactly as
  found (the one dirty source file is CEMENT-UNIFORM-2's, untouched).
