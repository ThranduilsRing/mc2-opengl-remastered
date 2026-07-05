# 10/10 Bespoke BattleMech Tactics Engine — Vision

> Not "UE5 with mechs." Focused engine that does one thing better than UE by default:
> large tactical battles with readable machines, terrain, command intent, damage, sensors, salvage, and modded campaigns.

---

## Six Pillars

### 1. Rock-Solid Modernization Substrate
Boring but mandatory foundation. Without this, every big feature becomes another archaeology session.

```
deploy coherence
S9 visual capture / Baseline A
render pass timing
GlStateGuard / explicit render-state ownership
shadow stability
S8 package/install banked
feature-flag registry
machine-readable done reports
```

→ "heroic modernization" becomes "maintainable engine"

---

### 2. Modern Tactical Command Layer
Player-facing jump. Engine must feel like a commander's tool, not a camera over old MC2.

```
Formation Line
Assault Line
Fallback Line
Overwatch Sector
Focus-fire paint
Command wheel
Role-aware squad spacing
Reserve / hold-fire / fire-at-will states
Sensor/contact confidence layer
Threat arrows / weapon envelope overlay
Fast-forward / tactical pause
```

F6/F7/F8 tacmap, squad cards, rings, Formation Line = the start.

10/10 = player expresses intent fast:
- hold this ridge
- screen this flank
- advance by bounds
- focus that mech
- fall back through this line
- overwatch this cone

Not just "move here, attack that."

---

### 3. BattleMech-Specific Simulation Depth
The soul.

```
location-based armor/internal damage
critical hits
heat
ammo explosions
knockdowns / stability
weapon recycle and accuracy curves
line-of-sight and partial cover
sensor quality / ECM / stealth / jamming
pilot skill and panic/morale
limb destruction
salvage state
repair/refit consequences
```

MC2 has some of this DNA. Upgrade = make it legible and data-driven.

**Key rule:** Player must understand why a mech lived, died, missed, overheated, or lost a limb.

---

### 4. Terrain as Gameplay, Not Backdrop
Biggest bespoke-engine opportunity.

```
height advantage
ridge peeking
forest concealment
water heat sinking
roads/speed
rubble/slowing
bridges/destruction
building cover/collapse
line-of-sight slicing
sensor occlusion
mines
choke points
drop zones
artillery lanes
```

Engine already has terrain/chunk/static-prop modernization. Next goal = tactical semantics:

**terrain object → cover/sensor/movement/combat meaning**

Not just draw meaning.

---

### 5. Renderer Tuned for Tactical Readability
Do not chase cinematic UE5 first. Chase readability under command pressure.

```
stable shadows
clear silhouettes
range/LOS overlays
sensor fog
thermal/night vision
weapon arcs
damage decals / smoke / fires
readable VFX
consistent depth compositing
clean tactical zoom LOD
unit outlines / selection rings
terrain contrast controls
```

Render recon train directly serves this. Shadows, transparency, VFX, pass state, visual capture — all matter because unreadable tactical rendering kills the game.

**10/10 BattleMech rendering:**
- pretty enough close-up
- crystal clear zoomed-out
- never lies tactically

---

### 6. Mod/Campaign/Content Pipeline
What makes it live.

```
mc2mod pack/install/uninstall
asset manifest validator
registry index
asset viewer
preview rendering
cook pipeline
campaign/mission validation
data ownership registry
diffable mods
safe load order
local workshop-like package layout
```

S8 turns modding from "folder ritual" into tool contract.

10/10 = modder can add new chassis/weapons/variants/missions/factions/campaigns/VFX/materials without corrupting base install or needing engine archaeology.

---

## Road to 10/10

### Phase 1 — Engine Dependable (~7.5/10)
```
deploy coherence
S9 visual capture / Baseline A
render pass timing
transparency depth-write fixes
shadow stability
GlStateGuard slice 1
```

### Phase 2 — Command Feels Modern (~8.2/10)
```
Formation Line
command wheel
assault/fallback/overwatch sectors
tactical pause / fast-forward
threat/contact overlays
squad card polish
```

### Phase 3 — BattleMechs Deep and Legible (~9/10)
```
damage model UI
heat/crit/ammo/salvage readability
LOS/cover/sensor confidence
weapon envelope overlays
AI behavior states
pilot/morale layer
```

### Phase 4 — Content Creation Easy (~9.5/10)
```
asset viewer
cook pipeline
mission validator
mech/weapon/chassis editors
mod packaging polish
visual regression lab
preview rendering
```

### Phase 5 — Signature Features (10/10)
Things that feel native to this engine, not bolted on:
```
drawn formation/assault/fallback lines
sensor-war tactical layer
true BattleMech damage/heat/salvage loop
terrain-aware AI
destructible tactical terrain
instant modded campaign packaging
large readable battles at tactical zoom
```

---

## Recommended Backlog (ordered)

```
 1. S9 visual capture / Baseline A
 2. deploy coherence
 3. transparency state fixes
 4. Shadow Stability v1
 5. Formation Line v1
 6. command wheel
 7. Overwatch Sector
 8. LOS/sensor-confidence overlay
 9. damage/heat/crit readable UI layer
10. mc2mod polish + campaign package validation
11. asset viewer preview/cook loop
12. terrain tactical semantics
13. AI command states
14. destructible buildings/cover pass
15. full visual regression lab
```

---

## Identity

> 10/10 does not mean copy UE5.
>
> 10/10 means:
> - best BattleMech command layer
> - best tactical readability
> - deep mech damage/heat/salvage
> - terrain that matters
> - modding that is safe
> - renderer that never lies
> - tools that let you build campaigns fast

Spine is right. Next leap = turn modernization infrastructure into command-layer and BattleMech-specific gameplay power.
