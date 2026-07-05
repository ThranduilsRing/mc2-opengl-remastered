# QUICKSTART-PILOT-DROP-RECON-1

Recon: campaign mission 1 (mc2_01, SP_QUICKSTART) shows "Bubba"×3 player pilots, no
pilot voices. Retail gave Longshot/Hacksaw/Flash in the same flow. Read-only recon,
2026-07-05.

## Verdict (one paragraph)

The named-pilot source in SP_QUICKSTART **is and always was the mission .fit itself**:
`[Warrior1..3] st Profile = "pmwflash"/"pmwhacksaw"/"pmwlongshot"`. There is no roster
table, no LogisticsData fallback, no code mapping — quickstart pilots come only from
those Profile strings. Our **mission editor re-saved mc2_01.fit with all 29 Profile
strings blanked** (editor `Pilot::save` writes `""` when its `info` pointer is null —
crash-guard added in `42d4aa9e`, 2026-06-07). The blanked file (mtime **2026-06-27
16:44**, the editor-authoring session) lives in `mc2srcdata/missions/mc2_01.fit` AND
was deployed **loose** into `A:/Games/mc2-opengl/releases/0.5 testing/mc2-win64-v0.5.0/
data/missions/mc2_01.fit` (byte-identical). Loose files beat FST in `File::open`
priority (mclib/file.cpp:69 "mod > base-loose > fastfile"), so on that install the
blanked file shadows the **correct** copy inside `mission.fst`. Installs without the
loose file (v0.4, v0.4d-rc1, releases/mc2-win64-v0.5.0) are clean — their `mission.fst`
mc2_01.fit has proper Profiles (verified by extraction).

## Q1 — Where SHOULD quickstart pilots come from

- `code/mission.cpp:3269-3388`: `[Warriors]` loop reads `Profile` per `[WarriorN]`,
  opens `warriors/<profile>.fit`, `MechWarrior::init(FitIniFile*)` reads DescIndex
  (name via `cLoadString`), pilotAudio/pilotVideo (→ `Radio` = pilot voice), skills.
- `code/mission.cpp:4049`: each `[PartN]` reads `ul Pilot = N` → `parts[i].pilot`.
- `code/mission.cpp:1672` (`createPartObject`): `mover->setPilotHandle(parts[partIndex].pilot)`
  — index into `MechWarrior::warriorList` (newWarrior hands out 1..N in [Warriors] order).
- SP_LOGISTICS differs: parts with `commanderID==0` are skipped (mission.cpp:4253) and
  player movers come from `Logistics::beginMission` addMover with
  `LogisticsPilot::getFileName()` (logistics.cpp:794-851). SP_QUICKSTART has **no**
  such branch — fit Profiles are the sole source. `commandersToLoad` carries only
  commander/team ids (logistics.cpp:630-634), never pilots.

Retail data (verified three ways: tracked `Viewer/mission.fst`, deployed
`mc2-win64-v0.4/mission.fst`, pristine `A:/Games/mc2-opengl/mc2srcdata-fresh/missions/mc2_01.fit`):

```
[Warrior1] st Profile = "pmwflash"     → DescIndex 42005 "Flash",    pilotAudio pilotg
[Warrior2] st Profile = "pmwhacksaw"   → DescIndex 42007 "Hacksaw",  pilotAudio piloti
[Warrior3] st Profile = "pmwlongshot"  → DescIndex 42011 "Longshot", pilotAudio pilotl
(warriors 4..29: PMWRegular/PMWGreen etc. for AI — ALSO blanked in the bad file)
```

## Q2 — The DROP (what + when + sha)

1. **Enabler (code)**: `42d4aa9e` (2026-06-07, "editor: fix three save/load crashes in
   mission editor") changed `editor/EditorObjects.cpp Pilot::save` from
   `writeIdString("Profile", info->fileName)` to writing `""` when `info` is null.
   Retail would have crashed (loudly) instead of silently exporting data loss. The
   null-`info` path exists because some editor unit-load path never populates
   `pilot.info` (comment says pak/fit load skips `pilot.load()`; note `Unit::load`
   at EditorObjects.cpp:601 *does* call it, so the null case likely comes from the
   pak-side load or from `Pilot::load` running before `initPilots()` populated
   `s_GoodPilots` — exact path not pinned, see follow-ups).
2. **Data event**: editor session **2026-06-27 16:44** (editor-authoring arc) re-saved
   `mc2srcdata/missions/mc2_01.fit` — diff vs pristine `mc2srcdata-fresh` = camera
   tweaks (PositionX/Y, NewScale, ZoomMax) + **29× `st Profile = ""`**, nothing else.
   `mc2srcdata` is **untracked** (no git history), so blame is by mtime, not sha.
   `mc2_13.fit` (mtime 2026-06-11 09:47) has the same blanking (49 empty Profiles).
   Junk test maps `1kbasicmap/aaaa/dawfae*.fit` also affected.
3. **Deployment**: the blanked mc2_01.fit was copied loose into
   `releases/0.5 testing/mc2-win64-v0.5.0/data/missions/` (alongside dev files
   carverbrain.fit, gaea_peaks_01, mc2_01.pak.bak_*). Loose beats FST → bug is live on
   that lane only. **v0.4 / v0.4d-rc1 / releases-v0.5.0 FSTs are clean** — no loose
   mission fit, packed Profiles intact.
4. **Not the cause**: the gosASSERT→MC2_VERIFY reclassifications (MC2-VERIFY-LIVE-1,
   MISSION-DATA-DEREF-HARDEN-1, mission.cpp:3321-3327) did not drop the data; they
   only let the broken data limp forward (retail would hard-assert on the failed
   `warriors/.fit` open). They are the reason the failure is *silent* at runtime.

## Q3 — Where "Bubba" comes from

- With `Profile=""`: `FullPathFileName(warriorPath, "", ".fit")` = `warriors\.fit` →
  open fails → `MechWarrior::init` returns at `seekBlock("General")`
  (code/warrior.cpp:1184-1186) **before** name/audio load.
- Warrior defaults from `MechWarrior::init(bool)` (warrior.cpp:993-1056): `name=""`,
  `callsign=""`, **`photoIndex=0`**, `radio=NULL`.
- `PilotIcon::setPilot` (code/mechicon.cpp:1730) uses `getPhoto()` → **portrait
  texture index 0 = Bubba's portrait** (`pmwbubba.fit: l PictureIndex = 0`, DescIndex
  42001 = "Bubba" in `data/defs/text/en_us/mc2res_legacy.fit` legacyId 42001). So all
  three pilots render Bubba's face; name text draws the empty/default name. (In the
  *editor* the fallback is explicit: `Pilot::load` not-found → `info=&s_GoodPilots[0]`
  = first pilots.csv row = Bubba.) No code references "bubba" — it arrives via
  photoIndex-0 art + string 42001, exactly as suspected.
- `radio=NULL` (pilotAudio never read) = **no pilot voices**. Consistent.

## Q4 — Fix proposal (smallest correct, no PlayLogistics flip)

**Data restore (the actual fix):**
1. Surgically restore the 29 `st Profile` strings in `mc2srcdata/missions/mc2_01.fit`
   from `A:/Games/mc2-opengl/mc2srcdata-fresh/missions/mc2_01.fit` (keep the Jun-27
   camera edits if wanted; only Profile lines differ otherwise). Same for
   `mc2_13.fit` (49 strings).
2. Delete (or overwrite with restored copy) the loose
   `releases/0.5 testing/mc2-win64-v0.5.0/data/missions/mc2_01.fit` so the pristine
   `mission.fst` copy wins again. (Not done here — recon must not touch installs.)

**Code fix (stop it re-happening):** editor `Pilot::save` must round-trip the raw
Profile string: have `Pilot::load` stash the string it read (`m_profileName`), and
`Pilot::save` write `info ? info->fileName : m_profileName` — never `""`. That keeps
the `42d4aa9e` crash-guard while making save lossless even when pilots.csv matching
fails. One file, ~6 lines, editor-only (no engine/gate impact).

**Optional hardening:** mission.cpp warrior loop — explicit
`MC2_VERIFY(warriorFile[0] != '\0', "[WarriorN] empty Profile — editor data loss?")`
before the open, so the next regression names itself.

**Follow-ups (separate slices):**
- Pin the editor path that leaves `pilot.info` null (pak-load vs initPilots ordering).
- Sweep other installs/lanes for loose `data/missions/*.fit` shadowing FST copies
  (`carverbrain.fit`, `gaea_peaks_01.fit` also loose on 0.5-testing).
- Consider tracking `mc2srcdata/` critical fits or a check-script diffing Profile
  fields against `mc2srcdata-fresh`.

## Evidence trail

- FST extraction: `tools/mc2x_import/fst.py` (deployed FSTs are zlib-repacked; use
  zlib-first fallback, the module tries LZW first and throws).
- Byte-identity: `mc2srcdata/missions/mc2_01.fit` ≡ 0.5-testing loose copy (49795 B,
  mtime 2026-06-27 16:44). Pristine = 50027 B (mc2srcdata-fresh, FST copies).
- Loose-beats-FST: mclib/file.cpp:69 priority comment + :839-853, :1056.
