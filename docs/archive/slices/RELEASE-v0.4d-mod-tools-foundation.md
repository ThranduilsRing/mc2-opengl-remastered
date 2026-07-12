# v0.4d — Mod Tools Foundation (Release Prep)

**Status:** RELEASE CANDIDATE PREP. nifty `claude/nifty-mendeleev` @ 23897b5d (2026-06-12).
**Theme:** the first complete, gated modder loop — create → playtest → package → install → regression-gate — on proven substrate, with a byte-stable visual baseline.

This doc is the release contract: what ships, how it was gated, what is explicitly NOT done, and the candidate checklist. Update as the remaining stabilization chips land.

---

## 1. What ships in v0.4d (banked & gated)

| Capability | Slice | Gate evidence |
|---|---|---|
| Telemetry tag registry + NDJSON lifter | S1 | drift check exit 0; golden test 435 records; tier1 5/5 |
| Smoke run-manifest + post-verdict cockpit hook | S2 | Opus SAFE (verdict-path byte-identical); manifest/oracle/telemetry artifacts; exit-parity drills |
| Engine file-resolve trace (`MC2_LOG_FILE_RESOLVE=2`) | S3 | Opus SAFE; 2117 JSONL records; tier1 5/5 env-unset |
| Shared static resolver + parity smoke | S4 | 52 unit tests; live mod parity 0-mismatch (2136/2136 mod layer) |
| FastFile `.fst` listing sidecars | S4b | 8/8 archives, 8176 entries; fastfile parity 164/164 |
| Unified dot-dir skip rule (ruling C4) | S5 | Opus SAFE; live probe absent from index; parity 0-mismatch; tier1 5/5 |
| Registry index builder (`mc2-registry-index/1`) | S6 | 27 unit tests; real v0.4 build 0.18s/2011 inputs; staleness loud-fail |
| Single canonical manifest validator (ruling C1) | S7 | scaffold deleted; fixture matrix 12/13/15; check-* green |
| `mc2mod` pack/install/uninstall/verify-lite | S8 | 19 unit tests; live mod-active smoke; uninstall byte-identical; canonical-deploy guard |
| Deterministic capture stack | S9D/S9E/S9 | scenarioTime@frame identical; 12 shader clocks pinned; 3/3 bookmarks byte-identical; Opus SAFE x3 |
| **Baseline A** golden-frame baseline | — | blessed (nifty 95efbeb6, 3 goldens); `.claude/baseline-A/` |
| `visual_diff.py` + HTML triptych comparator | — | 18 unit tests; self-match exit 0; real FLIP detection |
| Editor Playtest Launcher (active-mod + stale warn + archive) | S10 | editor build GREEN; editor-only diff; manual GUI verify |

Supporting (parallel-lane, banked on nifty): deploy fingerprint chip (`[BUILD_FINGERPRINT v1]` + smoke assert), deploy-coherence manifest, render-pass GPU timers, FrameInspector editor panel, Shadow Stability v1.

---

## 2. The modder loop (end to end)

```
Asset Viewer / editor authoring
  -> editor: save mission .pak, select mod (ModPicker -> MC2_ACTIVE_MOD)
  -> editor: "Launch Game on this Mission [mod: <id>]" (S10) -> playtest in game exe
       -> stale-build warning if game exe older than editor
       -> result archived to mods/<id>/.modproject/playtest/<ts>/playtest.json
  -> mc2mod pack <mod-dir>  (S8) -> <id>-<ver>.mc2mod + package.json
  -> mc2mod verify-lite / install --deploy <temp> / uninstall  (byte-identical restore)
  -> registry index + resolver parity (S4/S6) confirm overrides resolve as the engine sees them
  -> visual_diff vs Baseline A gates any render-affecting change
```

Resolution truth is single-sourced: `tools/mod_install/resolver.py` mirrors `mclib/file.cpp` (ruling C5), parity-gated against the engine trace.

---

## 3. Known limitations (ship with these documented)

- **Visual baseline is narrow.** Baseline A = mc2_01, 3 provisional bookmarks. Not yet ~25 bookmarks / 5 missions. Animation phase is *pinned* (deterministic) not *masked*; a new wall-clock shader time source will drift goldens unless pinned (S9E pattern).
- **Fixed-timestep pins the clock, not full sim determinism.** FX/combat visual goldens are only byte-stable on missions with a deterministic first-fire (mc2_24 yes, mc2_10 no — late combat accumulates non-pinned RNG/AI variance). `MC2_SMOKE_FIXED_TIMESTEP` is a capture/determinism knob, NOT a regression knob (sim speed is fps-proportional; missions end early).
- **`mc2mod` v1 scope:** single-mod, no multi-mod load-order solver, no signing, no central-manifest merge. FastFile parity needs `.fst.txt` sidecars present (S4b generator ships them).
- **Tube oracle stays default-OFF.** A/B was clean (Bucket A: renders, no regression) but default-ON restore + the event-tube-capture (S9F) are PINNED, not shipped. See [[tube-s9f-pinned]].
- **`MC2_VISUAL_*` and `MC2_VFX_ORACLE_TUBE` are dropped by run_smoke's Popen allowlist** — capture/trace runs launch mc2.exe directly.
- **Editor known crash (separate lane):** `BuildingBrush::update -> terrainElevation` 0xC0000005 when terrain/MapData not loaded (chip filed). Pre-existing, NOT S10. Editor smoke `asset_browser`/`foliage_menu_commands` flaky/failing on this.
- **Deploy is a matched tuple** (exe hash + shader payload + data/mod payload + cache state). Exe-only or data-only deploys are the two failure classes (magenta decals / unknown objType). Use the deploy-coherence + fingerprint checks.

---

## 4. Release candidate checklist

- [ ] **(chip 1)** smoke-preflight `--verify-only` per-path guard finished + tier1 rerun + merged (owned by `claude/run-smoke-verify-preflight-1` session)
- [ ] **(chip 2)** `modern-tree-pack-v1` example mod — exercises full mc2mod pack/install path as a shipped example
- [ ] **(chip 3)** mc2mod / asset-cook quickstart docs
- [x] **(chip 4)** release checklist + known limitations (this doc)
- [ ] **(chip 5)** tag candidate build: build nifty tip, deploy as matched tuple, capture Baseline A goldens from the tagged exe, run tier1 5/5 + editor smoke (note known editor crash), record fingerprint
- [ ] Baseline A re-captured from the tagged build; 3 goldens match or baseline re-blessed with review
- [ ] Deploy-coherence manifest written for the release deploy
- [ ] Editor BuildingBrush crash: fixed OR documented as known-issue in release notes (lane `claude/editor-buildingbrush-null-terrain-guard-1`)

---

## 5. Render lane status (parked, do NOT reopen in this arc)

- **Tube:** pinned (default-OFF; A/B clean but not flipped).
- **Shadow Stability v1:** merged (explicit pass-state brackets + `MC2_SHADOW_STATE_TRACE`).
- **GlStateGuard:** available, not started.
- **TransparentScenePass / broad state-ownership generalization:** Tube A/B + S9F friction suggest the generalization is not yet proven safe — hold.

Reopen render only after the stabilization arc ships v0.4d.
