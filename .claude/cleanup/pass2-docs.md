# Pass 2: MC2_* default-state claims (report only)

Method: registry defaults from `repo_query.py env --all`, cross-checked against the GL reader code. Scanned docs/ and .claude/*.md + CLAUDE.md, excluding docs/archive. Skipped history, plans, and specs.

| file:line | gate | doc says | truth | confidence |
|---|---|---|---|---|
| docs/tier1_env_vars.md:69 | MC2_IMGUI | "Default **OFF**" | ON. GuiRuntime/GuiRuntime.cpp:88 reads `!v \|\| v[0] != '0'`. Registry JSON also says off, so the registry is wrong here. | high |
| docs/engine-closure-audit.md:145 | MC2_SHADOW_ENABLE | "off ... shipped opt-in" | ON. gos_mech_batcher.cpp:1167-1169 and gos_static_prop_batcher.cpp (kill-switch `=0`). | high |
| docs/engine-closure-audit.md:201 | MC2_SHADOW_ENABLE | "`=0` (default)" | ON (same readers) | high |
| .claude/LIGHTING-HEADROOM-AUDIT.md:410 | MC2_SHORELINE_LEGACY_SUPPRESS | "suppression is opt-in via ..." | ON. gos_terrain_lod_chunk.cpp:4157 is `!(v && v[0]=='0')`. | high |

Count: 4 rows (high 4, med 0).

Notes:
- docs/tier1_env_vars.md:68 already corrects MC2_SHADOW_ENABLE to default ON, but the two rows above in engine-closure-audit.md were not updated.
- Not flagged: umbrella sub-gate wording (VK_SHARED_GBUFFER, VK_VFX), history lines, and plan/spec "will flip" lines.
- MC2_SMOKE_FIXED_TIMESTEP "default ON in the runner" (docs/superpowers/strategy/baseline-a-capture-runbook.md:58) was not verified. It is a script default, not an engine reader.
- MC2_STATIC_PROP_LEGACY_DISPATCH is live (reader gos_static_prop_batcher.cpp:228). No retired-gate claim found in live docs. MC2_DRAW_PACKET_STATIC_PROP_V6 is correctly marked retired in docs/tier1_env_vars.md:571.
