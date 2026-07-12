# Asset Cook Classification Recon

## Overview

This document maps the input/output flow of the asset cook pipeline, identifies which INI fields signal animation vs. static classification, correlates them to runtime isStaticEligible() guards, and proposes a classification schema to prevent animated props from receiving static (renderOnly: true) overrides.

Root cause: The bulk-generated 1629-entry manifest swept animated props (turrets, popup cannons, radar towers, generators) into the cook pipeline. They received renderOnly: true static GLB overrides with no animation data - frozen, offset, untextured in-game.

Fix strategy: Classify props at cook time by scanning INI files; exclude NODE_ANIMATED_PROP entries from the cook entirely, so regenerated manifests never re-introduce the bug.

---

## Cook Pipeline Map

### Pipeline Flow

cook_all_stock.py scans data/tgl/*.meshdump.json files.
For each meshdump:
1. Extract asset_id = stem of filename
2. Look up INI at data/tgl/asset_id.lower().ini
3. Call collect_animated_ids() to scan all INIs for AnimationNodeId != NONE
4. Skip animated appearances (line 90-93)
5. Stage GLB via trackg_cook.stage() produces staged.json
6. Resolve KTX2 tiles to materials.json
7. Assemble manifest to models.generated.json

All entries validated by registry_resolves() before write.

---

## INI to asset_id Mapping

### Mapping Rule (CONFIRMED)

asset_id.lower() == ini.stem.lower()

Example: artilleryTurret.meshdump.json maps to data/tgl/artilleryturret.ini
If no INI found: Classification = UNKNOWN_UNSAFE

### INI File Structure

Standard FITini format:

[Bounds]
[TGLData] - geometry, shadow, lighting
[TGLDamage] - damaged appearance
[AnimationNode] - KEY SECTION if present
  st AnimationNodeId = "<node_name>"
[Animation:X] - gesture definitions
[WeaponNode] - weapon attachment points

---

## AnimationNodeId Field Location

### Section and Key

Section: [AnimationNode] (optional)
Key: st AnimationNodeId = "<value>"
Meaning: Animation node name housing gesture definitions

### Values

Named node (e.g., turret, TankGTurret): Has animation capability
NONE: Explicitly no animations (rare)
Absence of section: Type is static

### Observed Distribution

From 104 animated props out of ~1500 total:
- 59x "turret" (weapon turrets, artillery)
- 32x "TankGTurret" (vehicle turrets)
- 4x "Mog_Turret"
- 3x "lrmc_turret"
- Singletons: srmc_turret, mrlTurret, hgtTurret, Vedette Turret, Striker_turret, L_SpotRotar

---

## Runtime Gate (isStaticEligible)

### Location

mclib/bdactor.cpp, lines 3059-3097, BldgAppearance::isStaticEligible()

### Disqualifiers Checked

Type-level: !appearType, spinMe, bldgTypeHasAnimations()
Instance-level: drawFlash, destructFX, activity, activity1
Override fix: hasRotationalNode (commit b2d7e6bc)
Gesture state: activeGestureHasAnimation && !overrideStatic

### Key Fields from INI

- spinMe: Type-level spin flag
- rotationalNodeId: Read from AnimationNodeId value
- bdAnimData[]: Animation pointers from [Animation:X] sections
- bldgRenderShape: Non-NULL if model override

---

## Unsafe Examples (NODE_ANIMATED_PROP)

### Characteristic: AnimationNodeId != "NONE"

MUST NOT receive static overrides.

Sample (10 of 104):
1. artilleryturret.ini
2. autocannonpopup.ini
3. clanerlaserturret.ini
4. acv.ini
5. alacorn.ini
6. artillery.ini
7. bulldog.ini
8. staracturret.ini
9. starlrmturret.ini
10. thunderstorm.ini

Cook action: SKIP (no override)

---

## Safe Examples (STATIC_RENDER_ONLY)

### Characteristic: No [AnimationNode] section

Safe to cook as static overrides.

Sample (3 of ~1400):
1. 2civliving.ini
2. bunker.ini
3. forest1.ini

Cook action: EMIT renderOnly: true

---

## Manifest Shape

### Deployed Central (1629 entries)

Location: A:/Games/mc2-opengl/mc2-win64-v0.4/data/model_overrides/models.json

Schema: type=model, class=staticprop|tree, replaces=<class>:<name>, source=cooked/..., renderOnly=true, scale=1.0, fallback=stock, lods=[optional]

### Worktree Source (7 entries)

Location: A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/data/model_overrides/models.json

All 7 are trees (safe).

---

## Proposed Classification Schema

### Four Categories

STATIC_RENDER_ONLY: No [AnimationNode] -> EMIT override
NODE_ANIMATED_PROP: AnimationNodeId != NONE -> SKIP
ROTATIONAL_ONLY_PROP: Code rotationalNodeId, no section -> SKIP (deferred)
UNKNOWN_UNSAFE: Missing INI, ambiguous -> SKIP

### Decision Tree

1. Lookup INI at data/tgl/<asset_id.lower()>.ini
   - NOT FOUND -> UNKNOWN_UNSAFE -> SKIP
   - FOUND -> parse
     2. Check for [AnimationNode]
        - NOT PRESENT -> STATIC_RENDER_ONLY -> EMIT
        - PRESENT -> read AnimationNodeId
           - == NONE -> STATIC_RENDER_ONLY -> EMIT
           - != NONE -> NODE_ANIMATED_PROP -> SKIP

---

## Integration Points

### Cook Pipeline (cook_all_stock.py)

Current (line 78-81): Scans INI for AnimationNodeId
Enhancement (Patch 5): Extend with per-class counters

### Runtime Gate (mclib/bdactor.cpp)

Current: Rotational-node check (commit b2d7e6bc)
Guards against bad manifest entries

### Manifest Promotion (promote_cooked.py)

Current: registry_resolves() validates entries

---

## Summary

Cook-Time Classification:
1. Scan data/tgl/<asset_id.lower()>.ini for [AnimationNode]
2. Absent -> STATIC_RENDER_ONLY (emit)
3. Present != NONE -> NODE_ANIMATED_PROP (skip)
4. Missing INI -> UNKNOWN_UNSAFE (skip)

Runtime Guard (In Place):
- isStaticEligible() checks spinMe, bdAnimData[], rotationalNodeId, gesture state
- Props with rotationalNodeId != NONE never admitted

Next Steps (Patch 5):
1. Extend cook_all_stock.py with per-class counters
2. Regenerate 1629-entry manifest (exclude ~104 animated)
3. Redeploy; verify tier1 smoke passes
4. Monitor for false-negatives
