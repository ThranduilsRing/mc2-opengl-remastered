# projectZ Callsite Inventory

**Date:** 2026-04-25  
**Status:** commit 2 markers placed — no logic changes  
**Spec:** `docs/superpowers/specs/2026-04-25-projectz-containment-design.md`  
**Branch:** `projectz-hunting`

---

## Discovery method

```
grep -rn "projectZ\|inverseProjectZ" code/ mclib/ GameOS/ shaders/ \
  --include="*.cpp" --include="*.h"
```

Paths searched: `code/`, `mclib/`  
Excluded from marker count (not callsites):
- `mclib/camera.h` — contains the method definition (not a call)
- `mclib/camera.cpp:914` — inside a `/* ... */` commented-out block
- `mclib/gamecam.cpp:155,168` — prose comments referencing the method name

**Total callsites tagged: 99**  
**Total `inverseProjectZ` callsites: 4** (all in `code/gametacmap.cpp`)

---

## Summary table

| id | file:line | category |
|----|-----------|----------|
| terrain_quad_vert0_admit | mclib/quad.cpp:525 | BoolAdmission |
| terrain_quad_vert1_admit | mclib/quad.cpp:593 | BoolAdmission |
| terrain_quad_vert2_admit | mclib/quad.cpp:661 | BoolAdmission |
| terrain_quad_vert3_admit | mclib/quad.cpp:729 | BoolAdmission |
| terrain_cpu_vert_admit | mclib/terrain.cpp:1098 | BoolAdmission |
| gameobj_visibility_admit | code/gameobj.cpp:2089 | BoolAdmission |
| light_terrain_active_test | mclib/camera.cpp:1752 | LightingShadow |
| light_spot_point_active_test | mclib/camera.cpp:1779 | LightingShadow |
| cloud_vertex_screen | mclib/clouds.cpp:211 | Both |
| crater_corner0 | mclib/crater.cpp:322 | Both |
| crater_corner1 | mclib/crater.cpp:324 | Both |
| crater_corner2 | mclib/crater.cpp:326 | Both |
| crater_corner3 | mclib/crater.cpp:328 | Both |
| weather_raindrop_top | code/weather.cpp:488 | Both |
| weather_raindrop_bot | code/weather.cpp:495 | Both |
| picking_closest_cell_center | mclib/camera.cpp:865 | SelectionPicking |
| picking_closest_vertex_fallback | mclib/camera.cpp:936 | SelectionPicking |
| picking_terrain_rect_select | mclib/terrain.cpp:1491 | SelectionPicking |
| picking_drag_select_origin | code/missiongui.cpp:3607 | SelectionPicking |
| tacmap_inverse_corner0 | code/gametacmap.cpp:225 | InverseProjectionPair |
| tacmap_inverse_corner1 | code/gametacmap.cpp:232 | InverseProjectionPair |
| tacmap_inverse_corner2 | code/gametacmap.cpp:239 | InverseProjectionPair |
| tacmap_inverse_corner3 | code/gametacmap.cpp:246 | InverseProjectionPair |
| actor_vfx_top_depth | code/actor.cpp:291 | ScreenXYOracle |
| actor_vfx_screen_pos | code/actor.cpp:295 | ScreenXYOracle |
| artlry_position_screen | code/artlry.cpp:1221 | ScreenXYOracle |
| artlry_iface_screen | code/artlry.cpp:1224 | ScreenXYOracle |
| artlry_iface_float_help | code/artlry.cpp:1395 | ScreenXYOracle |
| gui_drag_box_origin | code/missiongui.cpp:2921 | ScreenXYOracle |
| gui_mover_rotation_pos | code/missiongui.cpp:5635 | ScreenXYOracle |
| gui_cam_rotation_goal | code/missiongui.cpp:5639 | ScreenXYOracle |
| gvehicl_path_step_start | code/gvehicl.cpp:3990 | ScreenXYOracle |
| gvehicl_path_step_end | code/gvehicl.cpp:3992 | ScreenXYOracle |
| mech_path_step_start | code/mech.cpp:6520 | ScreenXYOracle |
| mech_path_step_end | code/mech.cpp:6522 | ScreenXYOracle |
| warrior_tac_order_preview_start | code/warrior.cpp:7424 | ScreenXYOracle |
| warrior_tac_order_preview_end | code/warrior.cpp:7426 | ScreenXYOracle |
| warrior_tac_queue_segment_start | code/warrior.cpp:7452 | ScreenXYOracle |
| warrior_tac_queue_segment_end | code/warrior.cpp:7454 | ScreenXYOracle |
| weaponbolt_laser_tip | code/weaponbolt.cpp:1639 | ScreenXYOracle |
| weaponbolt_laser_v0 | code/weaponbolt.cpp:1657 | ScreenXYOracle |
| weaponbolt_laser_v1 | code/weaponbolt.cpp:1727 | ScreenXYOracle |
| weaponbolt_laser_v2 | code/weaponbolt.cpp:1709 | ScreenXYOracle |
| weaponbolt_laser_v3 | code/weaponbolt.cpp:1675 | ScreenXYOracle |
| weaponbolt_beam_v0 | code/weaponbolt.cpp:1923 | ScreenXYOracle |
| weaponbolt_beam_v1 | code/weaponbolt.cpp:1941 | ScreenXYOracle |
| weaponbolt_beam_v2 | code/weaponbolt.cpp:1951 | ScreenXYOracle |
| weaponbolt_beam_v3 | code/weaponbolt.cpp:1961 | ScreenXYOracle |
| weaponbolt_beam_side0 | code/weaponbolt.cpp:1979 | ScreenXYOracle |
| weaponbolt_beam_side1 | code/weaponbolt.cpp:1997 | ScreenXYOracle |
| weaponbolt_beam_side2 | code/weaponbolt.cpp:2007 | ScreenXYOracle |
| weaponbolt_beam_side3 | code/weaponbolt.cpp:2017 | ScreenXYOracle |
| appear_select_box_ul | mclib/appear.cpp:115 | ScreenXYOracle |
| appear_select_box_lr | mclib/appear.cpp:119 | ScreenXYOracle |
| bdactor_screen_pos_a | mclib/bdactor.cpp:1143 | ScreenXYOracle |
| bdactor_box_rect_a | mclib/bdactor.cpp:1285 | ScreenXYOracle |
| bdactor_box_wire_a | mclib/bdactor.cpp:1821 | ScreenXYOracle |
| bdactor_screen_pos_b | mclib/bdactor.cpp:3699 | ScreenXYOracle |
| bdactor_box_rect_b | mclib/bdactor.cpp:3788 | ScreenXYOracle |
| bdactor_box_wire_b | mclib/bdactor.cpp:4049 | ScreenXYOracle |
| genactor_screen_pos | mclib/genactor.cpp:590 | ScreenXYOracle |
| genactor_box_rect | mclib/genactor.cpp:720 | ScreenXYOracle |
| genactor_box_wire | mclib/genactor.cpp:946 | ScreenXYOracle |
| gvactor_screen_pos | mclib/gvactor.cpp:1560 | ScreenXYOracle |
| gvactor_box_rect | mclib/gvactor.cpp:1707 | ScreenXYOracle |
| mech3d_screen_pos | mclib/mech3d.cpp:2081 | ScreenXYOracle |
| mech3d_box_rect | mclib/mech3d.cpp:2235 | ScreenXYOracle |
| mech3d_box_wire | mclib/mech3d.cpp:2702 | ScreenXYOracle |
| mine_cell_corner0 | mclib/quad.cpp:3564 | ScreenXYOracle |
| mine_cell_corner1 | mclib/quad.cpp:3569 | ScreenXYOracle |
| mine_cell_corner2 | mclib/quad.cpp:3574 | ScreenXYOracle |
| mine_cell_corner3 | mclib/quad.cpp:3579 | ScreenXYOracle |
| tgl_shadow_vertex_a | mclib/tgl.cpp:3018 | ScreenXYOracle |
| tgl_shadow_vertex_b | mclib/tgl.cpp:3158 | ScreenXYOracle |
| debug_cursor_crosshair | code/missiongui.cpp:3004 | DebugOnly |
| debug_los_line_a0 | code/team.cpp:1015 | DebugOnly |
| debug_los_line_a1 | code/team.cpp:1017 | DebugOnly |
| debug_los_line_b0 | code/team.cpp:1041 | DebugOnly |
| debug_los_line_b1 | code/team.cpp:1043 | DebugOnly |
| debug_los_line_c0 | code/team.cpp:1210 | DebugOnly |
| debug_los_line_c1 | code/team.cpp:1212 | DebugOnly |
| debug_los_line_d0 | code/team.cpp:1233 | DebugOnly |
| debug_los_line_d1 | code/team.cpp:1235 | DebugOnly |
| debug_cell_passability_0 | mclib/quad.cpp:2945 | DebugOnly |
| debug_cell_passability_1 | mclib/quad.cpp:2950 | DebugOnly |
| debug_cell_passability_2 | mclib/quad.cpp:2955 | DebugOnly |
| debug_cell_passability_3 | mclib/quad.cpp:2960 | DebugOnly |
| debug_door_outline_0 | mclib/quad.cpp:3070 | DebugOnly |
| debug_door_outline_1 | mclib/quad.cpp:3075 | DebugOnly |
| debug_door_outline_2 | mclib/quad.cpp:3080 | DebugOnly |
| debug_door_outline_3 | mclib/quad.cpp:3085 | DebugOnly |
| debug_los_cell_height_0 | mclib/quad.cpp:3167 | DebugOnly |
| debug_los_cell_height_1 | mclib/quad.cpp:3172 | DebugOnly |
| debug_los_cell_height_2 | mclib/quad.cpp:3177 | DebugOnly |
| debug_los_cell_height_3 | mclib/quad.cpp:3182 | DebugOnly |
| debug_cell_state_0 | mclib/quad.cpp:3444 | DebugOnly |
| debug_cell_state_1 | mclib/quad.cpp:3449 | DebugOnly |
| debug_cell_state_2 | mclib/quad.cpp:3454 | DebugOnly |
| debug_cell_state_3 | mclib/quad.cpp:3459 | DebugOnly |

**Category counts:** BoolAdmission=6, LightingShadow=2, Both=7, SelectionPicking=4, InverseProjectionPair=4, ScreenXYOracle=51, DebugOnly=25. Total=99.

---

## Full detail — BoolAdmission (6 sites)

These are the wedge-class hazard sites. `projectZ` uses `fabs(rhw)` internally, so a vertex that is behind the camera can pass the screen-rect test and set `clipData=true`. Any code that branches on the bool and then consumes screen.z/w for depth tracking is susceptible to phantom depth values.

### terrain_quad_vert0_admit … terrain_quad_vert3_admit
**File:** `mclib/quad.cpp:525,593,661,729`  
**Function:** `TerrainQuad::draw()`  
**Code pattern (vert0 shown; vert1-3 are structurally identical):**
```cpp
bool clipData = false;
// [PROJECTZ:BoolAdmission id=terrain_quad_vert0_admit]
clipData = eye->projectZ(vertex3D, screenPos);
bool isVisible = Terrain::IsGameSelectTerrainPosition(vertex3D) || drawTerrainGrid;
if (!isVisible) { clipData = false; vertices[0]->hazeFactor = 1.0f; }

vertices[0]->clipInfo = clipData;
vertices[0]->wx = screenPos.x;  vertices[0]->wy = screenPos.y;
vertices[0]->wz = screenPos.z;  vertices[0]->ww = screenPos.w;

if (clipData) {
    if (screenPos.z < leastZ) leastZ = screenPos.z;
    if (screenPos.z > mostZ)  mostZ  = screenPos.z;
    if (screenPos.w < leastW) { leastW = screenPos.w; leastWY = screenPos.y; }
    if (screenPos.w > mostW) { mostW  = screenPos.w; mostWY  = screenPos.y; }
}
```
**Bool consumed:** yes — `clipInfo` written, gates per-vertex leastZ/mostZ/leastW/mostW tracking.  
**Screen consumed:** screen.x/y/z/w all stored into `vertices[n]->wx/wy/wz/ww`.  
**Wedge-class hazard:** YES. If a vertex is behind the camera, `fabs(rhw)` lets it pass; `clipInfo=true` and the wrong screenPos.z/w values propagate into depth-range tracking for the entire quad. This is the primary hazard the containment spec targets.  
**Notes:** `isVisible` from `IsGameSelectTerrainPosition` can override `clipData=false` post-hoc, but only in the negative direction — it cannot suppress a false-positive behind-camera pass.

### terrain_cpu_vert_admit
**File:** `mclib/terrain.cpp:1098`  
**Function:** CPU terrain vertex processing loop  
**Code:**
```cpp
bool inView = false;
Stuff::Vector4D screenPos(-10000.0f, -10000.0f, -10000.0f, -10000.0f);
if (onScreen) {
    Stuff::Vector3D vertex3D(currentVertex->vx, currentVertex->vy, currentVertex->pVertex->elevation);
    // [PROJECTZ:BoolAdmission id=terrain_cpu_vert_admit]
    inView = eye->projectZ(vertex3D, screenPos);
    currentVertex->px = screenPos.x;  currentVertex->py = screenPos.y;
    currentVertex->pz = screenPos.z;  currentVertex->pw = screenPos.w;
}
```
**Bool consumed:** yes — `inView` gates downstream `clipInfo` and block activation in terrain.cpp.  
**Screen consumed:** screen.x/y/z/w stored into `currentVertex->px/py/pz/pw`.  
**Wedge-class hazard:** YES — same behind-camera `fabs(rhw)` path as quad.cpp; a false-positive `inView=true` stores bad depth into `pz/pw`, which feeds the GPU terrain vertex buffer.  
**Notes:** Outer `onScreen` guard (haze/distance check) provides a coarse pre-filter, but does not eliminate the W-sign problem.

### gameobj_visibility_admit
**File:** `code/gameobj.cpp:2089`  
**Function:** `GameObject::updateScreenPos()`  
**Code:**
```cpp
long isVisible = 0;
if (eye) {
    Stuff::Vector3D objPosition = position;
    // [PROJECTZ:BoolAdmission id=gameobj_visibility_admit]
    isVisible = eye->projectZ(objPosition, screenPos);
}
if (isVisible) { windowsVisible = turn; return true; }
```
**Bool consumed:** yes — `isVisible` gates `windowsVisible` (object visibility turn-stamp used by cull/lifecycle system).  
**Screen consumed:** `screenPos` (member variable) written; used by callers for HUD position.  
**Wedge-class hazard:** YES — a behind-camera object can receive a live `windowsVisible` turn-stamp, keeping it in the active set and potentially triggering HUD drawing at garbage screen coordinates.  
**Notes:** `windowsVisible` feeds `canBeSeen()`, which gates the cull/lifecycle chain (see cull_gates_are_load_bearing.md). A false-positive here keeps objects alive when they should be culled.

---

## Full detail — LightingShadow (2 sites)

### light_terrain_active_test
**File:** `mclib/camera.cpp:1752`  
**Function:** `Camera::updateLights()` — terrain light branch  
**Code:**
```cpp
if (light->lightType == TG_LIGHT_TERRAIN) {
    Stuff::Vector4D dummy;
    // [PROJECTZ:LightingShadow id=light_terrain_active_test]
    light->active = projectZ(light->position, dummy);
    if (light->active) {
        if (terrainLightCalc) activeLights[numActiveLights++] = light;
        terrainLights[numTerrainLights++] = light;
    }
    continue;
}
```
**Bool consumed:** yes — `light->active` gates whether the light enters `activeLights[]` and `terrainLights[]`.  
**Screen consumed:** none (`dummy` discarded).  
**Hazard:** If a terrain light is behind the camera, `fabs(rhw)` passes and `light->active=true`. The light then participates in terrain lighting calculations even though it is geometrically behind the viewer. This is a correctness issue, not a crash risk.  
**Notes:** Comment says "ON screen matters not" for terrain lights, implying the original intent was a different activation criterion. The projectZ screen-rect test is being (mis)used as a cheap world-space visibility proxy here.

### light_spot_point_active_test
**File:** `mclib/camera.cpp:1779`  
**Function:** `Camera::updateLights()` — spot/point light branch  
**Code:**
```cpp
if (light->lightType >= TG_LIGHT_POINT && light->lightType < TG_LIGHT_TERRAIN) {
    Stuff::Vector4D dummy;
    // [PROJECTZ:LightingShadow id=light_spot_point_active_test]
    light->active = projectZ(light->position, dummy);
    activeLights[numActiveLights++] = light;
    terrainLights[numTerrainLights++] = light;
}
```
**Bool consumed:** yes — `light->active` set; light unconditionally appended to both arrays regardless.  
**Screen consumed:** none (`dummy` discarded).  
**Hazard:** Unlike terrain lights, spot/point lights are *always* appended to the active arrays regardless of the bool. `projectZ` is called for the side-effect of setting `light->active` as metadata, but does not actually gate the arrays. The `fabs(rhw)` hazard exists in the `active` flag but is harmless in this code path since the arrays are populated unconditionally. Still worth tracking for correctness (callers of `light->active` elsewhere).

---

## Full detail — Both (7 sites)

These sites consume both the bool return and screen.x/y (and sometimes screen.z).

### cloud_vertex_screen
**File:** `mclib/clouds.cpp:211`  
**Function:** cloud vertex projection loop  
**Code:**
```cpp
Stuff::Vector3D vertex3D(cloudVertices[i].vx, cloudVertices[i].vy,
                         CLOUD_ALTITUDE + eye->getCameraOrigin().y);
Stuff::Vector4D screenPos;
// [PROJECTZ:Both id=cloud_vertex_screen]
bool inView = eye->projectZ(vertex3D, screenPos);
cloudVertices[i].px = screenPos.x;  cloudVertices[i].py = screenPos.y;
cloudVertices[i].pz = screenPos.z;  cloudVertices[i].pw = screenPos.w;
cloudVertices[i].clipInfo = onScreen && inView;
```
**Bool consumed:** yes — `clipInfo` stores `onScreen && inView`, gates cloud triangle submission.  
**Screen consumed:** screen.x/y/z/w all stored into vertex.  
**Hazard:** same behind-camera false-positive as terrain quad: cloud vertex at wrong depth can corrupt cloud geometry depth.

### crater_corner0 … crater_corner3
**File:** `mclib/crater.cpp:322,324,326,328`  
**Function:** crater rendering  
**Code:**
```cpp
// [PROJECTZ:Both id=crater_corner0]
bool onScreen1 = eye->projectZ(currCrater->position[0], currCrater->screenPos[0]);
// [PROJECTZ:Both id=crater_corner1]
bool onScreen2 = eye->projectZ(currCrater->position[1], currCrater->screenPos[1]);
// ... corner2, corner3 ...
```
**Bool consumed:** yes — `onScreen1..4` gate crater rendering (checked downstream before submitting geometry).  
**Screen consumed:** `currCrater->screenPos[0..3]` (all four) stored as member state, used for UV/vertex generation.  
**Hazard:** four-corner projection; if any corner is behind the camera, `fabs(rhw)` passes that corner and the stored `screenPos` is garbage. The crater quad could render with inverted/bogus coordinates for that corner.

### weather_raindrop_top / weather_raindrop_bot
**File:** `code/weather.cpp:488,495`  
**Function:** `WeatherManager::render()` rain loop  
**Code:**
```cpp
// [PROJECTZ:Both id=weather_raindrop_top]
bool onScreen = eye->projectZ(rainDrops[i].position, screen1);
if (onScreen) {
    // ...
    // [PROJECTZ:Both id=weather_raindrop_bot]
    onScreen = eye->projectZ(botPos, screen2);
    if (onScreen) {
        unsigned char amb = ambientFactor * (1.0f - screen1.z);
        // draw line from screen1 to screen2
    }
}
```
**Bool consumed:** yes — double-gated; both top and bot must be on-screen to draw.  
**Screen consumed:** screen1.x/y/z/w and screen2.x/y all consumed. Notably, `screen1.z` feeds the `amb` ambient factor calculation — depth influences raindrop brightness.  
**Hazard:** A behind-camera raindrop top that passes `fabs(rhw)` would have `screen1.z` outside [0,1], producing a negative or >1 ambient factor and potentially wrapping the `unsigned char` cast.

---

## Full detail — SelectionPicking (4 sites)

### picking_closest_cell_center
**File:** `mclib/camera.cpp:865`  
**Function:** tile-to-world picking (inside-tile path)  
**Code:**
```cpp
// [PROJECTZ:SelectionPicking id=picking_closest_cell_center]
eye->projectZ(point, cellCenter);
dx = (tvx - float2long(cellCenter.x));
dy = (tvy - float2long(cellCenter.y));
dist = dx * dx + dy * dy;
```
**Bool consumed:** no — return value discarded.  
**Screen consumed:** screen.x/y used for pixel-distance to cursor.  
**Notes:** Low-stakes picking; operates on terrain vertices already known to be near the click point. Not in a hot path.

### picking_closest_vertex_fallback
**File:** `mclib/camera.cpp:936`  
**Function:** tile-to-world picking (off-map fallback, inner loop)  
**Code:**
```cpp
// [PROJECTZ:SelectionPicking id=picking_closest_vertex_fallback]
projectZ(tmpWorld, tmpScreen);
float tmpDis = (tmpScreen.x - screenPos.x)*(tmpScreen.x - screenPos.x)
             + (tmpScreen.y - screenPos.y)*(tmpScreen.y - screenPos.y);
```
**Bool consumed:** no.  
**Screen consumed:** screen.x/y for distance computation.  
**Notes:** Called `this->projectZ` (not `eye->`) — Camera is calling its own method. This fallback iterates ALL terrain vertices (`realVerticesMapSide²`) to find the closest, which is O(n²) for a missed-tile click. Performance concern independent of projectZ correctness. The commented-out block directly above (~line 907-925) was a sparser version iterating only edge vertices.

### picking_terrain_rect_select
**File:** `mclib/terrain.cpp:1491`  
**Function:** `Terrain::selectVerticesInRect()`  
**Code:**
```cpp
// [PROJECTZ:SelectionPicking id=picking_terrain_rect_select]
eye->projectZ(worldPos, screenPos);
if (screenPos.x >= xMin && screenPos.x <= xMax &&
    screenPos.y >= yMin && screenPos.y <= yMax) {
    mapData->selectVertex(j, i, true, bToggle);
}
```
**Bool consumed:** no — manual screen-rect test replicated in caller.  
**Screen consumed:** screen.x/y for rect containment test.  
**Notes:** This manually replicates the screen-rect test that `projectZ` already performs internally. The bool return is discarded; behind-camera vertices could pass `fabs(rhw)` and have their screen.x/y compared against the selection rect with garbage coordinates. A vertex directly behind camera maps to screenPos near the projection center, potentially causing spurious selection.

### picking_drag_select_origin
**File:** `code/missiongui.cpp:3607`  
**Function:** drag-select loop over all movers  
**Code:**
```cpp
// [PROJECTZ:SelectionPicking id=picking_drag_select_origin]
eye->projectZ(dragStart, screenPos);
screenStart.x = screenPos.x;
screenStart.y = screenPos.y;
// ... moverInRect(i, screenStart, dragEnd) ...
```
**Bool consumed:** no.  
**Screen consumed:** screen.x/y fed to `moverInRect`.  
**Notes:** Called once per mover inside a loop, but `dragStart` is the *same world point* every iteration — the projection result is loop-invariant. This is a minor redundancy (the call could be hoisted before the loop). If `dragStart` is somehow behind the camera, `fabs(rhw)` would place `screenStart` at bogus coordinates and break drag selection.

---

## Full detail — InverseProjectionPair (4 sites)

### tacmap_inverse_corner0 … tacmap_inverse_corner3
**File:** `code/gametacmap.cpp:225,232,239,246`  
**Function:** tactical map viewport corner unprojection  
**Code pattern:**
```cpp
Stuff::Vector4D nScreen(/* viewport corner NDC coords */);
Stuff::Vector3D world;
// [PROJECTZ:InverseProjectionPair id=tacmap_inverse_corner0]
eye->inverseProjectZ(nScreen, world);
// world is then stored as the tacmap viewport corner in world space
```
Four calls for the four viewport corners (top-left, top-right, bottom-left, bottom-right).  
**Bool consumed:** n/a — `inverseProjectZ` returns void.  
**Screen consumed:** n/a — input is screen, output is world.  
**Notes:** These are the only `inverseProjectZ` callsites in the codebase. They are not affected by the `fabs(rhw)` hazard (inverse path, different code). The correctness risk is that the tactical map viewport calculation depends on the camera projection being accurate; if the projection matrix is set incorrectly, the tacmap corners will be wrong. No containment action needed here.

---

## Pattern — recalcBounds actor hierarchy (ScreenXYOracle, 14 sites)

**Pattern:** `recalcBounds()`-style function calls `projectZ` to compute screen position and/or 8-corner bounding box for selection rectangle and wireframe overlay. The bool return is not consumed in any case — screen.x/y drive the screen-space rect. screen.z/w are not consumed.

The pattern appears in four actor types in the same inheritance hierarchy (`BldActor → GenericAppearance → GVAppearance → Mech3DAppearance`). Each type has up to three sites: `screen_pos` (single-point center), `box_rect` (8-corner loop for selection rectangle), `box_wire` (8-corner loop for wireframe).

**Notes (low-stakes draw):** All ScreenXYOracle — screen.xy only needed, bool discarded. No depth tracking, no cull decisions, no lifecycle effects.

| id | file:line | notes |
|----|-----------|-------|
| bdactor_screen_pos_a | mclib/bdactor.cpp:1143 | first recalcBounds path; comment: "ALWAYS need to do this or select is YAYA" |
| bdactor_box_rect_a | mclib/bdactor.cpp:1285 | 8-corner loop, first occurrence |
| bdactor_box_wire_a | mclib/bdactor.cpp:1821 | 8-corner wireframe, first occurrence |
| bdactor_screen_pos_b | mclib/bdactor.cpp:3699 | second recalcBounds path; comment: "But now inView is correct." |
| bdactor_box_rect_b | mclib/bdactor.cpp:3788 | 8-corner loop, second occurrence |
| bdactor_box_wire_b | mclib/bdactor.cpp:4049 | 8-corner wireframe, second occurrence |
| genactor_screen_pos | mclib/genactor.cpp:590 | single center point |
| genactor_box_rect | mclib/genactor.cpp:720 | 8-corner loop |
| genactor_box_wire | mclib/genactor.cpp:946 | 8-corner wireframe |
| gvactor_screen_pos | mclib/gvactor.cpp:1560 | single center point |
| gvactor_box_rect | mclib/gvactor.cpp:1707 | 8-corner loop |
| mech3d_screen_pos | mclib/mech3d.cpp:2081 | single center point |
| mech3d_box_rect | mclib/mech3d.cpp:2235 | 8-corner loop |
| mech3d_box_wire | mclib/mech3d.cpp:2702 | 8-corner wireframe |

---

## Pattern — HUD path-line and drag-box overlay (ScreenXYOracle, 10 sites)

**Pattern:** Project start/end world points to screen to draw HUD overlay lines (movement path, tactical order queue, drag selection box). Bool discarded; screen.x/y consumed for line endpoint vertices.

**Notes (low-stakes draw):** No depth consumed, no cull decisions.

| id | file:line | notes |
|----|-----------|-------|
| gui_drag_box_origin | code/missiongui.cpp:2921 | drag selection box start corner |
| warrior_tac_order_preview_start | code/warrior.cpp:7424 | tac order preview line start |
| warrior_tac_order_preview_end | code/warrior.cpp:7426 | tac order preview line end |
| warrior_tac_queue_segment_start | code/warrior.cpp:7452 | queued order segment start |
| warrior_tac_queue_segment_end | code/warrior.cpp:7454 | queued order segment end |
| gvehicl_path_step_start | code/gvehicl.cpp:3990 | GV path step line start |
| gvehicl_path_step_end | code/gvehicl.cpp:3992 | GV path step line end |
| mech_path_step_start | code/mech.cpp:6520 | mech path step line start |
| mech_path_step_end | code/mech.cpp:6522 | mech path step line end |
| appear_select_box_ul | mclib/appear.cpp:115 | selection box upper-left |
| appear_select_box_lr | mclib/appear.cpp:119 | selection box lower-right |

---

## Pattern — WeaponBolt render vertices (ScreenXYOracle, 13 sites)

**Pattern:** WeaponBolt projects laser and beam geometry vertices to screen coordinates for the `gos_VERTEX` render pipeline. Bool discarded; screen.x/y consumed. Two structurally distinct render blocks in the same file:

- **Laser block** (`weaponbolt_laser_*`, 5 sites, lines 1638–1727): vertices use `.z = 0.1f` (fixed depth). Screen.z is not consumed from projectZ output.
- **Beam block** (`weaponbolt_beam_*`, 8 sites, lines 1922–2017): vertices use `.z = screenPos.z` (depth from projection). Screen.z IS consumed.

The beam block's use of `screenPos.z` for vertex depth is notable: if a beam vertex is behind the camera, `fabs(rhw)` produces a garbage `screenPos.z`, which is then written as the vertex depth. This is a `Both`-adjacent site marked as `ScreenXYOracle` because the bool is discarded; however, the screen.z consumption means commit 3 should examine this block.

| id | file:line | notes |
|----|-----------|-------|
| weaponbolt_laser_tip | code/weaponbolt.cpp:1639 | laser tip; z=0.1f fixed |
| weaponbolt_laser_v0 | code/weaponbolt.cpp:1657 | laser vert 0; z=0.1f fixed |
| weaponbolt_laser_v1 | code/weaponbolt.cpp:1727 | laser vert 1; z=0.1f fixed |
| weaponbolt_laser_v2 | code/weaponbolt.cpp:1709 | laser vert 2; z=0.1f fixed |
| weaponbolt_laser_v3 | code/weaponbolt.cpp:1675 | laser vert 3; z=0.1f fixed |
| weaponbolt_beam_v0 | code/weaponbolt.cpp:1923 | beam vert 0; z=screenPos.z |
| weaponbolt_beam_v1 | code/weaponbolt.cpp:1941 | beam vert 1; z=screenPos.z |
| weaponbolt_beam_v2 | code/weaponbolt.cpp:1951 | beam vert 2; z=screenPos.z |
| weaponbolt_beam_v3 | code/weaponbolt.cpp:1961 | beam vert 3; z=screenPos.z |
| weaponbolt_beam_side0 | code/weaponbolt.cpp:1979 | beam side vert 0; z=screenPos.z |
| weaponbolt_beam_side1 | code/weaponbolt.cpp:1997 | beam side vert 1; z=screenPos.z |
| weaponbolt_beam_side2 | code/weaponbolt.cpp:2007 | beam side vert 2; z=screenPos.z |
| weaponbolt_beam_side3 | code/weaponbolt.cpp:2017 | beam side vert 3; z=screenPos.z |

---

## Pattern — miscellaneous ScreenXYOracle (11 sites)

Remaining ScreenXYOracle sites that don't fit the larger groups above.

| id | file:line | notes |
|----|-----------|-------|
| actor_vfx_top_depth | code/actor.cpp:291 | only screen.z consumed (topZ depth); bool discarded |
| actor_vfx_screen_pos | code/actor.cpp:295 | screen.x/y for bounding rect; bool discarded |
| artlry_position_screen | code/artlry.cpp:1221 | artillery object screen position |
| artlry_iface_screen | code/artlry.cpp:1224 | artillery interface screen position; shares same bool-discard + manual rect test pattern as terrain.cpp picking (see observations) |
| artlry_iface_float_help | code/artlry.cpp:1395 | floating help text screen position |
| gui_mover_rotation_pos | code/missiongui.cpp:5635 | screen.xy fed into slope/rotation calculation — screen.xy drives further math, not just draw |
| gui_cam_rotation_goal | code/missiongui.cpp:5639 | screen.xy fed into slope/rotation calculation |
| mine_cell_corner0 | mclib/quad.cpp:3564 | mine cell corner 0; game-visible (not debug); screen.z/w consumed for render depth |
| mine_cell_corner1 | mclib/quad.cpp:3569 | mine cell corner 1 |
| mine_cell_corner2 | mclib/quad.cpp:3574 | mine cell corner 2 |
| mine_cell_corner3 | mclib/quad.cpp:3579 | mine cell corner 3 |
| tgl_shadow_vertex_a | mclib/tgl.cpp:3018 | legacy blob-shadow vertex A; bool discarded, screen.x/y fed into transformedPosition |
| tgl_shadow_vertex_b | mclib/tgl.cpp:3158 | legacy blob-shadow vertex B; same pattern |

---

## Pattern — quad.cpp debug visualizers (DebugOnly, 16 sites)

**Pattern:** Four `TerrainQuad` debug-only functions each project 4 cell corners for coloured line drawing. All are inside `if (drawTerrainGrid)` guards (set only via debug UI / LAB builds). Screen.x/y consumed for line endpoints; bool discarded.

| id | file:line | function | notes |
|----|-----------|----------|-------|
| debug_cell_passability_0 | mclib/quad.cpp:2945 | drawLine | cell passability, XP_RED |
| debug_cell_passability_1 | mclib/quad.cpp:2950 | drawLine | |
| debug_cell_passability_2 | mclib/quad.cpp:2955 | drawLine | |
| debug_cell_passability_3 | mclib/quad.cpp:2960 | drawLine | |
| debug_door_outline_0 | mclib/quad.cpp:3070 | drawLine | door outline |
| debug_door_outline_1 | mclib/quad.cpp:3075 | drawLine | |
| debug_door_outline_2 | mclib/quad.cpp:3080 | drawLine | |
| debug_door_outline_3 | mclib/quad.cpp:3085 | drawLine | |
| debug_los_cell_height_0 | mclib/quad.cpp:3167 | drawLOSLine | LOS height visualizer |
| debug_los_cell_height_1 | mclib/quad.cpp:3172 | drawLOSLine | |
| debug_los_cell_height_2 | mclib/quad.cpp:3177 | drawLOSLine | |
| debug_los_cell_height_3 | mclib/quad.cpp:3182 | drawLOSLine | |
| debug_cell_state_0 | mclib/quad.cpp:3444 | drawDebugCellLine | cell state debug |
| debug_cell_state_1 | mclib/quad.cpp:3449 | drawDebugCellLine | |
| debug_cell_state_2 | mclib/quad.cpp:3454 | drawDebugCellLine | |
| debug_cell_state_3 | mclib/quad.cpp:3459 | drawDebugCellLine | |

---

## Pattern — team.cpp LOS debug + dead code (DebugOnly, 9 sites)

**Pattern:** 8 LOS visualizer sites inside `#ifdef LAB_ONLY` / `if (drawTerrainGrid)` in `team.cpp`, plus 1 dead-code site in `missiongui.cpp`.

`LAB_ONLY` is defined only in debug builds (`CMAKE_CXX_FLAGS_DEBUG -DLAB_ONLY`). These sites never compile in RelWithDebInfo or Release.

`DRAW_CURSOR_CROSSHAIRS` is never defined anywhere in the codebase (grep confirms the `#ifdef` itself is the only occurrence). The enclosed code is permanently dead.

| id | file:line | guard | notes |
|----|-----------|-------|-------|
| debug_los_line_a0 | code/team.cpp:1015 | LAB_ONLY | SD_RED segment, startHeight check |
| debug_los_line_a1 | code/team.cpp:1017 | LAB_ONLY | |
| debug_los_line_b0 | code/team.cpp:1041 | LAB_ONLY | SD_GREEN, end-of-first-block |
| debug_los_line_b1 | code/team.cpp:1043 | LAB_ONLY | |
| debug_los_line_c0 | code/team.cpp:1210 | LAB_ONLY | SD_RED, isTree branch |
| debug_los_line_c1 | code/team.cpp:1212 | LAB_ONLY | |
| debug_los_line_d0 | code/team.cpp:1233 | LAB_ONLY | SD_GREEN, end-of-function |
| debug_los_line_d1 | code/team.cpp:1235 | LAB_ONLY | |
| debug_cursor_crosshair | code/missiongui.cpp:3004 | DRAW_CURSOR_CROSSHAIRS (never defined) | permanently dead code |

---

## Observations

**O1 — artlry.cpp duplicate screen-rect test (`artlry_iface_screen`).**  
`artlry.cpp` around lines 1220-1222 manually tests `screenPos.x`/`screenPos.y` against the screen bounds after calling `projectZ`, replicating the internal screen-rect test that `projectZ` already performs. The bool return is discarded and the test is re-done by hand. This is a correctness concern: the manual test uses different boundary values than `projectZ`'s internal pixel-rectangle, so the two can disagree. The same pattern appears in `picking_terrain_rect_select` (terrain.cpp) and is a candidate for consolidation in commit 3.

**O2 — weaponbolt.cpp beam block consumes `screenPos.z` (`weaponbolt_beam_v*`, `weaponbolt_beam_side*`).**  
The 8 beam-block sites write `vertex.z = screenPos.z` directly from the projectZ output. This is structurally closer to `Both` than pure `ScreenXYOracle` (bool is discarded, but depth is consumed). If a beam vertex is behind the camera, the garbage `screenPos.z` from `fabs(rhw)` becomes the vertex depth in the render pipeline. Flagged for commit 3 review.

**O3 — missiongui.cpp rotation math (`gui_mover_rotation_pos`, `gui_cam_rotation_goal`).**  
These two sites (lines 5635, 5639) feed `screen.xy` into slope/rotation angle calculations rather than direct draw calls. The screen coordinates drive further arithmetic. If the projected point is behind the camera and `fabs(rhw)` produces screen coordinates far outside the viewport, the rotation calculation gets garbage input. Low practical risk (camera rotation is user-initiated), but architecturally different from the other "just draw here" ScreenXYOracle sites.

**O4 — camera.cpp:914 commented-out callsite found by discovery grep.**  
The discovery grep finds `projectZ` inside a `/* */` block at camera.cpp:914. This was the original off-map vertex-search implementation (iterating only edge vertices). It was replaced by the active loop at line 927+ (tagged `picking_closest_vertex_fallback`) which iterates ALL vertices. The replacement is O(n²) worse. No action needed for containment, but noted as a performance regression.

**O5 — weather.cpp `screen1.z` feeds ambient factor (`weather_raindrop_top`).**  
`unsigned char amb = ambientFactor * (1.0f - screen1.z)` at weather.cpp:498. If `screen1.z` is outside [0,1] due to the `fabs(rhw)` hazard (behind-camera raindrop), the multiplication produces a value outside [0,1], which truncates or wraps when cast to `unsigned char`. The `Both` classification is correct; the depth consumption makes this slightly more hazardous than plain HUD rendering.

**O6 — light_spot_point_active_test unconditional array append.**  
Unlike terrain lights (which check `light->active` before appending), spot/point lights are appended to `activeLights[]`/`terrainLights[]` unconditionally. The `projectZ` call only sets `light->active` as metadata; the call has no actual gating effect on the array population. Whether this is intentional ("spot/point lights always active") or an oversight is unclear from context.

---

## Open questions

**Q1 — Should `picking_terrain_rect_select` be upgraded to `BoolAdmission`?**  
The bool is discarded, but a behind-camera vertex with garbage `screenPos.x/y` could fall inside the selection rect and trigger a spurious `selectVertex`. The impact is bounded (worst case: wrong terrain vertex selected), but it's structurally identical to the BoolAdmission hazard. Keeping it `SelectionPicking` for now; revisit in commit 3.

**Q2 — `picking_drag_select_origin` is loop-invariant.**  
`dragStart` is the same world point each iteration of the mover loop. The `projectZ` call could be hoisted before the loop with no semantic change. Is there a reason it's inside the loop? (Possibly historical / copy-paste.)

**Q3 — `light_terrain_active_test` comment says "ON screen matters not".**  
If screen-visibility is genuinely irrelevant for terrain light activation, this call should use a different criterion (e.g., distance-to-camera). The current use of `projectZ` as a "is the light anywhere near the camera frustum" proxy is imprecise and susceptible to the W-sign hazard. Fix scope: commit 3 or later.

**Q4 — Are there `projectZ` calls in `GameOS/` or `shaders/`?**  
Discovery grep searched `code/` and `mclib/` only. `GameOS/` contains the renderer core; `shaders/` contain GLSL. Neither is expected to call the C++ `projectZ` method, but this was not verified. If any exist, they are untagged.
