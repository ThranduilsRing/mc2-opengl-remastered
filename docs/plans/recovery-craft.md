# Recovery craft lane state

- Assignment: slot03, claude/recovery-craft, base f269b312d8328aaafccec0b53a15f23ce5775efb.
- Budget: <=150 calls; inspect ~30, implementation ~50, proof ~40, hand-back ~20.
- Existing install logs checked: no support-craft spawn/draw evidence.
- Flow: missiongui_support.cpp beginVtol(147) creates BldgAppearance(karnov);
  missiongui_style.cpp updateVTol presents landing/hover/departure;
  missiongui.cpp drawVTOL submits through bdactor_render.cpp GPU prop batcher.
- Existing fixture: select mech; animate <partId> damaged; support 147 <x> <y>.
- Next: instrument spawn/submission, reproduce on mc2_01, fix demonstrated cause.
- Proof pending: covering regression/contract, box build+unit+smoke-lite,
  final Windows Ninja compile, lint, PC recovery log and capture.
