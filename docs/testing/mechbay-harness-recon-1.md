# MECH-BAY / LOGISTICS HARNESS TARGET RECON-1

**Status:** RECON ONLY (read-only, no code changes). Verdict: **GREEN on icon/atlas
math via a net-positive extraction; DEFER the .mdf inventory guards (fit-parse blocker).**
**Recommended target:** `ICON-ATLAS-HARNESS-1`.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## Recurring bug classes (commit evidence)
- **Icon-atlas grid-cell math**: `f276c50d` (cols = floor not round → MCO 1024px atlas
  phantom column), `9792af03` (pilot rank/photo UV divisor from real texture size).
- **Icon blit OOB**: `a85c1c6b` VEHICLEICON-OOB-GUARD-1 (srcX/srcY past source buffer on
  atlas dim-mismatch; crashed MC2X-TCE m1).
- **.mdf inventory index validation**: `d21e7855` GUARD-1 (masterID ≥ numComponents →
  masterList OOB), `1cf8c7b8` GUARD-2 (spaceData ≥ MAX_MOVER_INVENTORY_ITEMS).
- Pilot-screen flow `4b8604b4`, SMART-LOAD `1d45a876` — control-flow/threading, not harnessable.

## Verdicts
| Candidate | Verdict | Why |
|---|---|---|
| Icon/atlas grid-cell + UV math | **GREEN (via extraction)** | `logisticsmechicon.cpp:154` and `mechlistbox.cpp:529` hold **byte-identical copies** of the cell math (`cols=(long)(fileW/width+0.01f); xIndex=index%cols; …`). Math itself: zero game globals. Extract `code/icon_atlas_cell.h` (free fn), call from both sites → **removes real duplication** (net cleanup, not testability-only churn). C++ harness. |
| Icon blit OOB (`MechIcon`/`VehicleIcon::init`) | **GREEN (via extraction)** | blit inner loop is pure over a `DWORD*` buffer; only `gos_LockImage`/tex-mgr around it are coupled. Extract `blitIconCell(...)` into the same header; harness with synthetic in-memory buffer. |
| .mdf inventory index validation | **DEFER** | guards sit inside `mechFile->readUChar(...)` reading `MasterComponent::numComponents`/`ItemLocationToInvLocation[]`/`body[]` → pulls `FitIniFile:File` + FST + full BattleMech = **identical to the deferred fit-parse verdict**. Extracting just `idInRange()` would be fake-green (the real bug is the read site using the raw byte, not the compare). |
| Python/data-declared source-of-truth | N/A | icon atlases are runtime `.tga`/`.fit`, not a Python manifest. `scripts/verify_campaign_assets.py` (`ee92fe5e`) already covers asset-existence pre-flight — a separate "icons exist" harness would be redundant. |

## Recommendation: `ICON-ATLAS-HARNESS-1` (C++)
Covers (a) cell/UV math + (b) blit-OOB guard behind ONE small behavior-preserving
extraction `code/icon_atlas_cell.h` that **deduplicates the two identical copies** and
centralizes the blit bound-check. Best mech-bay target because:
- math is genuinely pure (no globals, <1s, no GL),
- the extraction **removes real duplication** (net cleanup — strictly better than the
  objmgr extraction which existed only for testability),
- regression-locks three shipped real fixes whose edge cases (MCO 1024px atlas, TCE
  dim-mismatch) are exactly the "hard to hit in 30s tier1" class.

Proposed tests — cell/UV: retail 256/25→10 cols; MC2X 512/25→20; **MCO 1024/40→25 not 26**
(the bug); idx wrap (118 APC on 512); epsilon (256/32→8 not 7); width=0→fallback 10;
idx0→(0,0); UV monotonic. Blit: in-bounds exact copy; srcY≥actualH→row zero-fill no
overread; srcX≥actualW→transparent; negative offset clamp; adjacent non-corruption;
1×1 source; TCE dim-mismatch→blank not crash.

**Defer** the .mdf inventory guards under the standing fit-parse deferral.

## ★ Live-bug note (verify before acting)
`code/mechicon.cpp:322`: `int iconsPerPage = ((int)s_textureMemory->width / (int)unitIconY);`
divides texture **width** by `unitIconY` (cell **height** 38) — almost certainly should
be `height/unitIconY`; line 321 correctly uses `width/unitIconX`. NOT touched by any recent
commit (`git log -G` clean) → likely a latent `ForceGroupIcon` paging bug. Outside harness
scope — flagged for separate confirmation, not claimed fixed.
