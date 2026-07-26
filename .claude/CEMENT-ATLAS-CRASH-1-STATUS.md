# CEMENT-ATLAS-CRASH-1 — STATUS: FIXED

Branch `claude/nifty-mendeleev`. Fix commit **`06ffbcf4`**
(`fix(terrain): bound getTextureNameID against numTypes`).

## Defect

Intermittent READ access violation at mission load in `BuildCementCatalogAtlas`,
`GameOS/gameos/gos_terrain_indirect.cpp:2074`. Reported by QUIET-DEFAULT-BUILD-1,
which reproduced the identical stack on the pre-change exe `01bfcc055ff2`.

## Root cause — a slot-index / type-index mixup

The `[CEMENT_CELL_TONE v1]` census (RUNWAY-APRON-TONE-2, `608dea4db`) calls:

```cpp
ttNames->getTextureNameID(slot)      // slot from cementSlots[]
```

`slot` is a terrain **texture slot** — an index into `textures[]`, valid over
`[0, getNextAvailableSlot())`. But `getTextureNameID` indexes **`types[]`**, a
different and much smaller heap block of `numTypes` entries
(`mclib/terrtxm.cpp:223`), and it had **no bounds check**:

```cpp
long getTextureNameID( long id ) const { return types[id].nameId; }   // before
```

Cement slots are `createTransition()` handles, allocated from `nextAvailable`,
and run far above `numTypes`.

### Faulting index (measured, from the fixed exe, `DEPLOY_FINGERPRINT OK`)

```
[CEMENT_CELL_TONE v1] mission=mc2_01 cell=36 slot=789 nameId=-1 band_lum=86.5 dark=1
```

`MaxTerrainTypes = 255` (`textures.fit:5`) versus **`slot = 789`** — a **534-entry
overrun** past the end of the `types[]` allocation, inside the `tileRAMHeap`
arena. Whether it faulted depended on whether `types + 789*sizeof(MC_TerrainType)`
landed on a committed page that run. Hence intermittent and heap-layout dependent.

Only **one** cell (`cell=36`) is classified `dark=1` in mc2_01, so there was
exactly one OOB read per load at one fixed offset — which is precisely why it hit
roughly 1-in-5 rather than every time, and why 3/3 isolated runs missed it.

### Reachable on a default launch — yes

- `gate=0` — `MC2_CEMENT_ALPHA_ATLAS` is OFF by default, `alpha_admitted=0`.
  The faulting slot comes from the **solid** pass (`solid_cells=65`).
- The census branch is `if (dark || traceOn())` — the dark cell triggers it with
  no trace flag set.
- The `getTextureNameID` call is a plain statement **outside** the `MC2_DIAGF`
  macro, so the new quiet-default diagnostics did not suppress the read.

Default gates, default mission, default build. The user would have hit this.

## Prime suspect `1779bf1d` — INNOCENT, left in place

The brief nominated the orphaned CEMENT-UNIFORM-2 WIP. It is not the cause:

- Its whole effect is one `continue` in the solid slot→layer loop, itself already
  bounds-guarded (`k < (int)cementSlotIsAlpha.size()`).
- `cementSlotIsAlpha` is pushed in **lockstep** with `cementSlots` at both push
  sites — it cannot desynchronise, and it is never used to index another vector.
- With the alpha gate OFF (the default) the vector is all-zero and the `continue`
  never fires at all.

No revert, so no patch preserved and no `c3804cfe` dependency to untangle. The
cement work can resume from `1779bf1d` as-is.

`c3804cfe` (MC2_CEMENT_ALPHA_ATLAS) is also not the cause — it is default-OFF and
contributes zero cells on a default launch.

## Fix

1. **`mclib/terrtxm.h`** — bound `getTextureNameID`: `gosASSERT(id >= 0 && id <
   numTypes)` plus a `return -1` for out-of-range. This is the **guard for the
   class**, placed at the single chokepoint rather than at one call site.
   `gosASSERT` is `((void)0)` outside `_ARMOR`, so it is loud in debug builds and
   inert in shipping, while `return -1` keeps shipping safe. Every sibling
   accessor (`terrainMapRGB`, `terrainPriority`) already asserted this exact
   invariant; `getTextureNameID` was the lone unguarded outlier. This also covers
   three editor callers that walk ids unbounded (`SelectTerrainTypeDlg.cpp:70`,
   `EditorInterface.cpp:1234`, which can pass `i - groupCount` < 0).
2. **`gos_terrain_indirect.cpp`** — stop asking a type-index accessor for a
   texture slot's name: qualify on `slot < getNumTypes()`, report `-1` otherwise.

Swept for the same slot/type mixup elsewhere: `terrainMapRGB` / `terrainPriority`
have no callers outside `terrtxm`, so this accessor was the only reachable one.

## Verification — exe `06ffbcf4b09f`, `dirty=0`

- **10/10 consecutive `mc2_01` loads clean**, `DEPLOY_FINGERPRINT OK` on all 10.
- **tier1 5/5 PASS**, `DEPLOY_FINGERPRINT OK` on all 5 missions.
- **Harnesses 16/16 PASS** (`build_contract_harnesses.py --run`, 12/12 built).
- **Contracts 67/67 PASS** (`check-contracts.sh`).
- Clean-fingerprint build, `deploy_payload.py --require-clean`, md5-verified
  (`6b94ebcb37f2770bcbee73e622e54cda` build == deploy), PDB check OK.

Confidence rests primarily on the **static proof** (789 vs 255 on the exact
reported line), not on the run count. Honest framing of the runs alone: at the
observed ~1-in-5 rate, 15 clean mc2_01 loads is only ~3.5% likely by chance —
corroborating, not conclusive on its own.

## Process note — a stale-exe trap worth recording

The first verification pass (10/10 + tier1 5/5) was **invalid** and nearly
reported as success. `scripts/deploy_payload.py` was pointed at
`A:/Games/mc2-opengl/mc2-win64-0.4c` while `--deploy 0.4c` resolves via
`scripts/smoke_lib/deploy_lease.py:55` to `A:/Games/mc2-opengl/mc2-win64-**v**0.4c`
— two directories differing by one `v`. The smoke ran the stale
`01bfcc055ff2`, i.e. the very exe the crash was reported against. Caught only by
reading the `[DEPLOY_FINGERPRINT] MISMATCH` line in the tier1 tail. Everything
above was re-run after redeploying to the leased path.

Side effect to be aware of: `A:/Games/mc2-opengl/mc2-win64-0.4c` (no `v`, not in
the lease map, apparently a stray) now holds a `06ffbcf4` payload.
