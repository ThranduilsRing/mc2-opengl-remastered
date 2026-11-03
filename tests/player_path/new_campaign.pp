# new_campaign: main menu -> New Campaign -> mission 1, run past the first comm video (V1A) with real video decode.
# The owner's 2026-11-03 CTD (ABL playVideo on the sim-overlap worker, about frame 1600) is exactly this path.
#! timeout 600
#! expect \[VIDEO\] init: opened \S*V1A
#! forbid smoke mode active
await \[INPUT_SCRIPT v1\] frame=0 script= 60
wait 300
shot {out}/menu.tga
mark done
