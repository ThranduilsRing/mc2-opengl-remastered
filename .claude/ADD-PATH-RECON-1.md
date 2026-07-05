# ADD-PATH-RECON-1 — OVERLAY → ADD ladder (equipment + units)

Read-only recon, branch `claude/unit-profile-seam`. Line refs verified in THIS worktree 2026-07-05; re-grep before coding (slice-preflight mandatory).

## TL;DR

- **The 255 cap does NOT bite today.** `loadMasterList(compFileName, 255, …)` is hardcoded at both callers (`code/mission.cpp:2532`, `code/saveload.cpp:847`); stock `compbas.csv` = exactly 255 data rows (ids 0..254), of which only **130 populated**, **125 "undefined" free ordinals** (39,40,41,44… scattered). ADD = **fill a free ordinal**, not grow the array. No realloc, no E4 widen, until total live rows >254.
- **Unit VARIANT ADD is already pure data** — variants are rows in the per-chassis `objects/<mech>.csv` (row 23 + n*97, `logisticsdata.cpp:508-513`); a mod whole-file-shadowing that CSV adds a variant with ZERO engine change. New CHASSIS is the expensive one (buildings.csv row + new CSV + **Object2.pak fitID entry** = art packaging).
- **Purchase gating = mission `purchase.fit`** (`[Components] ComponentN=<id>`, `[Mechs] MechN=<chassisFile>`, `logisticsdata.cpp:1878-2045`) — whole-file-shadowable by mods today. The E2 `enabled` flag still has **zero consumers**.

---

## 1. EQUIPMENT ADD

### Data structures
- `MasterComponent::masterList` = flat `systemHeap->Malloc(sizeof(MasterComponent) * numComponents)` (`mclib/cmponent.cpp:636-637`). NOT compile-time fixed — sized by the `listSize` param. But both call sites pass literal **255**, and the load loop reads exactly `numComponents` CSV lines (fails with -1 if the file has fewer, `cmponent.cpp:661-671`). So today: array always 255 slots, ids 0..254, every slot init()ed, "undefined" rows get `masterID=-1` + empty name (`initEXCEL`, `cmponent.cpp:109-112`).
- Stock occupancy (converter fixture `tools/unit_converter/tests/fixtures/stock/objects/compbas.csv`): 130 populated / 125 undefined. Highest populated id = 236. (Handoff's "80comps" = converter-exported weapon-ish subset, not row count.)

### Where 255 bites TODAY vs later
| Site | Kind | Bites when |
|---|---|---|
| `mission.cpp:2532` / `saveload.cpp:847` literal 255 | count passed to loadMasterList | only if CSV grows past 255 rows |
| `logisticsdata.cpp:1913` `bool available[255]` + `:1929 for(i<255)` ComponentN loop | fixed purchase array indexed by id | id ≥255 = stack smash — >254 only |
| `updateAvailability` `long componentArray[255]` per-variant (`:2004`) | variant loadout scratch | >255 comps per variant (never) |
| `InventoryItem::masterID` uchar (`mover.h:415`), `ammoMasterId` uchar (`cmponent.h:134`), 0xff sentinel, `.mdf` `readIdUChar`, save `writeByte`, MP `CompressedMech`, `IDS_COMP_ABBR0+masterID` | the E4 8-site list (`.claude/EQUIPMENT-MASTERID-USAGE-RECON-1.md`) | id ≥255 only (255 itself reserved = sentinel, so usable ids top out at **254**) |

**Nothing bites for ADD-by-free-ordinal.** All runtime loops bound by `MasterComponent::numComponents` (dynamic), not literal 255 (MECH-INV-IDX-GUARD-1 clamps against numComponents, `mech.cpp:1737/1813/1896`).

### Consumers: iterate-by-count vs by-MAX
- **By `numComponents` (safe, auto-correct):** `equipment_registry_engine.cpp:49,159`, `fire_executor_registry.cpp:86`, `unitprofile_fit.cpp:549`, `weapon_presentation_proof.cpp:38`, guard clamps `mech.cpp:1737+`, ~180 `masterList[masterID]` read sites (all bounded by stored id).
- **By literal 255 (MAX):** the two `loadMasterList(…,255,…)` callers; `updateAvailability` `available[255]` + `for(i<255)` ComponentN/MechN/PilotN loops; `saveMasterList` symmetric.
- **Parallel second list:** `LogisticsData::initComponents` re-parses `compbas.csv` itself into `COMPONENT_LIST components` (`logisticsdata.cpp:231-280`, `components.Append` :273) — a row must ALSO parse there (needs icon filename column non-`0`, else `return -1` = skipped, `logisticscomponent.cpp:~155`) to appear in mechlab. Two consumers of the same CSV row.

### Minimal ADD mechanics (no E4)
A new `sample:plasma_rifle` becomes live by **claiming a free ordinal** (e.g. 39) two ways:

1. **LEGACY (works TODAY, zero code):** mod overrides `data/objects/compbas.csv` whole-file (magic-ballistic-weapons pattern) filling row 39, + overrides mission `purchase.fit` adding `ComponentN=39`. Hazards: whole-file shadow = conflicts between two mods both touching compbas; name/abbrev come from string table (`cLoadString(32000+id)` / `33000+id`, `cmponent.cpp:63-64,100-101`) → **new id has no string** → blank name in-sim UI; LogisticsComponent name comes from the CSV `stringID` column (`logisticscomponent.cpp:174` `cLoadString(stringID)`) → can point at any existing IDS but not a new custom string.
2. **CLEAN (the slice):** gated post-`loadMasterList` pass (exactly where `overlayGeneratedEquipmentStatsIfEnabled` already sits, `equipment_registry_engine.cpp:152+`): for each registry def with `LegacyMasterId` naming an **undefined** row, populate that `MasterComponent` in place (form/stats/**name+abbrev from the def**, bypassing cLoadString). Registry `qualifiedId⇄legacyMasterId` already exists (E2). `legacyMasterId` auto-assign = "lowest free ordinal" is possible but **DON'T v0** — auto-assignment makes ids unstable across mod sets → breaks saves/MP; require explicit `LegacyMasterId` in the def + conflict report (fits MOD-LOAD-ORDER-CONFLICT-REPORT-1).

**Name/UI hazard (both ways):** in-sim `MasterComponent::name/abbreviation` are fixed char arrays filled at initEXCEL — clean path can write them directly. But `infowindow.cpp:630/649/795` computes `IDS_COMP_ABBR0 + masterID` **independently at display time** → new ids show missing-string garbage until routed through the registry (site 7 of the E4 list — the ONE E4-list site the ADD path touches early, UI-only, no format risk).

## 2. UNIT ADD

### Where the variant list lives
`LogisticsData::initVariants` (`logisticsdata.cpp:347`): reads `art/buildings.csv` row-by-row; type==MECH rows give chassis CSV name (col 1), string nameID (col 3), **fitID (col 5) → packet index into `objects/Object2.pak`** (art/appearance), scale (col 11). Each `objects/<mech>.csv` = 1 chassis (`LogisticsChassis::init`) + N variants at rows 23, 120, 217… (stride 97), each `LogisticsVariant::init`. Variant components read as `readIdUChar("MasterID")`-style uchar refs (`logisticsvariant.cpp:1184-1187`) resolved against `LogisticsData::components`.

Mission-side spawn: mission `.fit` parts carry `VariantNumber` (`mission.cpp:4052`) + object/fitID; `mover->init(parts[i].variantNum)` (`mission.cpp:1642`). Editor palette also enumerates `Buildings.csv` (`editor/EditorInterface.cpp:749`).

### (a) New VARIANT of existing chassis — pure data
- Mechlab/purchase: mod shadows `data/objects/<mech>.csv` with an appended variant block → `initVariants` picks it up automatically → purchasable iff chassis file is in `purchase.fit [Mechs]` AND every component id in the variant is `available[]`. **Zero engine change.** Save-load of mod-purchased variants already hardened (recovery paths `logisticsdata.cpp:67,1174,1725`).
- Mission spawn: editor writes VariantNumber = row index; a mission built against the modded CSV needs the mod active (missing variantNum falls back to 0, `mission.cpp:4054` — graceful).
- CLEAN follow-on: generated unit def declaring a variant → engine synthesizes the LogisticsVariant post-initVariants (registry-side ADD), avoiding whole-CSV shadow conflicts.

### (b) New CHASSIS — data + ART packaging (the hard one)
Needs: buildings.csv row (whole-file shadow — high conflict surface), new `<mech>.csv`, **new fitID packet inside Object2.pak** (appearance/.fit bundle) — PacketFile, only whole-file-shadowable (mod ships a full replacement Object2.pak = huge + max-conflict), OR a pak-append/sideload seam (new engine work: fitID>pak-count → resolve from loose `data/objects/<fitID>.fit`). Plus nameID string, icons, encyclopedia. **Defer**; variant ADD delivers most modder value first.

## 3. PURCHASE / ENABLED WIRE

- Gate TODAY: mission's `purchase.fit` (or `missionInfo->getCurrentPurchaseFile()`): `[Components] AllComponents / ComponentN=<id>` → `available[255]` → `LogisticsComponent::setAvailable`; `[Mechs] MechN=<chassisFileStem>` → variant available iff all its component ids available; `[Pilots]`. RP (`resourcePoints`) only prices the purchase (`purchaseMech` `logisticsdata.cpp:663`), doesn't gate catalog membership. `MC2_PURCHASE_ALL` env = unlock-everything override (`:1876,1927`).
- E2 registry `enabled` flag: **still zero consumers** (confirmed vs MOD-SYSTEM-RECON-1) — the core_unlocks mod rides legacy compbas/purchase overrides.
- **Minimal seam:** one gated hook at end of `updateAvailability` (`~logisticsdata.cpp:2045`): for each registry entry `enabled && hasLegacyMasterId`, set `available[legacyMasterId]=1` + `component.setAvailable(1)` (and optionally mark matching variants). One function, one call site, default-OFF. This is BOTH the core_unlocks clean path and the new-equipment surface — same wire.

## 4. RISK LADDER (gated slices, smallest first; all stock-byte-identical gate-OFF)

### S1 — EQUIP-ENABLE-WIRE-1 (enabled→purchase)
Gate `MC2_EQUIPMENT_ENABLED_WIRE` (register in gate docs). Files: `code/logisticsdata.cpp` (1 hook in updateAvailability), `code/equipment_registry.h` (query helper). Proof: offline doctest on the pure filter; in-mission witness = mechlab shows a stock default-off weapon (registry-enabled) purchasable; gate-OFF diff = none. Hazards: none on save/MP (availability is recomputed, not persisted as ids); watch `available[255]` bound (assert id<255).

### S2 — EQUIP-ADD-FILL-1 (new equipment on a free ordinal)
Gate `MC2_EQUIPMENT_ADD` (+ requires SOURCE=generated). Files: `code/equipment_registry_engine.cpp` (fill-undefined-row pass after overlay), `code/equipment_registry.h`, converter/modlint (accept `sample:` defs w/ explicit LegacyMasterId + free-ordinal check), conflict report (dup ordinal = ERROR, never silent). Proof: offline = modlint fixture mod w/ `sample:plasma_rifle@39`; in-mission = S1 wire surfaces it in mechlab, buy it, fire it (fire path reads masterList row like any stock id — E7 executor already keyed off form/stats). Hazards: **UI strings** (`IDS_COMP_ABBR0+masterID` garbage → S3), LogisticsComponent icon columns mandatory (icon atlas coords must be valid or row skipped), save games store masterID 39 = fine while mod active / dangling if removed (conflict report should warn), MP: both peers need same mod (id-by-ordinal already the MP contract), 0xff untouched (ids ≤254).

### S3 — EQUIP-UI-NAME-REGISTRY-1 (kill ordinal string-block for new ids)
Gate shared w/ S2. Files: `gui/infowindow.cpp:630/649/795` (+ any IDS_COMP siblings), route name/abbrev via registry-with-legacy-fallback; `cmponent.cpp` name fill from def. Proof: offline unit test of resolver; witness = new weapon shows real name in info window + mechlab. Hazards: this is E4-list site 7 done early — UI-only, zero format risk; keep legacy path byte-identical when id has no registry entry.

### S4 — UNIT-VARIANT-ADD-1
Gate `MC2_UNIT_VARIANT_ADD`. Files: `code/logisticsdata.cpp` (post-initVariants synthesis from generated unit defs) OR data-only v0 (example mod shadowing one `<mech>.csv` + purchase.fit — zero code, ship as EXAMPLE-MODS follow-on first). Proof: offline = modlint variant fixture; witness = new variant in mech bay list, purchasable, drops into mission via logistics. Hazards: variant component refs are uchar (fine ≤254); save recovery for mod variants already exists (`:1174`); VariantNumber in mission .fits = row-index coupling (document: appended variants only, never insert).

### S5 — CHASSIS-ADD-RECON-1 (later, recon first)
The Object2.pak fitID sideload seam (loose-file fallback for fitID ≥ pak count), buildings.csv additive registry, nameID strings, editor palette. Do NOT attempt inside this arc; separate recon.

## 5. E4-FORCING FLAGS

- **Nothing above forces E4.** All new ids live in the 125 free ordinals ≤254; 255 stays sentinel.
- E4 triggers to watch: (a) total populated rows >254 (mod ecosystem growth — conflict report should print remaining-free-ordinal count so we see it coming); (b) any temptation to auto-assign ids ≥ current file size / grow the CSV past 255 rows (loadMasterList literal 255 + `available[255]` + purchase loop all break together); (c) new AMMO types are double-exposure (`ammoMasterId` uchar too); (d) MP `CompressedMech` uchar components — new ids ≤254 fine, widen not. Auto-assignment itself (even ≤254) is save/MP-unstable — keep ids explicit + reserved per mod until a persistent id-allocation map exists.
