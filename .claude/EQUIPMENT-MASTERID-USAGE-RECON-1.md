# EQUIPMENT-MASTERID-USAGE-RECON-1 — 8-bit/ordinal assumptions before widening

Read-only recon before the equipment registry/widen engine slices. Verdict: **facade is safe/additive NOW; widen is deferrable until content >254 live rows.**
★ Line refs read against canonical worktree `.claude/worktrees/unit-profile-seam/`; mc2-repo-intel indexes nifty-mendeleev (~150–200 line drift in mech.cpp) — re-grep anchors before editing.

## The two 8-bit id spaces (the actual cap)
- `InventoryItem::masterID` = **`unsigned char`** (`code/mover.h:415`) — the runtime loadout component id.
- `ComponentStats.weapon.ammoMasterId` = **`unsigned char`** (`mclib/cmponent.h:134`) — SECOND independent id space; widening one without the other breaks ammo >255.
- `MasterComponent::masterID` = `long` (already wide, `mclib/cmponent.h:158`); `masterList` = flat malloc array indexed by ordinal, size = `numComponents` (dynamic, `cmponent.cpp:56/638/665`).

## Key facts
- **255/0xff is a SENTINEL ("empty slot") AND the max id** — load-bearing collision (`mech.cpp:1545`, guards `1560-1564/2158/2335`). Widen must migrate the sentinel (0xFFFF or explicit `bool empty`) or a real id-255 component reads as empty.
- **~180 `masterList[...masterID]` read sites auto-widen** — most locals are already `long` (`mech.cpp:7006`, `mover.cpp:5856+`). Indexing a `long` element by a wider id is safe. These need NO change.
- **No hardcoded `masterList[255]` / `for(<255)`** — loops use `< numComponents`. MECH-INV-IDX-GUARD-1 clamps `masterID >= numComponents → 0xff` (`mech.cpp:1560/1627/1705`) against the DYNAMIC bound, not 255. So the registry can already exceed 255 rows internally; only STORAGE/SERIALIZATION width caps it.
- **`WEAPON_AMMO_OFFSET=65` is DEAD** (`cmponent.h:50`) — zero arithmetic callers; ammo is data-driven via stored `ammoMasterId`/`getWeaponAmmoMasterId()`. The `+65` in schema comments can be dropped.
- **★ Default-off components DO occupy ordinals** — `loadMasterList` reads every CSV row sequentially with NO enabled-filter (`cmponent.cpp:665-672`). Enabling stock's disabled gyro/engine/armor/heatsink rows pushes ids toward/past 255 with zero code change — exactly the cap. The facade can own the enabled/default-off concept the load loop lacks.

## VERDICT: spiderwebbed for full widen, but web is mostly auto-widening reads
Hard parts = the 0xff sentinel collision + two 8-bit fields + `InventoryItem` struct-layout relink + the `.mdf`/`.csv` on-disk 8-bit format (the real external contract). NOT the 180 index sites.

### INVENTORY-MASTERID-WIDEN site list (when needed)
| # | Site | Change | Risk |
|---|---|---|---|
| 1 | `mover.h:415` `InventoryItem::masterID` uchar→uint16 | struct layout → full relink, every `.mdf` load | HIGH |
| 2 | `cmponent.h:134` `weapon.ammoMasterId` (review `:133 ammoType`) | widen (second id space) | HIGH |
| 3 | 0xff/255 sentinel logic (`mech.cpp:1545,1560-64,2158,2335,2386,2455`, all `!=255/==0xff`) | 0xFFFF sentinel or `bool empty` | HIGH (silent "id255=empty" bug) |
| 4 | Save write `mech.cpp:3493,3502,3511` `writeByte` | widen or keep 8-bit + version gate | MED (on-disk) |
| 5 | `.mdf` reads `readIdUChar`/`readUChar` (`gvehicl.cpp:1087/1139/1167`, `mech.cpp:1552/1619/1697/3076/3111/3144`) | wide read | MED |
| 6 | `logistics.cpp:694` `(uchar)`, unify `MoverData.components[50]` (long, `mover.h:1825`) vs `CompressedMech`/`MechData.components[50]` (uchar, `multplyr.h:323`) | widen/unify | MED (MP+logistics) |
| 7 | `infowindow.cpp:630/649/795` `IDS_COMP_ABBR0 + masterID` string index | route names via registry, not ordinal string block | MED |
| 8 | index-guard clamps | no change (use numComponents) — re-verify sentinel interplay | LOW |
| — | ~180 `masterList[..masterID]` reads | NO CHANGE (auto-widen) | LOW |

## ★ FACADE IS PURELY ADDITIVE (E2 can ship now, no widen)
An EQUIPMENT-REGISTRY-FACADE querying by qualified string id + exposing `legacyMasterId` sits BESIDE `masterList` with ZERO edits to `InventoryItem`, save format, or the 180 sites: maps `qualifiedId ⇄ existing legacyMasterId (row ordinal)` as a new lookup layer over the same array. New entries with `legacyMasterId` in 0–254 stay addressable by the unchanged 8-bit path; only entries needing id ≥255 require the widen. The facade can also own the enabled/default-off filter.

## Recommended sequencing (confirmed)
E2 FACADE first (additive, non-breaking, gated — unblocks string-id registry + enabled-flag semantics) → defer E4 WIDEN until content actually exceeds 254 live rows, at which point it's the 8-site list above (dominated by the struct relink + 0xff-sentinel migration, not the 180 reads).
