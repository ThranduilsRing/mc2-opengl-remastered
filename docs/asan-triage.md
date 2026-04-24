# ASan Findings Triage — 2026-04-24

Full inventory of ASan MVP findings across base game + mod content, with
severity, reproducer, root-cause hypothesis, and proposed fix. Format:
highest impact at top.

Source of truth for the longer write-ups: `docs/asan-follow-ups.md`.
Source of truth for "what was run where and when":
`docs/asan-autonomous-run-log.md`.

## Verification status (2026-04-24 after fix pass)

| # | Fix commit | Verified? |
|---|------------|-----------|
| 4 | `933d716 fix(gameplay): guard neutral turret enable lookup` | **code correct, not directly exercised** — mc2_03 ABL crashes before any turret tick; will verify itself once finding #3 is fixed and tier1 reaches play phase |
| 5,6 | `e7d68c0 fix(abl): add code-segment sentinel byte` | **verified** — the 1-byte heap-buffer-overflow at `ablexec.cpp:361` is gone on all three tier1 re-runs (mc2_03, mc2_10, mc2_24). The crash line is now `ablexec.cpp:372` (same `getCodeToken`, renumbered by the fix's added lines) with a *different* bug class (AV on corrupt pointer `0xNN00000001`) — this is finding #3's signature |
| 3 | _(not fixed yet)_ | **severity upgraded to P0** — base-game `mc2_03_turretguard2.abl`, `mc2_03_dred.abl`, etc. use the same `startpatrolpath` / `startposition` parser-gap vocabulary. Finding #3 was masked by #5/#6 in the crash ordering; with #5/#6 fixed it surfaces on stock tier1 |

Re-run detail: `tests/smoke/artifacts/2026-04-24T14-24-*/` (one directory per mission, `mc2_03.log` / `mc2_10.log` / `mc2_24.log`).

## Severity scale

- **P0** — hard crash, reproducible on base-game content (affects
  shipping builds, any player).
- **P1** — hard crash on mod content we care about (Carver5O, Exodus,
  Magic Expansion).
- **P2** — ASan-only symptom that doesn't affect shippable builds.

## Triage table

| # | Severity | Site | Missions | Class | Fix cost | Fix strategy |
|---|----------|------|----------|-------|----------|--------------|
| 5,6 | **P0** | `ablexec.cpp:361 getCodeToken` (heap-buffer-overflow, 1 byte past end) | mc2_10, mc2_24 (base game tier1) | Off-by-one in ABL code-segment emission/execution | **S** (1-2 line fix) | Either add explicit terminator byte in `createCodeSegment` and emit end-of-segment check in executor, OR size allocation as `codeSegmentSize + 1` so the 1-byte overread hits slack. |
| 4 | **P0** | `turret.cpp:588 Turret::update` (global-buffer-overflow) | mc2_03 (base game tier1) | Negative `teamId` (neutral turret) indexes `turretsEnabled[-1]` | **XS** (1 line) | `if (getTeamId() < 0 || !turretsEnabled[getTeamId()])` — guard the index. |
| 2 | P1 | `move.cpp:3069 GlobalMap::clearPathExistsTable` (AV, `0xBE`-pattern pointer) | Carver5O mc2_01 | Partial-init of `GlobalMap` skips `pathExistsTable = NULL` assignment; non-zero garbage passes null-guard | **S** | Initialize `pathExistsTable` (and sibling pointer fields) in `GlobalMap` constructor instead of relying on `init()`. |
| 3 | **P0** _(upgraded)_ | `ablexec.cpp:372 getCodeToken` (AV, `0xNN00000001`-pattern pointer) | mc2_03, mc2_10, mc2_24 (base game, after #5/#6 fix); Exodus mc2_01, Magic Expansion mc2_01 | ABL compile fails with SYNTAX ERROR but mission init continues with corrupt bytecode; VM derefs garbage `codeSegmentPtr` in `execDeclaredRoutineCall` path | **M** | Preferred: `ABLModule::compile` failure aborts mission load (hard fail). Alternative: sanity-check `codeSegmentPtr` against module's segment range in VM hot loop. Base-game trigger vocabulary: `startpatrolpath`, `startposition` — narrow audit set, likely same class as the 3-name `magicAttack`/`corePatrol`/`coreGuard` collision the parallel session identified. |
| 1 | P2 | `gos_postprocess.cpp` FBO/post-process (hypothesized) | All ASan runs (visual) | Uninit FBO attachment handle or stale `glClear` mask surfaced by ASan's memory-layout change | **M** (investigation needed first) | Add `glGetError()` drains around FBO binds + log FBO handles at init + log `glClear` bitmasks. See follow-ups doc §1 for diagnostic toggles (F5 / RAlt+4 / F3). |

## P0 fixes deserve urgency

Findings #4, #5, #6 reproduce on the *base-game tier1 missions* —
`mc2_03`, `mc2_10`, `mc2_24` — which every player sees and which the
smoke harness considers canonical regression references. Under ASan
they are 100% crash-on-load / crash-mid-mission. On the normal
RelWithDebInfo build they happen silently (heap slack absorbs the
1-byte read; bad team index produces whatever the adjacent global
happened to contain — likely the neighboring `Turret::turretsEnabled`
8-byte slot, which just means "turret always disabled for neutral
teams").

The normal-build symptom of finding #4 is that neutral turrets
occasionally fail to engage (they read the wrong "enabled" bit). This
is likely an observable gameplay oddity that has been around forever.

The normal-build symptom of findings #5/#6 is nothing visible — the
VM reads 1 byte of slack it isn't supposed to have, interprets it as
an opcode, and falls through because the opcode happens to decode to
something benign. This has been latent since the original MC2 1.0.

## Recommended fix order

1. **Finding #4 (Turret teamId)** — one-line guard, trivially safe.
   Re-run tier1: mc2_03 should now pass under ASan.
2. **Findings #5/#6 (ABL code-segment off-by-one)** — pick the
   "`codeSegmentSize + 1` allocation" variant to start; it's the
   smallest change and likely to make both missions pass. If it
   doesn't, fall back to the terminator-byte variant.
   Re-run tier1: mc2_10 and mc2_24 should pass.
3. **Finding #2 (GlobalMap init)** — enables clean ASan runs on
   Carver5O without masking the downstream ABL bugs that are likely
   waiting behind it.
4. **Finding #3 (ABL compile-fail fall-through)** — biggest behavioral
   change; deferred until we have other bugs sorted so we can re-test
   mod content with a single variable changed.
5. **Finding #1 (rendering regression)** — dedicated investigation
   session, not part of this batch.

## Verification plan after fixes

For each fix, re-run under ASan:
- **#4 fix** → re-run tier1 (mc2_03 must pass).
- **#5/#6 fix** → re-run tier1 (mc2_10 + mc2_24 must pass); re-run
  Carver5O (should still hit #2 which is upstream of ABL VM).
- **#2 fix** → re-run Carver5O mc2_01 (should clear past the mission
  load; next bug behind it, if any, gets reported).
- **#3 fix** → re-run Exodus + Magic Expansion mc2_01 (compile error
  should now abort mission load cleanly; user sees the SYNTAX ERROR
  list but no AV).

After all four:
- Tier1 under ASan should pass 5/5 (pristine base game clean under ASan).
- Mod-content runs should either (a) pass the ASan session or (b)
  report a new, previously-hidden bug that was behind these.

## Mod content coverage map (what was run and where)

| Install | Content | Missions run | Result |
|---------|---------|--------------|--------|
| `mc2-win64-v0.1.1/` | base game | mc2_01, mc2_03, mc2_10, mc2_17, mc2_24 | 2/5 pass — finds #4 #5 #6 |
| `mc2-win64-v0.1.1/` + corebrain.abx + Sample_brains | base + brain lib additive | mc2_01 | PASS — no ASan reports |
| `mc2-win64-v0.1.1/` + gamesys.fit | base + stat overrides | mc2_01 | PASS — no ASan reports |
| `mc2-win64-v0.1.1/` + Magic Expansion overlay | base + Expansion | mc2_01 | FAIL — finding #3 duplicate |
| `MC2-MagicExpansion/` (fresh clone + Expansion) | Expansion alone | mc2_01 | FAIL — finding #3 duplicate |
| `Carver5-feasibility/` + FFmpeg DLLs deployed | Carver5O | mc2_01 | FAIL — finding #2 |
| `MC2-Exodus/` | Exodus campaign | mc2_01 | FAIL — finding #3 |

## Blockers encountered

- `A:/Games/Magic_MC2_archive/Unofficial_MechCommander_2_Patch_by_Magic_files/`
  contains only saved-webpage support files (js/css), no deployable
  game content. The actual Magic's Unofficial Patch is likely split
  across the numbered `3. Zoom Out` / `4. Weapons` / `5. Mechs` /
  `6. Mechlab` / `7. Carver V` / `8. FixABL` folders in the same
  archive root. Not tested in this batch.

## What ASan caught vs. what it missed

**Caught (6 findings):** off-by-one heap reads, global-buffer-overflow,
uninit-pointer AV, content-format-mismatch → corrupt bytecode →
downstream deref.

**Missed (not checked):** use-after-free (no candidates observed),
double-free (no candidates observed), uninitialized reads of
non-pointer values (MSVC ASan can't detect — MSan is Linux-only).

**Known out of scope:** anything inside `TG_GOSVertexPool` slab (MVP
left it un-annotated; ASan sees the slab allocation but not
intra-slab overruns). Candidate for a follow-up commit annotating
with `__asan_poison_memory_region`.

## Follow-up: `8. FixABL` + narrowed ABL collision audit

User flagged `A:/Games/Magic_MC2_archive/8. FixABL` as documented ABL
fixes that may resolve the mod-content parser errors driving finding #3.
Applying it before re-running Exodus / Magic Expansion would tell us
whether finding #3 is purely a mod-vocabulary gap (fixed by FixABL
layer) or whether the underlying "VM trusts corrupt state" bug still
fires on some other content.

Narrower collision audit (per in-session note): stock MC2 already owns
all the `core*` ordering primitives at `code/ablmc2.cpp:7886-7900`
(`coreMoveTo`, `coreAttack`, etc.). Magic's library adds a *different*
layer: `magicAttack`, `corePatrol`, `coreGuard`. Our existing MCO
stubs collide on exactly those three names. So the real audit set is
3 entries, not a 50-name sweep — much cheaper than initially feared.

Sequencing: do the P0 fixes first (finding #4, then #5/#6), then try
FixABL + re-run Exodus / Magic Expansion before deciding whether the
"compile-fail aborts mission load" fix for finding #3 is even
necessary. If FixABL resolves the SYNTAX ERROR cascade, finding #3's
trigger goes away even if the underlying VM bug remains — acceptable
outcome, worth documenting as "known-good mod content" rather than
fixing the VM.
