"""Intent pass, military base family, batch 1 (owner 2026-10-24: "on to the next set").

BattleTech base language over each stock silhouette: battered ferrocrete and plate masses with a proud plinth,
chamfered heads and stepped armour ribs; blast doors in deep chamfered portals with hazard jambs and sills;
armoured slit windows under visors; unit and faction stencils; antenna masts and dishes; sandbag barriers.
Faction colours follow the stock (Steiner grey, Davion tan-yellow, Liao green, generic khaki). MW5/BT2018 are
vocabulary only; nothing is copied.

This module is now the import surface and the SPECS registry; the builders live in
intent_military_{parts,field,walls,rebuild,batch4}.py, split in file order (later modules override earlier ones as before).
"""
import math

import numpy as np

import geo
import ind_parts as ip
import intent as I
import intent_power as PW

from intent_military_parts import (  # noqa: F401
    SEG, _null0, anim_node, antenna, bag_run, batter_mass, blast_door, contextmanager, count, dish, frame_of, loft,
    on_face, radar_dipole, rring, sandbag, slit_window, spin_antenna, stencil, stencil_panel,
)
from intent_military_field import (  # noqa: F401
    DOORS, MECH, _bunker, _ladder_roof, _mast_spin, _spin_dish, army_tent, barracks, blast_wall, crate,
    d_turretcontrol, fit_plan, hangar, l_turretcontrol, longtent, mast_lattice, mech_bay_dressing, mil_hall,
    munitionscrates, pallet, poly_ring, quonset2, relaystation, repairbay, repairbay_moving, roof_kit,
    s_turretcontrol, sandbagbunker, sandbagwall, sector_panel, sensortower, tent, tower_details, turret_control,
    vehiclehangar, wedge,
)
from intent_military_walls import (  # noqa: F401
    Field, GATES, GATE_APRON, GATE_BED, GATE_CAP, GATE_DOORS, GATE_ENDPAD, GATE_LAYOUT, GATE_RUNS, LEAF_END, POSTS,
    POST_DIMS, _MASS, _ccw_toward, _null, _outside, _tri_ccw, _variant, _wall_call, _whip, beacon, bturretcontrol,
    control_cab, gate_control, guntower, helipad, house_emblem, hq, massed, massing, mast_platform, masts,
    part_blocks, perimeteralarm, pier, profile_block, roof_at, roof_z, roundel, searchlight, sensor_array,
    sensor_control, spin_dish, spotlight, top_at, wall_gate, wall_post,
)
from intent_military_rebuild import (  # noqa: F401
    barrel, block, cab, chamfer_tower, clutter, compound_wall, d_gatecontrol, d_hq, d_sensorcontrol, doors, face,
    generic_hq, glazed_bays, k_at_face, l_gatecontrol, l_hq, l_sensorcontrol, l_spotlightcontrol, ribbon,
    s_gatecontrol, s_hq, s_sensorcontrol, s_spotlightcontrol, sensor_control2, spot_tower, tower_storeys,
)
from intent_military_batch4 import (  # noqa: F401
    _hpg_corridor, _terminal_hall, batter_block, emblem, ext_stair, hpg_corner, hpg_corridor, hpg_end, hpg_fourway,
    hpg_gen, hpg_office, hpg_threeway, hpg_walkway, hpgoutbldg, hpgstation, light_pylon, meethall, roof_pylon, shack,
    terminal1, terminal2, tinshack, trafcon, trafcon2, walkway,
)


SPECS = {
    'sandbagwall': dict(fn=sandbagwall, palette='intent_mil', cls='prop', counts=dict(panels_sections=1), tol=dict(grid_mae=.3),
        intent='A straight sandbag wall: five staggered courses of slumped, individually set bags, two rows wide for the lower three.'),
    'sandbagbunker': dict(fn=sandbagbunker, palette='intent_mil', cls='small', counts=dict(panels_sections=1),
        intent=('A C-shaped sandbag gun pit: six staggered courses (two rows wide below), a timber firing step along the '
                'east wall, a sloped steel firing shield with a gun slot and unit number, a tripod machine gun behind the slot, '
                'ammunition boxes and a water can.')),
    'tent': dict(fn=tent, palette='intent_mil', cls='prop', counts=dict(doors=2),
        intent=('An army frame tent: olive canvas ridge tent on a timber deck, the canvas sagging between pole stations under '
                'darker reinforcing straps, an eave valance, ridge pole ends, guy lines to pegs, and framed doors in both gables.')),
    'longtent': dict(fn=longtent, palette='intent_mil', cls='small', counts=dict(doors=2, windows=8),
        intent=('A long army mess tent: the same olive canvas ridge tent at twice the length, with framed end doors, rolled side '
                'vent flaps over screens, straps, guy lines and pegs on a timber deck.')),
    'munitionscrates': dict(fn=munitionscrates, palette='intent_mil', cls='prop', counts=dict(crates=8),
        intent=('A munitions stack: six timber ammunition crates on pallets with two more on top; corner posts, rim battens, '
                'plank slats, a hazard band, stencilled number plates, rope handles and steel corner brackets.')),
    'barracks': dict(fn=barracks, palette='intent_mil', cls='small', counts=dict(doors=2, windows=6),
        intent=('A low battered ferrocrete troop barracks on a proud plinth: walls leaning in to a chamfered roof slab, four '
                'stepped plate armour ribs over the roof and down both flanks, three armoured slit windows under visors per side, '
                'a blast door in a deep plate portal with hazard jambs and sill at each end, sandbag blast walls flanking each '
                'door, unit stencil plates, roof vents, a hatch and a heat-sink fin pack.')),
    'vehiclehangar': dict(fn=vehiclehangar, palette='intent_mil', cls='large', counts=dict(doors=8, windows=9, stairs_ladders=2),
        intent=('A drive-through vehicle hangar: a cast ferrocrete hall on a mitred glacis skirt with plate ribs, an olive band '
                'and plate coping; an armoured bay door at each end in a deep hazard-striped portal under the stencilled bay '
                'number 13; a windowed office lean-to along the front; plant room and stores annexes behind; a louvred exhaust '
                'tower rising through the roof with a bent exhaust duct down onto the annex roof; caged ladders ground to '
                'lean-to roof to hall roof, roof hatches, vents and a mast.')),
    'repairbay': dict(fn=repairbay, palette='intent_mil_brown', cls='large', counts=dict(doors=6, stairs_ladders=8, windows=6),
        tol=dict(grid_mae=1.0, footprint=.55, height=1.1, centroid=.25),
        intent=('Mech repair bay that fits every stock mech (tallest atlas.ase 16.94 m, widest bloodasp.ase 13.83 m), in the '
                'stock brown stepped massing grown to suit: the open pad between the workshop wings is 15.7 m clear between '
                'the gantries and 12.6 m deep (hazard border, foot targets, tie-down rails, floor number); a service gantry '
                'each side with platforms at knee 4.4, waist 8.6 and shoulder 13.0 m on caged ladders; an overhead crane '
                'runway with its underside at 19.0 m (2.06 m over the Atlas) from the hall head to an east portal, with '
                'bridge, trolley and hook block; a 16 x 19.5 m armoured bay door into an 11 m deep, 22 m tall hall; racks, '
                'status boards, the west services block and an external fire stair to the upper gallery door.')),
    'hangar': dict(fn=hangar, palette='intent_mil', cls='large', counts=dict(doors=10, windows=29),
        intent=('A big arched dropship/aircraft hangar: cast long walls on a glacis skirt with a high window row and office '
                'strip windows, plate buttress feet carrying proud armour ribs over a barrel-vault roof with a ridge vent and '
                'skylights; six staggered sliding hangar doors on a top track between hazard pilasters under a big stencilled 07 '
                'on the east gable; a roll-up door, personnel door and clerestory louvres on the west gable.')),
    'quonset2': dict(fn=quonset2, palette='intent_quonset', cls='small', counts=dict(stairs_ladders=1, antennas_dishes=1),
        intent=('The approved quonset (rust corrugated barrel on arch ribs, framed end walls, dormer windows) carrying a sensor '
                'station: a railed steel deck on posts and roof stubs over the east flank with a caged ladder, an electronics '
                'cabinet with status light and placard, a tracking dish on a yoke, and a cable run down to the end wall.')),
    's_turretcontrol': dict(fn=s_turretcontrol, palette='intent_mil_s', cls='large', counts=dict(doors=4, windows=19, stairs_ladders=1, antennas_dishes=3),
        intent=('Steiner turret control in grey-silver plate: a tall octagonal command tower on a blue-grey base band with proud '
                'corner ribs and plate collars, a slit band and a glazed armoured cupola carrying the rotating radar dipole (stock node LOS_Radar); a blast door at its foot; a low '
                'link block and two chamfered hex pods with slit windows, blast doors and a unit stencil; an L-shaped cast blast '
                'wall, a cable tray to the tower, a roof ladder, a guyed mast with a rotating antenna head (stock node LOS_Antenae) and a tracking dish on a pod.')),
    'd_turretcontrol': dict(fn=d_turretcontrol, palette='intent_mil_d', cls='large', counts=dict(doors=4, windows=16, stairs_ladders=1, antennas_dishes=3),
        intent=('Davion turret control in tan-yellow plate over a pale grey base: a tall battered square tower with plate collars, '
                'paired slit windows on every face under an overhanging glazed crown carrying the rotating radar bar (stock node Spinner); a blast door at its foot; '
                'a stepped low annex (main hall, raised chamfered operations block with the rotating dish (stock node small Dish), north wing, east pod) and a gate '
                'kiosk with a mast; cable tray, roof ladder, stencil, vents and hatch.')),
    'l_turretcontrol': dict(fn=l_turretcontrol, palette='intent_mil_l', cls='large', counts=dict(doors=3, windows=19, stairs_ladders=1, antennas_dishes=5),
        intent=('Liao turret control in green plate over a tan base: a tall twelve-sided tapered tower with proud ribs and collars, a '
                'slit band and a glazed head ring under a pointed spire topped by a rotating dipole (stock node LOS_ant3); a blast door at its foot; a chamfered annex '
                '(main hall with a raised operations block, a command block and an end pod) with slit windows and stencils; an '
                'L-shaped blast wall, two small rotating roof antennas (stock nodes LOS_ant1, LOS_ant2), roof ladder, antenna and dish.')),
    'relaystation': dict(fn=relaystation, palette='intent_mil', cls='small', counts=dict(doors=2, stairs_ladders=2, antennas_dishes=7),
        intent=('Comms relay: a corrugated equipment shelter with cast corner piers, louvres, a double door and a personnel door '
                'under a hipped roof with a grated centre deck; a lattice mast standing on the deck (straight to a railed service '
                'platform at 13 m, tapering above) carrying five sector panels, two link dishes and a beacon; caged ladders ground '
                'to roof deck and roof to platform; cable tray.')),
    'sensortower': dict(fn=sensortower, palette='intent_mil', cls='small', counts=dict(doors=0, stairs_ladders=1, antennas_dishes=2),
        intent=('Sensor tower: a buttressed cast footing with steps cut in, a tapered ribbed pylon with plate collars and edge ribs, '
                'a full-height caged ladder through a hatch onto a railed head platform on knee braces, and the rotating sensor head (stock node Sensorpivot: column, turntable, housing, beacon) carrying the '
                'twin panel arrays in a V with glowing sensor strips (child node Sensordish).')),
    'd_heavygate': dict(fn=wall_gate, palette='intent_wall_d', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Davion heavy gate on the approved d_heavywall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'd_lightgate': dict(fn=wall_gate, palette='intent_wall_d', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Davion light gate on the approved d_lightwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'd_mediumgate': dict(fn=wall_gate, palette='intent_wall_d', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Davion medium gate on the approved d_mediumwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'l_heavygate': dict(fn=wall_gate, palette='intent_wall_l', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Liao heavy gate on the approved l_heavywall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'l_lightgate': dict(fn=wall_gate, palette='intent_wall_l', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Liao light gate on the approved l_lightwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'l_mediumgate': dict(fn=wall_gate, palette='intent_wall_l', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Liao medium gate on the approved l_mediumwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    's_heavygate': dict(fn=wall_gate, palette='intent_wall_s', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Steiner heavy gate on the approved s_heavywall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    's_lightgate': dict(fn=wall_gate, palette='intent_wall_s', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Steiner light gate on the approved s_lightwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    's_mediumgate': dict(fn=wall_gate, palette='intent_wall_s', cls='large', counts=dict(doors=2, legs_supports=2, panels_sections=2),
        intent=('Steiner medium gate on the approved s_mediumwall code: end piers and short cast runs exactly as the walls, two taller gate-tower piers with slit windows, lamps and beacons, and two armoured sliding leaves (framed panels, hazard foot band, faction roundel both faces) on a ground track.')),
    'd_heavypost': dict(fn=wall_post, palette='intent_wall_d', cls='small', counts=dict(legs_supports=1),
        intent=('Davion heavy wall post: the approved d_heavywall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'd_lightpost': dict(fn=wall_post, palette='intent_wall_d', cls='small', counts=dict(legs_supports=1), tol=dict(grid_mae=.3),
        intent=('Davion light wall post: the approved d_lightwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'd_mediumpost': dict(fn=wall_post, palette='intent_wall_d', cls='small', counts=dict(legs_supports=1),
        intent=('Davion medium wall post: the approved d_mediumwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'l_heavypost': dict(fn=wall_post, palette='intent_wall_l', cls='small', counts=dict(legs_supports=1),
        intent=('Liao heavy wall post: the approved l_heavywall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'l_lightpost': dict(fn=wall_post, palette='intent_wall_l', cls='small', counts=dict(legs_supports=1),
        intent=('Liao light wall post: the approved l_lightwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    'l_mediumpost': dict(fn=wall_post, palette='intent_wall_l', cls='small', counts=dict(legs_supports=1),
        intent=('Liao medium wall post: the approved l_mediumwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    's_heavypost': dict(fn=wall_post, palette='intent_wall_s', cls='small', counts=dict(legs_supports=1),
        intent=('Steiner heavy wall post: the approved s_heavywall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    's_lightpost': dict(fn=wall_post, palette='intent_wall_s', cls='small', counts=dict(legs_supports=1),
        intent=('Steiner light wall post: the approved s_lightwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    's_mediumpost': dict(fn=wall_post, palette='intent_wall_s', cls='small', counts=dict(legs_supports=1),
        intent=('Steiner medium wall post: the approved s_mediumwall terminal pier (same staged cast taper, face fields, bolts and cap) as a free-standing post, with a red beacon on the cap.')),
    's_gatecontrol': dict(fn=s_gatecontrol, palette='intent_mil_s', cls='large', counts=dict(antennas_dishes=1, doors=1, windows=21),
        intent=('Steiner gate control. Features: (1) long two-storey gate block with a balcony band and an upper ribbon of windows; (2) tall dark glazed bays at ground (the gatehouse front); (3) the tower with a dark overhanging control cab and the rotating radar (stock Base01); (4) blue-grey bands, roof clutter.')),
    'd_gatecontrol': dict(fn=d_gatecontrol, palette='intent_mil_d', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=9),
        intent=('Davion gate control. Features: (1) central tower: a battered tan base under a pale cab storey with a window band and the rotating dish (stock Dish); (2) twin side blocks with horizontally ribbed tan upper walls; (3) a garage door and a long low east wing with a ribbon of windows; (4) the steel front stair to the gate roof; (5) tan-yellow bands on pale cast walls.')),
    'l_gatecontrol': dict(fn=l_gatecontrol, palette='intent_mil_l', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=23),
        intent=('Liao gate control. Features: (1) the pale tower with a dark rounded overhanging cab and the rotating roof dish (stock Dish); (2) the green ribbed arched annex roof over tan walls; (3) tall framed windows and a door in the annex front; (4) green faction bands; (5) clutter.')),
    'bturretcontrol': dict(fn=bturretcontrol, palette='intent_mil', cls='large', counts=dict(antennas_dishes=2, doors=2, windows=17),
        intent=('Generic turret control on the stock massing: tall command tower over a low battered annex, slit windows per storey, blast door, a tall whip antenna, and the rotating radar bar (stock animated node Spinner_PAB).')),
    's_sensorcontrol': dict(fn=s_sensorcontrol, palette='intent_mil_s', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=8),
        intent=('Steiner sensor control. Features: (1) the tall dark sensor-array slab with a grid of panels and lit emitter strips; (2) the pale control tower beside it with ribbon storeys and a glazed cab near the top; (3) the rotating sensor dish on a stand on top (stock L_Dish, and L_Sdish on the Liao); (4) the low annex with a garage door; (5) faction bands.')),
    'd_sensorcontrol': dict(fn=d_sensorcontrol, palette='intent_mil_d', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=8),
        intent=('Davion sensor control. Features: (1) the tall dark sensor-array slab with a grid of panels and lit emitter strips; (2) the pale control tower beside it with ribbon storeys and a glazed cab near the top; (3) the rotating sensor dish on a stand on top (stock L_Dish, and L_Sdish on the Liao); (4) the low annex with a garage door; (5) faction bands.')),
    'l_sensorcontrol': dict(fn=l_sensorcontrol, palette='intent_mil_l', cls='large', counts=dict(antennas_dishes=1, doors=2, windows=8),
        intent=('Liao sensor control. Features: (1) the tall dark sensor-array slab with a grid of panels and lit emitter strips; (2) the pale control tower beside it with ribbon storeys and a glazed cab near the top; (3) the rotating sensor dish on a stand on top (stock L_Dish, and L_Sdish on the Liao); (4) the low annex with a garage door; (5) faction bands.')),
    's_spotlightcontrol': dict(fn=s_spotlightcontrol, palette='intent_mil_s', cls='large', counts=dict(antennas_dishes=1, doors=3, windows=15),
        tol=dict(grid_mae=.3),
        intent=('Steiner spotlight control (a small air-traffic-control building in a walled yard). Features: (1) a slim control tower with ribbon storeys and a glazed overhanging cab with a balcony; (2) the searchlight on the cab roof (stock Light, rotating); (3) the two-storey operations block, a garage wing and a south entrance block placed on the stock plan, their doors facing the yard; (4) a low cast compound wall closing the SE yard, its chamfered corner carrying the gate; (5) blue-grey bands and roof clutter.')),
    'l_spotlightcontrol': dict(fn=l_spotlightcontrol, palette='intent_mil_l', cls='large', counts=dict(antennas_dishes=1, doors=3, windows=7),
        tol=dict(grid_mae=.3),
        intent=('Liao spotlight control (a small air-traffic-control building in a walled yard). Features: (1) a pale octagonal control tower with a band per storey and a glazed octagonal cab with a balcony; (2) the searchlight on the cab roof (stock light, rotating); (3) the operations block, the green ribbed arched annex and an arched entrance wing placed on the stock plan, their doors facing the yard; (4) a low cast compound wall closing the SE yard, its chamfered corner carrying the gate; (5) green bands.')),
    's_hq': dict(fn=s_hq, palette='intent_mil_s', cls='large', counts=dict(antennas_dishes=1, doors=6, windows=27),
        intent=('Steiner HQ. Features: (1) tall office tower with stacked ribbon storeys and a glazed overhanging control cab with a balcony; (2) long central hall with a raised clerestory spine; (3) tall dark glazed bays on the front; (4) a low flat-roofed garage block built onto the hall with roll-up doors; (5) blue-grey faction bands, roof clutter and a fist-emblem roof plate.')),
    'l_hq': dict(fn=l_hq, palette='intent_mil_l', cls='large', counts=dict(antennas_dishes=1, doors=5, windows=31),
        intent=('Liao HQ. Features: (1) pale chamfered tower with a dark window band per storey and a flared glazed head with a helipad; (2) the long green ribbed arched hangar roof; (3) tan lower walls with doors and windows under the arch; (4) green faction bands; (5) roof clutter and masts.')),
    'd_hq': dict(fn=d_hq, palette='intent_mil_d', cls='large', counts=dict(doors=5, windows=21),
        intent=('Davion HQ. Features: (1) tower with a tan glazed cab band and helipad over pale stacked storeys; (2) the tan front blocks with garage doors and repeated pilasters; (3) the sloped stair-ramp shoulder stepping down west; (4) tan-yellow faction bands on pale cast walls; (5) roof clutter.')),
    'generic_hq': dict(fn=generic_hq, palette='intent_mil', cls='large', counts=dict(antennas_dishes=1, doors=5, windows=21),
        intent=('Generic HQ. Features: (1) chamfered blue-grey tower with dark storey bands and a flared glazed head with a helipad; (2) the long sloped riveted hall stepping down to the east; (3) the stepped west wing with a sloped roof; (4) riveted plate panels and bands; (5) doors and roof clutter.')),
    'guntower': dict(fn=guntower, palette='intent_mil', cls='small', counts=dict(legs_supports=4, stairs_ladders=1, windows=4),
        intent=('Timber trestle gun tower: four raked legs on cast footings with X bracing and girts, a caged ladder inside to an armoured gun cab with a slit band and a gun on a mantlet.')),
    'spotlight': dict(fn=spotlight, palette='intent_spot', cls='small', counts=dict(antennas_dishes=1), tol=dict(grid_mae=.3),
        intent=('Field searchlight after the stock (owner: no platform, no ladder, nothing on top of the lamp): battered armoured plinth, slim mast, and the stock L_SpotRotar fork turning on the mast with the round lamp drum held at its sides on trunnions, lens out, cable and vent ring.')),
    'perimeteralarm': dict(fn=perimeteralarm, palette='intent_mil', cls='small', counts=dict(antennas_dishes=1, stairs_ladders=1),
        intent=('Perimeter alarm mast: equipment cabinet with placard and status light, steel pole, caged ladder to a railed ring platform, and the rotating siren head with four horns and a red beacon (stock animated node spinny).')),
    'trafcon': dict(fn=trafcon, palette='intent_terminal', cls='large', counts=dict(antennas_dishes=1, doors=3, windows=27),
        intent=('Airfield traffic control. Features: (1) the dominant wide glazed control cab, a ring of slanted glass under a heavy cap, wider than the shaft; (2) the rotating radar with a big airfield beacon stacked on it, on the cab roof (stock RotatyBit); (3) the setback shaft with ribbon storeys rising out of a battered base; (4) a low east annex with a roll-up door; (5) the sloped ramp shoulder to the north.')),
    'trafcon2': dict(fn=trafcon2, palette='intent_terminal', cls='large', counts=dict(antennas_dishes=1, doors=4, windows=34),
        intent=('Spaceport traffic control. Features: (1) the north tower with the dominant wide glazed control cab and the rotating radar (stock Spinny); (2) the 20 m office block to the south with three ribbon storeys and garage doors; (3) the 9 m podium between with side wings and a stair rising to the tower; (4) faction-neutral bands, clutter.')),
    'terminal1': dict(fn=terminal1, palette='intent_terminal', cls='large', counts=dict(antennas_dishes=0, doors=2, windows=4), tol=dict(footprint=.5, centroid=.2),
        intent=('Spaceport terminal. Features: (1) the long white hall with sloped upper walls and a continuous dark glazing band; (2) roof skylight troughs; (3) a tall light pylon on the roof; (4) the external stairs at the ends; (5) a roof access hatch block.')),
    'terminal2': dict(fn=terminal2, palette='intent_terminal', cls='large', counts=dict(doors=2, windows=4), tol=dict(footprint=.5),
        intent=('Spaceport terminal (two pylons): the white hall with sloped upper walls, the continuous dark glazing band and skylight troughs, two tall light pylons on the roof at opposite ends, doors front and back.')),
    'walkway': dict(fn=walkway, palette='intent_terminal', cls='large', counts=dict(panels_sections=1, windows=4),
        intent=('Terminal link walkway: the same white hall section (sloped upper walls, dark glazing band, skylight trough) as a connector between terminals.')),
    'hpgstation': dict(fn=hpgstation, palette='intent_hpg', cls='large', counts=dict(antennas_dishes=1, doors=1, windows=22),
        intent=('HPG station. Features: (1) the large pale faceted HPG dish with concentric panel rings and edge tabs, tilted skyward on an elevation axle in a two-arm yoke; (2) a single central feed mast with the feed horn and a glowing emitter; (3) the radial-ribbed turntable drum on a battered tan pylon that rises out of a broad podium, heavy conduit loops from the drum into a stepped link wing joining the control complex; (4) the battered control complex with emblem slabs; (5) slit bands and roof grates.')),
    'hpg_office': dict(fn=hpg_office, palette='intent_hpg', cls='large', counts=dict(antennas_dishes=0, doors=2, windows=14),
        intent=('HPG office. Features: (1) three stacked battered tan tiers stepping up to a 20 m crown; (2) the tall ComStar emblem slab on the west face; (3) dark slit bands on each tier; (4) the low east annex; (5) doors at the foot.')),
    'hpg_gen': dict(fn=hpg_gen, palette='intent_hpg', cls='large', counts=dict(doors=2, panels_sections=2, windows=3),
        intent=('HPG generator. Features: (1) a long stator drum in stepped ribbed segments with cyan glow in the joints, in pale ComStar collars on a battered tan cradle plinth; (2) a maintenance gantry with a hoist over the drum; (3) coolant manifolds and conduit into the control block; (4) two louvred radiator banks; (5) the battered east control block with the ComStar emblem, two doors and roof grates.')),
    'hpgoutbldg': dict(fn=hpgoutbldg, palette='intent_hpg', cls='large', counts=dict(doors=1, windows=14),
        intent=('HPG outbuilding. Features: (1) the low U-shaped battered tan block round a courtyard; (2) dark slit bands; (3) emblem pillars flanking the courtyard entrance; (4) a recessed glazed entrance and roof grates.')),
    'hpg_threeway': dict(fn=hpg_threeway, palette='intent_hpg', cls='large', counts=dict(panels_sections=4, windows=12),
        intent=('HPG three-way corridor node: a battered chamfered hub with a lantern grate and three battered corridor arms with dark slit bands, roof walkway grates and end portals (one module family for the HPG set).')),
    'hpg_corner': dict(fn=hpg_corner, palette='intent_hpg', cls='large', counts=dict(panels_sections=3, windows=8),
        intent=('HPG corner node: the battered hub with two corridor arms (same module family).')),
    'hpg_end': dict(fn=hpg_end, palette='intent_hpg', cls='large', counts=dict(panels_sections=2, windows=4),
        intent=('HPG end node: the battered hub with one corridor arm and a ComStar emblem on its blind end.')),
    'hpg_fourway': dict(fn=hpg_fourway, palette='intent_hpg', cls='large', counts=dict(panels_sections=5, windows=16),
        intent=('HPG four-way node: the battered hub with four corridor arms.')),
    'hpg_walkway': dict(fn=hpg_walkway, palette='intent_hpg', cls='large', counts=dict(panels_sections=1, windows=16),
        intent=('HPG corridor: the battered tan corridor section with a dark slit band, roof walkway grate, hatches and end pilasters.')),
    'tinshack': dict(fn=tinshack, palette='intent_shed', cls='small', counts=dict(doors=2, panels_sections=1, windows=2),
        intent=('Tin shack. Features: (1) a corrugated gabled shed; (2) the open lean-to porch on posts along the south side; (3) the tall dark flue/boiler stack on the north side; (4) louvred gable vent, door and windows.')),
    'meethall': dict(fn=meethall, palette='intent_shed', cls='large', counts=dict(doors=3, windows=9),
        intent=('Mess hall. Features: (1) the tall central barrel-vault hall; (2) the semicircular spoked fanlight in the gable; (3) the long low lean-to wings either side with paired windows; (4) the timber-boarded walls and doors.')),
    'shack': dict(fn=shack, palette='intent_shed', cls='small', counts=dict(doors=1, windows=4),
        intent=('Shack. Features: (1) a small corrugated gabled hut with a pale roof; (2) paired windows on each long side; (3) a steel door on the end; (4) a concrete pad.')),
}
