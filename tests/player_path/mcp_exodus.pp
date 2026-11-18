#! timeout 420
#! differ 03_pick1 04_bought1 0.001
#! differ 07_bay_after_purchase 08_add_mech 0.01
await \[INPUT_SCRIPT v1\] frame=0 script= 400
await \[SOAK\] parked screen=2,1 400
wait 150
mark seed=2002
shot {out}/01_bay.tga
wait 10
click 665 277
wait 120
shot {out}/02_purchase.tga
wait 10
shot {out}/03_pick0.tga
wait 10
key F8
wait 60
shot {out}/04_bought0.tga
wait 10
click 167 111
wait 40
shot {out}/05_storage_sel.tga
wait 10
click 394 124
wait 60
shot {out}/06_sold.tga
wait 10
shot {out}/03_pick1.tga
wait 10
key F8
wait 60
shot {out}/04_bought1.tga
wait 10
click 730 565
wait 120
shot {out}/07_bay_after_purchase.tga
wait 10
click 662 111
wait 40
click 433 78
wait 60
shot {out}/08_add_mech.tga
wait 10
click 86 132
wait 60
shot {out}/09_pilot_click.tga
wait 10
key ESCAPE
wait 30
click 40 132
wait 40
click 604 498
wait 200
shot {out}/10_lab.tga
wait 10
click 573 100
wait 30
click 600 411
wait 30
click 375 322
wait 30
click 449 78
wait 40
shot {out}/11_add0.tga
wait 10
click 573 100
wait 30
click 600 200
wait 30
click 375 322
wait 30
click 449 78
wait 40
shot {out}/11_add1.tga
wait 10
click 679 100
wait 30
click 600 200
wait 30
click 267 222
wait 30
click 449 78
wait 40
shot {out}/11_add2.tga
wait 10
click 679 100
wait 30
click 600 411
wait 30
click 479 322
wait 30
click 449 78
wait 40
shot {out}/11_add3.tga
wait 10
click 285 276
wait 15
click 252 276
wait 15
click 285 167
wait 15
click 252 167
wait 15
click 498 276
wait 15
click 465 276
wait 15
shot {out}/12_armor.tga
wait 10
click 573 100
wait 30
click 600 200
wait 15
click 267 222
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 375 183
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 479 222
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 267 328
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 375 322
wait 15
click 449 78
wait 25
shot {out}/13_overweight.tga
wait 10
click 625 100
wait 30
click 600 200
wait 15
click 267 222
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 375 183
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 479 222
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 267 328
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 375 322
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 479 322
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 267 433
wait 15
click 449 78
wait 25
click 600 200
wait 15
click 479 433
wait 15
click 449 78
wait 25
shot {out}/14_overheat.tga
wait 10
click 730 565
wait 150
shot {out}/15_save_dialog.tga
wait 10
click 289 226
wait 20
key T
wait 5
key E
wait 5
key S
wait 5
key T
wait 5
key 1
wait 5
shot {out}/15b_typed.tga
wait 10
click 554 446
wait 120
shot {out}/16_after_save.tga
wait 10
click 40 132
wait 40
click 604 498
wait 200
click 573 100
wait 30
click 600 317
wait 30
click 267 222
wait 30
click 449 78
wait 40
click 68 565
wait 120
shot {out}/17_after_cancel.tga
wait 10
click 722 565
wait 200
shot {out}/18_pilot_screen.tga
wait 10
click 198 141
wait 40
click 596 259
wait 40
click 449 88
wait 40
shot {out}/19_pilot_assign0_slot474_row4.tga
wait 10
click 198 141
wait 40
click 596 259
wait 40
click 449 88
wait 40
shot {out}/19_pilot_assign1_slot474_row4.tga
wait 10
click 198 141
wait 40
click 596 222
wait 40
click 449 88
wait 40
shot {out}/19_pilot_assign2_slot474_row3.tga
wait 10
click 198 141
wait 40
click 596 108
wait 40
click 449 88
wait 40
shot {out}/19_pilot_assign3_slot474_row0.tga
wait 10
click 722 565
wait 200
shot {out}/20_next.tga
wait 10
await \[SOAK\] autowin mission= 300
await \[SOAK\] parked screen=2,1 200
wait 150
shot {out}/21_bay_after_mission.tga
wait 10
click 665 277
wait 120
shot {out}/22_purchase_after.tga
wait 10
click 730 565
wait 120
shot {out}/23_bay_after2.tga
wait 10
mark done
