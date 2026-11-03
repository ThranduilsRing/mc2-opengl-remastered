# load_game: new campaign to mission 1, ESC -> QUICK SAVE, ESC -> QUICK LOAD, run ~1500 frames after the reload.
# (The main-menu Load Game dialog needs a campaign .fit fixture; the in-mission quick save/load round-trip is used instead.)
#! timeout 300
#! expect (?i)\[VIDEO\] init: opened \S*v1a
#! expect \[FIRST60 v1\]
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
await \[VIDEO\] init: opened \S*cinema1 120
wait 300
key SPACE
await \[FIRST60 v1\] 120
await \[VIDEO v1\] event=play_requested name=V1A 120
wait 700
key ESCAPE
wait 90
click 700 154
wait 120
key ESCAPE
wait 90
shot {out}/before_load.tga
click 700 180
wait 600
shot {out}/after_load.tga
wait 1500
mark done
