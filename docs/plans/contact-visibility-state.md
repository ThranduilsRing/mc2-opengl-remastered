# Contact visibility lane state

Assignment: A:/Games/mc2-slot05, claude/contact-visibility; requested base
f269b312d8328aaafccec0b53a15f23ce5775efb. Fast-forwarded to pick-fix tip
cb52de4e5 (includes Linux link fix); its report still lists proof pending.

Implemented: one home-team contact table, resolved once per object at the
quiescent frame extraction, copied into RenderView and Appearance. Consumers:
- code/mech_update.cpp and gvehicl_update.cpp: Full/Sensor/SensorText/None modes.
- mclib/mech3d_render.cpp and gvactor_render.cpp: native body submit, bars,
  brackets/text, sensor shapes, detached mech arms, vehicle shadow admission.
- code/missiongui_update.cpp: hover identification and hidden target rejection.
- code/gametacmap.cpp and controlgui_render.cpp: legacy/snapshot minimap blips.
- code/infowindow.cpp and missiongui_style.cpp: clicked health/details admission,
  including contact loss at extraction; objmgr_pick.cpp, missiongui_input.cpp and
  gamecam.cpp: pick admission, orders target and camera tracking eligibility.
- Mech shadows consume the same admitted native batch (gos_mech_batcher_flush.cpp
  snapshotFrame, vulkan_shadow_mech_casters.cpp); vehicle shadow feed is current-frame.

Rules: none = hidden; qualities 1/2 = blip; 3 = blip + weight-class tag;
4 = blip + identified tag; visual = body + bar/bracket eligibility + tag + shadow.
Own, disabled/destroyed, ShowMovers and defeated-player overrides retain stock reveal.
ContactStatus already resolves visual range/LOS; sensor quality never grants a model.
Minimap retains sensor-only ECM/null-signature/shutdown suppression.

Defects addressed: CONTACT_NONE could leave the old sensorLevel unchanged; native
opaque submits did not implement legacy alpha fades, separating first-frame HUD
from body admission. The snapshot now sets both sensorLevel and alpha every frame,
including frames without a sim tick. Snapshot minimap no longer re-queries contacts.
Main-view CPU/GPU lagged cull/HZB remains unchanged; model and appearance bars share
the enclosing visibility/cull block. Selected/targeted flags still control bar display.
HZB admits only static props, not movers. No render-cull root cause established;
no PC capture or performance claims.

Proof: contact table/consumer doctests (including quality x visual-range/LOS),
covering shader/layout contracts, box build/unit/smoke-lite/headless mc2_03
determinism, Windows Ninja compile and lint. The final completion state, exact
proved-at SHA and receipt paths are recorded outside git at
E:/mc2-archive-2026-10/proofs/contact-visibility/STATE.md to preserve tip identity.
No PC install created. Retain this note and proof receipts; remove lane scratch.
