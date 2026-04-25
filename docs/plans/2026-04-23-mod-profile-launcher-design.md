# Mod Profile Selector / Launcher — Design

Status: design. Do not implement yet — this is the shape of the feature and the first content list.

## Motivation

We now have three distinct "MC2 experiences" we want playable without data/ cross-contamination:

- Stock Carver V campaign (what the engine ships with).
- Magic's Unofficial Patch + corebrain 1.45 + rebalanced Carver V.
- Magic's "New Exodus" user campaign (self-contained content tree with its own missions, warriors, movies, per-mission water flipbooks, burn-in, detail textures).

Plus, on deck: MCO / Omnitech, MC2X / Wolfman imports (see `memory/mco_omnitech_integration_attempt.md`, `memory/mc2x_integration_attempt.md`, `memory/carver5_mission_playable.md`). All of these today rely on manually overlaying files into `mc2-win64-v0.1.1/data/`. That's worked but is fragile:

- "Which loose file broke vanilla?" diagnosis has no clean answer.
- A/B comparisons require full redeploys.
- Content that overlaps (e.g. corebrain.abx vs. our 51 ABL stubs) silently collides.

A profile is the right abstraction: a named, self-contained content root that the launcher selects at startup.

## Scope of the v1 launcher

In scope:

- Profile directory layout on disk.
- `profile.json` schema.
- Launcher UI (minimal: list + select + launch).
- Engine-side path redirection (artPath / tglPath / objectPath / missionPath / texturePath).
- Profile inheritance (a profile may declare `base: "stock"` and only carry overrides).
- Launch-time diagnostic log: which files in the profile shadow which FST entries.

Explicitly out of scope for v1:

- In-game profile switching (restart required).
- Profile download / repository.
- Per-profile save games (stretch — noted below).
- Modifying the FST packer.

## Directory layout

```
mc2-win64-v0.1.1/
  mc2.exe
  profiles.json                  # launcher-maintained list
  profiles/
    stock/
      profile.json
      (no data/ — uses FST only)
    magic_carver_v/
      profile.json
      data/
        missions/
          corebrain.abx
          gamesys.fit
          mc2_01.fit ... mc2_24.fit
          purchase*.fit
          warriors/
            dredAttack01.abl ... (Magic's Carver V brains)
        art/
          (Magic's mechlab/weapon art)
        objects/
          compbas.csv
          effects.csv
          (rebalanced mech CSVs)
    exodus_1_1/
      profile.json
      data/
        campaign/exodus.fit
        missions/ (a_*, e_*, x_*, purchase, warriors/)
        movies/ (.bik)
        textures/ (per-mission water flipbooks, burnin, detail, lst)
        art/
    mcox_carver5o/                  # stretch — mirror existing mc2x-import
      profile.json -> base: mcox
    wolfman/                        # stretch
      profile.json
```

Each profile's `data/` mirrors the stock layout exactly. The engine at load time resolves paths against the selected profile's `data/` first, then the stock `data/` (if the profile declares `base: "stock"`), then FST.

## profile.json schema

```json
{
  "id": "magic_carver_v",
  "name": "Magic's Unofficial Patch — Carver V",
  "version": "1.45",
  "author": "Magic",
  "base": "stock",
  "description": "Corebrain 1.45 smarter AI + Gold-like weapon ranges + rebalanced Carver V missions/purchase.",
  "overrides": {
    "missions": true,
    "warriors": true,
    "art": true,
    "objects": true
  },
  "default_mission": "mc2_01.fit",
  "requires_engine": ">=0.1.1",
  "notes": [
    "Requires ABL stub audit — see compatibility matrix."
  ]
}
```

- `base` — null, `"stock"`, or another profile id. Forms a linear chain; no diamond inheritance in v1.
- `overrides` — soft declaration; used for the launcher's "what this changes" tooltip and for the diagnostic log.
- `default_mission` — hint for launcher quick-launch.
- `requires_engine` — if the exe is older, launcher refuses with a clear message.

## Engine wiring (sketch, not code)

Current path config lives near FastFile / `txmmgr` init and in `mc2.cfg`. The minimum changes:

1. Add `--profile <id>` CLI arg and equivalent `ProfileId` key in `mc2.cfg`.
2. At engine init, resolve the profile chain into an ordered list of content roots (`[profiles/magic_carver_v/data, data]`).
3. Patch `File::open` (the loose-file override seam documented in memory) to walk the list in order before the FST fallback.
4. Emit a `[PROFILE]` diagnostic line per load hit showing which root served the file — gated by an env var so release is quiet.

No shader changes, no renderer changes. This is pure file-layer plumbing.

## Launcher UI — v1 shape

Either a standalone tiny Win32/SDL window, or more pragmatically a reuse of MC2's existing `LogisticsMainMenu` with a pre-game "Select Profile" screen. Prefer the latter — the widget system already exists and matches the game's look.

- List of installed profiles (scanned from `profiles/`).
- Per-profile card: name, author, version, description, "based on" chain.
- Launch button → restarts the engine with `--profile <id>` (or re-inits in place if safe).
- "Validate" button → runs the profile-file audit (see validation below) without launching.

**v1 requirement:** per-profile save-game directory (`profiles/<id>/saves/`). Profile contamination through saves is painful and confusing — promoted out of stretch. Stock save path is redirected to the active profile's `saves/` at engine init.

## Staging Magic's archive as the first real profile

Following the advisor sketch, five profiles derived from one archive:

1. `magic_corebrain_only` — just `corebrain.abx`. Tests whether stock Carver V runs against Magic's brain.
2. `magic_corebrain_gamesys` — above + `gamesys.fit`. Tests weapon-range / sensor / pilot-skill tuning layered on smarter AI.
3. `magic_carver_v` — above + Magic's 24 Carver V missions + purchase FITs + warrior ABLs. This is the headline profile.
4. `magic_weapons_mechs` — above + `compbas.csv`, `effects.csv`, 32 mech CSVs, weapon/mechlab TGAs. Full balance mod.
5. `exodus_1_1` — independent profile (does not inherit Magic's Carver V). Bases on `stock` and overlays its full content tree, including the `.bik` movies and `textures/a_*_*.water*.tga` flipbooks.

Profiles 1-4 form an inheritance chain via `base`. Profile 5 stands alone.

## Validation order (gated, no parallel mess)

Each step must pass before the next is staged:

1. **Profile 1 boot test.** Launch stock Carver V through `magic_corebrain_only`. Expected outcomes:
   - Game starts, reaches mc2_01.
   - AI behavior observably changes (Magic's brain picks tactics from weapons loadout).
   - No new crashes in first 5 minutes.
2. **ABL stub collision audit.** *Before* step 1 results are trusted, grep our engine for every ABL function name Magic's corebrain declares (`magicAttack`, `corePatrol`, `coreGuard`, `coreEscort`, `coreSentry`, `coreRepair`, plus the tier-2 set committed in `db8c00a` per memory). For each name, determine:
   - Is the engine-side stub no-op, partial, or functional?
   - Does `ABLi_addFunction` collide by name, override, or error?
   - What does Magic's `.abx` actually export? (Disassemble with FixABL or the ABL decoder in our tree if present.)
   - Produce a compatibility matrix: `[function, our-behavior, magic-behavior, expected-winner, risk]`.
3. **Profile 2.** Layer gamesys.fit. Verify weapon ranges in-game (short/medium/long 80/160/240) and sensor-has-range (previously zero).
4. **Profile 3.** Swap to Magic's Carver V missions + warriors. Compare against the stock mc2_01 playable baseline from `carver5_mission_playable.md` — same mission name, different FIT/warrior content.
5. **Profile 4.** Drop weapon/mech rebalance CSVs + art. Boot test + one combat run per changed weapon class.
6. **Profile 5 (Exodus).** Isolated. First mission (`e_1_0.fit`) must boot. Then check:
   - `.bik` movie playback for campaign intros.
   - Per-mission animated water flipbooks (`a_0_5.water0000.tga` ... `0015.tga`) — verify they reach disk via our loose-file path and, critically, verify the water shader path (`water_rendering_architecture.md`) doesn't require engine changes to consume flipbooks (it probably does — note it).

Stop and file a task the moment any step regresses a previously-passing profile.

## ABL stub collision — specific test plan

This is the highest-priority compatibility test because failure mode is silent misbehavior, not a crash.

1. Enumerate names from Magic's corebrain — either via FixABL output, `.abx` disassembly, or by reading `Brain_Library.txt` (already extracted) and sample brains in `1. Brain Library/Sample_brains/`.
2. Enumerate names our engine registers — grep for `ABLi_addFunction` and adjacent registration sites in our source tree. Cross-reference against the tier-2 commit `db8c00a` file list.
3. Build the compat matrix described above.
4. **Determine actual runtime precedence first — do not add override machinery speculatively.** Run Profile 1 with stubs registered as today and observe:
   - Does Magic's `.abx` load without error when C++ stubs of the same name already exist?
   - When a brain calls `magicAttack`, which implementation runs? (Instrument with a one-line `[ABL]` print in each stub; infer from log presence/absence.)
   - If `.abx` functions already win at runtime, or no collision exists, **stop here** — the profile system needs no ABL logic.
5. Only if step 4 proves our stubs shadow Magic's real functions and break observable behavior: add the minimum mechanism to fix it (per-profile `abl_overrides` list, or unconditional unregister at `.abx` load — decide at that point, not now).
6. Re-run Profile 1 boot test and observe whether AI behavior matches Magic's documentation (auto tactic switch on loadout change is the most visible indicator).

## Open questions / risks

- **FST ordering.** Today `File::open` tries loose disk then FST. Does it honor multiple loose roots? We need to confirm no hidden assumption that `data/` is a single path — a grep will settle it, but call it out.
- **Save compatibility.** A save created under `magic_carver_v` almost certainly will not load cleanly under `stock`. Resolved by the v1 per-profile saves requirement above.
- **Texture handle cap interaction.** Exodus adds a lot of per-mission textures (`memory/texture_handle_cap.md`). We already raised the cap to 3000; verify it's still headroom after Exodus loads.
- **`burnin.jpg` vs deferred shadow pipeline.** Exodus uses baked burn-in textures. Our post-process shadow multiply (`postprocess_shadow_plan.md`) may double-darken. Expected triage: sample a burn-in mission, decide whether to skip post-shadow on burn-in tiles or tune amplitudes.
- **16-frame water flipbooks.** Concrete input for the water shader plans in `docs/plans/`. Loading the frames is trivial; feeding them into the water overlay shader as an animated sampler is a modest renderer task. Worth a separate design doc under the water effort, not bundled into the launcher.
- **FixABL.exe as a dev tool.** Shipping it alongside the launcher would let users drop arbitrary campaigns onto corebrain 1.45. Nice-to-have; not v1.
- **Legal / credits.** Magic's patch and Exodus are community content with unclear licensing. The launcher should display author/attribution from `profile.json` prominently. Don't bundle Magic's files in the repo — users install profiles to `mc2-win64-v0.1.1/profiles/` themselves. See `memory/public_fork_and_release.md`.

## v1 exit criteria

- Launcher lists `stock`, `magic_carver_v`, `exodus_1_1` (others optional).
- `stock` still works identically to today (no regressions).
- `magic_carver_v` boots mc2_01 and survives 5 min of play without crashing.
- `exodus_1_1` boots e_1_0 and reaches in-mission.
- ABL compatibility matrix committed to the repo as living doc.
- Loose-file override diagnostic log proves per-profile resolution is correct.

## Deferred to v2

- GUI polish, profile thumbnails.
- Profile dependency / conflict detection beyond simple linear `base`.
- Remote profile install.
- Integration with MCO / Omnitech and Wolfman / MC2X as first-class profiles (their engine-side blockers must clear first per the relevant memory entries).
