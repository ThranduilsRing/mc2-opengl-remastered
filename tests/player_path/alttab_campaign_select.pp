# alttab_campaign_select: owner 11-25 "crashes if I alt tab out on new campaign selection". Main menu -> New Campaign picker
# (Carver V selected), then 20 alt-tab cycles: Alt down, the real SDL FOCUS_LOST (input script `focus lost` queues it), Alt up,
# hold unfocused, FOCUS_GAINED. The game must keep running and leave no crash record. Coordinates as in new_campaign.pp.
#! timeout 300
#! expect \[INPUT_SCRIPT v1\] frame=\d+ cmd=mark cycles_done
#! forbid \[CRASHBUNDLE\]
#! forbid \[RENDER_BACKEND\] FATAL
await \[INPUT_SCRIPT v1\] frame=0 script= 60
wait 150
key SPACE
wait 1400
move 300 300
wait 20
click 401 135
wait 150
click 229 248
wait 60
shot {out}/picker.tga
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
keydown Left Alt
wait 2
focus lost
keyup Left Alt
wait 45
focus gained
wait 30
shot {out}/picker_after.tga
mark cycles_done
mark done
