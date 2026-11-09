# Recovery craft lane state

- Assignment: slot03, claude/recovery-craft, base f269b312d8328aaafccec0b53a15f23ce5775efb.
- Budget: <=150 calls; inspect ~30, implementation ~50, proof ~40, hand-back ~20.
- Existing install logs checked: no support-craft spawn/draw evidence.
- Flow: missiongui_support.cpp beginVtol(147) creates BldgAppearance(karnov);
  missiongui_style.cpp updateVTol presents landing/hover/departure;
  missiongui.cpp drawVTOL submits through bdactor_render.cpp GPU prop batcher.
- Existing fixture: select mech; animate <partId> damaged; support 147 <x> <y>.
- Fixed: preload the support palette before immutable prop geometry finalization
  on mission init and quickload. Late registration had forced Karnov/VTOL to CPU fallback.
- Added SUPPORT_CRAFT diagnostics (request with MC2_DIAG_TAGS=SUPPORT_CRAFT).
- Green: focused doctest (1 case / 8 assertions), static_prop_family contract,
  cxx17 ratchet. Box build blocked by unrelated FSR Windows headers / atomic includes.
- Remaining: final Windows compile, lint after diagnostic tag guards, PC smoke
  and recovery capture. Current receipts/next steps are in
  E:/mc2-archive-2026-10/proofs/recovery-craft/state.md.
