# new_campaign: what a player does. Intro movie -> main menu -> New Campaign -> Carver V Campaign -> LOAD -> campaign cinema (skipped)
# -> mission 1 loads -> intro camera pan -> first comm video (V1A) -> run well past it with real video decode. No SmokeMode, no MC2_BOOT_TO_*.
# The owner's 2026-11-03 CTD (ABL playVideo on the sim-overlap worker, about engine frame 1600) is exactly this path.
# After LOAD the Game Rules dialog opens (rules-screen-1); Return = ACCEPT keeps the defaults (key, not a click: the
# dialog moved to a centred 16:9 page in game-rules-ui-2, so a canvas coordinate goes stale with its layout).
# Coordinates are 800x600 canvas pixels; screen layout was read off captures (docs/tier1_env_vars.md PLAYER-PATH-1).
#! timeout 420
#! expect (?i)\[VIDEO\] init: opened \S*v1a
#! expect \[FIRST60 v1\]
#! differ pan_a pan_b 0.05
#! forbid smoke mode active
await \[INPUT_SCRIPT v1\] frame=0 script= 60
wait 150
key SPACE
wait 1400
move 300 300
wait 20
click 401 135
wait 150
click 229 248
wait 30
click 554 435
wait 150
key Return
await (?i)\[VIDEO\] init: opened \S*cinema1 120
wait 300
key SPACE
await \[FIRST60 v1\] 120
wait 30
shot {out}/pan_a.tga
wait 240
shot {out}/pan_b.tga
wait 2600
shot {out}/mission_late.tga
mark done
