<!-- doc-status: current | reviewed:2026-10-11 -->
# Next-Gen RTS Engine Vision

**Date:** 2026-05-26  
**Status:** Vision / Thesis — pre-roadmap  
**Context:** Post-"catch up to modern engines" framing; what comes after static props / materials / pipelines / views are stable.

---

## Core thesis

Most modern engines ask:
> *How do we render more realistic worlds?*

A next-gen RTS engine asks:
> *How do we render the battlefield as an intelligible command system?*

**A future RTS renderer should not just render the world. It should render the commander's understanding of the world.**

RTS has constraints AAA first/third-person renderers don't optimize for:
- Extreme zoom range
- Many units, dense tactical state
- Strategic readability
- Persistent battlefield state
- Fog / sensors / ECM
- Command clarity

These constraints are advantages — they give room to invent, not catch up.

---

## 1. Perception-first rendering

Four semantic layers, each a first-class render product (not "normal view plus overlays"):

| Frame | Content |
|---|---|
| Visual | Normal battlefield appearance |
| Sensor | LOS, radar confidence, ECM/ECCM distortion, heat signatures |
| Command | Weapon envelopes, safe paths, cover, threat gradients |
| Damage | Armor/heat/critical state projected onto silhouettes/icons |

**ViewMode** becomes an engine service, not a UI hack.

---

## 2. Semantic visibility — importance-weighted LOD

Standard cull: frustum → occlusion → distance → LOD.

Semantic cull: frustum → occlusion → **importance** → presentation.

```
importance =
  selected unit weight
  enemy contact certainty
  weapon threat to force
  sensor relevance
  recent damage
  commander focus area
  objective proximity
  line-of-sight role
```

A far enemy artillery that can hit you renders more meaningfully than a nearby decorative building. Not graphics LOD — tactical semantic LOD.

Proposed type:
```cpp
struct SemanticVisibilityResult {
    RenderImportance importance;
    ContactConfidence confidence;
    TacticalRole      role;
    PresentationLOD   presentation;
};
```

Renderer asks: *not just how far, but how much does the player need to understand this?*

---

## 3. Zoom-continuous presentation ladder

RTS cameras span cinematic to map-command range. Icons popping in is a hack. Better: renderer-owned continuous ladder.

```
Close:     full mech model, damage decals, weapon effects
Mid:       simplified model, readable silhouette, team highlights
Far:       tactical icon, facing wedge, range ring, heat strip
Very far:  grouped lance/company marker, contact confidence, threat area
```

```cpp
enum class PresentationLOD {
    Model3D,
    SilhouetteBillboard,
    TacticalIcon,
    AggregatedFormation,
    StrategicGlyph
};
```

BattleTech icons can encode: heat, armor, sensor lock, ECM, jump jets, shutdown, weapon range, facing, lance membership. Richer than any current RTS icon system.

---

## 4. Tactical field rendering

Instead of drawing individual overlays one by one — generate renderable fields as GPU textures, composite like terrain layers:

```
threat field
sensor confidence field
movement cost field
cover field
LOS field
heat danger field
artillery danger field
```

Fields update from simulation data. Renderer composites spatially.

Novel axis: **fields respond to selected unit / lance / command mode.**  
Map becomes an interactive tactical instrument answering:
- Where is it safe?
- Where can I see?
- Where can I shoot?
- Where am I exposed?

---

## 5. Persistent battlefield memory

Renderer shows temporal state, not just current:

```
last known enemy position
movement trail (confidence-faded)
recent artillery impact marks
heat/scorch trails
sensor ghost fading
destroyed unit debris as tactical landmarks
```

Fog-of-war as temporal uncertainty gradient (not binary visible/hidden):

```
visible now → seen 5s ago → sensor ghost → probable contact → stale contact → unknown
```

---

## 6. Commander attention model

Render according to attention, not camera alone.

Inputs:
```
selected units
current order preview
combat alerts
threats to selected force
objectives
recent damage events
sensor events
```

Output:
```
increase contrast / annotation for relevant objects
de-emphasize irrelevant clutter
auto-surface tactical facts near focus
```

Per-unit-role adaptation:
```
selected LRM carrier:   show indirect-fire arcs, spotter links, sensor confidence
selected brawler:       show cover, heat risk, short-range threat cones
selected scout:         show sensor fields, ECM shadows, probable contacts
```

---

## 7. RenderWorld as query engine

Once RenderWorld is stable, it answers:

```cpp
// What rendered this pixel?
// What tactical entity owns it?
// What material/LOD/pipeline produced it?
// What sensor state applies?
// What command affordances exist?
```

Enables a **battlefield debugger** for modders and players:
- Why can I see this?
- Why can't I shoot this?
- Why did this icon aggregate?
- Why is this contact uncertain?
- Why did this building draw as impostor?

Major differentiator for an open RTS engine.

---

## 8. GPU-driven tactical overlays

Push overlays from CPU/UI into the render pipeline via SSBOs:

```
unit data SSBO
terrain heightfield
sensor mask texture
threat map texture
selection state buffer
```

GPU draws:
```
LOS cones
range rings
threat gradients
path preview
contact uncertainty halos
```

Scales with unit count. Overlays visually integrated with world, not screen-space sprites.

---

## 9. BattleTech-specific lane

BattleTech has unusually rich tactical state most RTS engines don't surface cleanly:

```
heat / armor facings / internal structure / critical hits
ECM/ECCM / sensor locks / jump capability / shutdown state
ammo risk / weapon range bands / minimum ranges
indirect fire / LOS vs sensor contact
pilot skill / morale
```

North-star feature:
> At every zoom level, the player can understand:
> who can see me, who can hit me, who I can hit,
> how much danger I am in,
> and what the enemy probably knows.

---

## 10. Practical sequence (post-catch-up)

After static props / materials / pipelines / views are stable:

```
1. ViewMode system
   Visual / Tactical / Sensor / Thermal / Command

2. TacticalVisibilityService
   LOS + sensor + confidence + contact age

3. Zoom presentation ladder
   model → silhouette → icon → aggregate

4. Field renderer
   threat/sensor/cover/movement fields as GPU textures

5. Semantic LOD
   importance-weighted rendering + icon detail

6. Object-ID powered inspector
   explain any pixel/entity/material/visibility decision
```

---

*This document is pre-implementation vision. No tasks here — translate to plan docs when a slice is ready to execute.*
