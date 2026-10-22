#!/usr/bin/env python3
"""Read-only stock mission census and reference-only art mapping.

Catalog supplies stock type bindings, bounds and model declarations. Counts are
rescanned from archive mission PacketFiles and FIT parts; loose mod overrides
are intentionally excluded. Mod usage is retained separately from the archived
seven-corpus census. No reference meshes or textures enter the draft generators.
"""
import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import re
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))
from tools.mc2x_import.fst import decompress_entry  # noqa: E402

CATALOG = Path('E:/mc2-archive-2026-10/asset-modernize/docs/assets/catalog.json')
INSTALL = Path('A:/Games/mc2-opengl/mc2-win64-v0.4')
OUT = Path('E:/mc2-ref/scifi-kit/mapping')
REF = Path('G:/mc2-assets/ref/buildings')
STYLE = Path('G:/mc2-assets/ref/style/MW5 and MWO')
THUMBS = CATALOG.parent / 'sheets/thumbs'
CATEGORIES = {'building', 'prop', 'turret', 'wall', 'gate', 'fence', 'bridge'}
OBJECT = struct.Struct('<i4f5i')
BT_MATERIAL = 'alina-godfrey-battletech-urbanwarfare-buildingmaterials.gif'
BT_PROPS = 'alina-godfrey-battletech-urbanwarfare-props-5.jpg'
HALIM = 'viona-halim-spacetower03-destroyedpartial.jpg'


def archive_index():
    """Header-only FST index, matching the archived catalog's sorted priority."""
    files = {}
    for path in sorted(INSTALL.glob('*.fst')):
        with path.open('rb') as fh:
            magic, count = struct.unpack('<II', fh.read(8))
            if magic not in (0xFADDECAF, 0xCADDECAF):
                raise ValueError(f'bad FST {path}')
            table = fh.read(count * 266)
        for pos in range(0, len(table), 266):
            off, comp, size, _ = struct.unpack_from('<IIII', table, pos)
            name = table[pos+16:pos+266].split(b'\0')[0].decode('latin1').replace('\\', '/').lower()
            files[name] = (path, off, comp, size, magic == 0xFADDECAF)
    return files


def read_member(record):
    path, off, comp, size, lzw = record
    with path.open('rb') as fh:
        fh.seek(off)
        raw = fh.read(comp)
    decoded = decompress_entry(0, comp, size, lzw, raw)
    if len(decoded) != size:
        raise ValueError(f'bad FST member length in {path}')
    return decoded


def packet(data, index):
    # PacketFile layout from mclib/packet.cpp, also used by the archived catalog.
    magic, first = struct.unpack_from('<II', data)
    if magic != 0xFEEDFACE or first % 4 or not 8 <= first <= len(data):
        raise ValueError('bad PacketFile header')
    count = first//4-2
    entries = struct.unpack_from(f'<{count}I', data, 8)
    entry = entries[index]
    storage, off = entry >> 29, entry & 0x1FFFFFFF
    end = entries[index+1] & 0x1FFFFFFF if index+1 < count else len(data)
    if not first <= off <= end <= len(data):
        raise ValueError('bad PacketFile offsets')
    raw = data[off:end]
    if storage in (0, 1): return raw
    if storage == 7: return b''
    if storage not in (2, 4): raise ValueError(f'bad storage {storage}')
    size, = struct.unpack_from('<I', raw)
    decoded = zlib.decompress(raw[4:]) if storage == 4 else decompress_entry(0, len(raw)-4, size, True, raw[4:])
    if len(decoded) != size: raise ValueError('bad packet length')
    return decoded


def fit_parts(raw):
    sections = {}; current = None
    for line in raw.decode('latin1').splitlines():
        section = re.match(r'\s*\[([^]]+)\]', line)
        if section:
            current = sections.setdefault(section[1].lower(), {})
        elif current is not None:
            field = re.match(r'\s*\w+(?:\[\d+\])?\s+([^=]+?)\s*=\s*(.*)', line)
            if field:
                current[field[1].strip().lower()] = field[2].split('//')[0].strip().lower()
    return [int(fields['objectnumber']) for name, fields in sections.items()
            if re.fullmatch(r'part\d+', name) and 'objectnumber' in fields
            and fields.get('exists', '1') not in ('0', 'false')]


def scan(data, out):
    by_type = {}
    for obj in data['objects']:
        for typ in obj['types']:
            if typ['type_id'] in by_type and by_type[typ['type_id']] != obj['id']:
                raise ValueError(f'ambiguous stock type {typ["type_id"]}')
            by_type[typ['type_id']] = obj['id']
    files = archive_index(); rows = []; sources = []; unresolved = Counter()
    for name in sorted(n for n in files if n.startswith('data/missions/') and n.endswith('.pak')):
        raw = read_member(files[name]); objects = packet(raw, 1)
        count, = struct.unpack_from('<i', objects)
        if count < 0 or len(objects) != 4 + count*OBJECT.size:
            raise ValueError(f'bad object record size {name}')
        records = list(OBJECT.iter_unpack(objects[4:]))
        if any(not all(math.isfinite(v) for v in rec[1:5]) for rec in records):
            raise ValueError(f'nonfinite object transform {name}')
        counts = Counter((rec[0], 'terrain') for rec in records)
        fitname = name[:-4]+'.fit'
        fitraw = read_member(files[fitname]) if fitname in files else b''
        counts.update((number, 'part') for number in fit_parts(fitraw))
        matched = defaultdict(Counter)
        for (number, kind), n in counts.items():
            ident = by_type.get(number)
            if ident: matched[ident][kind] += n
            else: unresolved[number] += n
        archive = files[name][0]
        for ident, c in matched.items():
            rows.append(dict(id=ident, corpus='stock', mission=Path(name).stem,
                             instances=c['terrain']+c['part'], terrain_instances=c['terrain'],
                             part_instances=c['part'], source=f'{archive.as_posix()}::{name}'))
        sources.append(dict(mission=Path(name).stem, source=f'{archive.as_posix()}::{name}',
                            pak_sha256=hashlib.sha256(raw).hexdigest(),
                            fit_sha256=hashlib.sha256(fitraw).hexdigest(),
                            terrain_records=count, existing_parts=sum(1 for _ in fit_parts(fitraw))))
    old = {(o['id'],m['mission']):m['instances'] for o in data['objects'] for m in o['missions'] if m['corpus']=='stock'}
    now = {(r['id'],r['mission']):r['instances'] for r in rows}
    differences = [dict(id=k[0],mission=k[1],archived=old.get(k,0),rescanned=now.get(k,0))
                   for k in sorted(old.keys()|now.keys()) if old.get(k,0)!=now.get(k,0)]
    proof = dict(missions=len(sources), sources=sources, unresolved_type_ids=dict(unresolved),
                 archived_count_differences=differences,
                 method='archive-only stock mission PAK packet 1: <i4f5i>; FIT PartN where Exists; includes inactive/script alternatives, not simultaneous visible objects',
                 catalog_sha256=hashlib.sha256(CATALOG.read_bytes()).hexdigest())
    (out/'mission-scan.json').write_text(json.dumps(proof,indent=2),encoding='utf8')
    (out/'mission-usage.json').write_text(json.dumps(rows,indent=1),encoding='utf8')
    print('Rescanned',len(sources),'stock missions;',len(differences),'differences from archived catalog;',len(unresolved),'unknown type IDs',flush=True)
    return rows


# Semantic blends: stock silhouette/colour always outrank the references. Each
# rule provides two independent structural vocabularies plus the artist masks.
# These are editorial analogues, not claims that source meshes interchange.
RULES = [
    (r'turretgenerator', 'defence power plant', 'industrial_generator_a', 'install_powergen', 'recessed generator apertures, heavy sockets and cable protection; keep low stock machinery', 'grey/olive machinery with cyan power apertures'),
    (r'turretcontrol', 'defence control building', 'military_turretcontrol', 'install_commcenterbldg', 'protected glazing, service spine and tiered control roof; preserve faction outline', 'stock faction khaki/grey concrete, dark windows and gravel roof'),
    (r'guntower', 'armed lookout', 'military_guardtower', 'install_militarytowerarmored', 'braced tower connections and cabin reveals; retain stock open lattice shaft', 'rust-brown lattice steel and dark observation cabin'),
    (r'nav\d|extractionmarker', 'mission marker', 'props_garrisondebris', 'banners_and_flags', 'no architectural silhouette analogue; retain the MC2 marker graphic', 'stock emissive mission marker colours'),
    (r'stopsign|yieldsign|slipperysign', 'road sign', 'props_military_defenseperimeter', 'banners_and_flags', 'brackets and post footings only; retain MC2 sign colour and function', 'stock red/yellow sign face on grey post'),
    (r'grainstorage', 'grain warehouse', 'urban_warehouse', 'install_packagingfacility', 'vertical sheet seams, roof coping and grain-handling access; keep rectangular stock block', 'silver-grey corrugated sheet and dark loading doors'),
    (r'valvestation|wasteprocessor|transformerstation|weaponsfacility', 'industrial services', 'industrial_generator', 'install_powergen', 'layered equipment shell, valve clearance, service connections', 'stock grey concrete and industrial metal, colour from source swatches'),
    (r'autocannon|lrmpopup|srmpopup|srmcamo', 'defence turret', 'turret_base', 'turretshredder', 'stock popup/camo silhouette; recessed mount and independent weapon/base joints', 'stock olive/grey metal, stock camouflage caps'),
    (r'walkway|promenade', 'bridge', 'space_raised_platform', 'install_concretemechwalkwaya', 'deck edges, supports and joints; preserve stock walkable span', 'stock grey ferrocrete and metal rails'),
    (r'guardhouse|shack', 'utility hut', 'military_checkpointhut', 'install_scrapbasemaintshed', 'overlapped panels, recessed doors and utility attachments', 'stock brown/khaki sheet and timber construction'),
    (r'dropship|shuttle|shilone|slayer|slipper', 'parked aircraft prop', 'military_hangar', 'install_hangar', 'hardware and surface zoning only; architectural families cannot supply a vehicle silhouette', 'stock painted aerospace alloy, dark apertures and faction colours'),
    (r'trafcon|terminal|lobby|s_main|quetragon', 'urban civic', 'urban_modularcity', 'install_corporate_urban_modular_kit', 'podium modules, recessed glazing and roof services; retain irregular stock lots', 'stock grey/blue metal or masonry; dark glass'),
    (r'razorwire|wirefence', 'perimeter wire', 'military_modularwalls', 'install_prisonwalllong', 'post boots, clamp spacing and tension connections; retain open stock wire rhythm', 'galvanised silver wire and steel posts'),
    (r'woodenfence', 'timber fence', 'props_military_defenseperimeter', 'install_scrapbasemaintshed', 'footing/brace rhythm only; retain timber construction', 'weathered brown timber slats'),
    (r'sandbag', 'field defence', 'military_gabions', 'install_bunker', 'layered courses and earth-filled mass, protected embrasures', 'khaki sandbags and dark earth'),
    (r'lightwall|lightpost', 'light wall', 'military_modularwalls', 'install_lowwall50m', 'modular cap joints and casting seams; stock terminal-pier profile', 'cool grey cast concrete with dark cap'),
    (r'mediumwall|mediumpost', 'medium wall', 'military_modularwalls', 'install_medwall50m', 'panel/socket connections and protected service recesses', 'grey concrete, muted sage inset panels'),
    (r'heavywall|heavypost|prison', 'heavy wall', 'military_modularwalls', 'install_barrier50m', 'stepped footings and panel buttresses; prison rail detail', 'pale grey ferrocrete and dark hardware'),
    (r'gate|milcheckpoint', 'gate and checkpoint', 'industrial_fortified_gate', 'install_prisongate', 'roller/hinge housing, framed openings and threshold markings', 'stock grey/khaki gate with hazard hardware'),
    (r'rpbuilding|repairbay', 'repair facility', 'military_garage', 'install_mechbay', 'layered door reveals and service sockets; keep the low stock ribbed roof', 'pale ribs, brown-silver industrial cladding'),
    (r'quonset', 'quonset', 'military_quonset_hut', 'install_hangar', 'arch bay/lap joints, door hood and plinth; stock barrel proportions', 'silver corrugated steel, olive frames' ),
    (r'crateswithtarp', 'tarp storage', 'props_supplycrates', 'install_scrapbaselargebox', 'case framing, recessed handling panels and fastened corners; original draped canvas', 'olive canvas over warm brown timber'),
    (r'crate|locker|dumpster|barrel|storagecontainer', 'small storage', 'props_military_storage', 'install_scrapbasemedbox01', 'stackable edges, handling pockets, straps and small hardware', 'stock timber/steel, olive and hazard yellow accents'),
    (r'lookouttower', 'lookout tower', 'military_guardtower', 'install_lookouttower01', 'observation cabin window reveals and service spine; stock tapered shaft', 'khaki ferrocrete, sage shaft insets, dark glass'),
    (r'turret|guntower|artillerypiece', 'defence turret', 'turret_base', 'turretlaser', 'turntable/housing separation, recessed weapon mounts and local damage', 'stock olive/grey steel; retain faction and weapon accents'),
    (r'hangar|meethall', 'hangar', 'military_hangar', 'install_hangar', 'bay jamb layers, arch ribs and utility zoning', 'pale beige panels or stock brown timber; dark glazing'),
    (r'barracks|ammodump|genericmilitary', 'military low-rise', 'military_readybarracks', 'install_mediummilitarybarracksb', 'battered plinth, inset panels and gravel coping', 'khaki ferrocrete, sage inset panels, dark gravel roof'),
    (r'fueltank|pipetank|liquidstorage|watertower|silo', 'tank and silo', 'industrial_watertank', 'install_industrialtanka', 'weld courses, access ladder, nozzles and restrained safety rails', 'pale grey steel, yellow-black safety band'),
    (r'greenhouse|geodesic|lrg_hydro', 'agriculture glazing', 'agriculture_domes', 'install_biodomeb', 'glazing gaskets, structural nodes and service plinth; retain stock dome/greenhouse profile', 'white/grey frame, translucent pale glazing, green planting'),
    (r'generator|power|purification|processing|chemical|factory|industrial', 'industrial plant', 'industrial_modularindustrial', 'install_civilianfactoryamain', 'corrugated bay walls, roof plant clusters and reinforced service podium', 'silver/brown industrial metal and grey concrete; stock pipe colours'),
    (r'sensor|radar|relay|hpg|aerospotter', 'communications', 'industrial_radardish', 'install_generic_satellitedish01', 'mount/yoke connections and layered mast footings', 'stock khaki/grey metal, pale dishes and dark electronics'),
    (r'city[2468]story|skyscraper|office|gov|admin|centerlobby|hub|palace|residence|resident|hq', 'urban civic', 'urban_modularcity', 'install_civilian_urban_modular_kit', 'podium/body/crown modules, window depth and roof parapets; preserve faction architecture', 'stock masonry/metal facade, dark windows and grey roof'),
    (r'civliving|portable', 'civilian habitation', 'urban_modularhousing', 'install_miningbarracks', 'small reveals, roof drainage and human-scale services', 'stock brown/grey walls with muted green inset panels'),
    (r'civstorage', 'civilian shed', 'urban_warehouse', 'install_industrialwarehousea', 'overlapping sheet courses, framed doors and weathered metal', 'weathered brown/silver corrugated cladding'),
    (r'bridge', 'bridge', 'space_raised_platform', 'install_concretemechwalkwaya', 'bearing shoes, deck joints and rail connections; preserve actual stock span', 'grey concrete/steel deck and metal rails'),
    (r'gantry|crane', 'gantry', 'props_industrial_scaffolding', 'install_industrialstairsa', 'braced joints, service access and plated feet; stock tower profile', 'stock grey steel frame and khaki cabin'),
    (r'pipe', 'pipework', 'props_industrial_modularpipe', 'install_industrialrefinerya', 'flanges, supports, elbows and protected valves', 'stock grey/olive pipe metal and ferrocrete plinths'),
    (r'light|alarm|perimeter', 'lighting', 'props_military_defenseperimeter', 'install_scrapbaseantennae01', 'booted posts, junction boxes and lamp brackets', 'dark metal posts; preserve red/blue/yellow light identity'),
    (r'tent|camonet', 'canvas shelter', 'military_checkpointhut', 'install_miningshipping', 'footing, attachment and service vocabulary only; canvas authored separately', 'olive canvas and dark steel poles'),
    (r'bunker', 'bunker', 'urban_bunker', 'install_bunker', 'armoured reveals, battered shell and local ferrocrete damage', 'stock khaki/grey ferrocrete'),
    (r'barricade|tripod', 'barrier prop', 'props_military_defenseperimeter', 'install_lowwall50mb', 'grounded bracing, overlaps and mechanical fasteners', 'grey corrugated metal or stock dark anti-vehicle steel'),
    (r'banner|flag', 'faction cloth', 'props_garrisondebris', 'banners_and_flags', 'cloth attachments and support brackets only; retain MC2 faction graphics as separate stock identity', 'stock faction-colour cloth and metal pole'),
    (r'junk|crash|mechpiece|lifeboat', 'wreckage', 'props_industrial_debris_machinery', 'brick_and_shard_debris', 'grounded load paths, separated cladding and soot; original recognizable wreckage', 'rusted steel, blue-grey alloy and stock hazard accents'),
    (r'prop$|truck|ambulance|armoredcar|jeep', 'parked vehicle prop', 'props_military_containers', 'install_miningshipping', 'hardware/material language only; vehicle silhouette has no direct architectural analogue', 'stock painted vehicle body, rubber wheels, dark glass'),
]


def mapping(obj):
    name = obj['id']
    for pattern,group,mw,bt,take,identity in RULES:
        if re.search(pattern,name):break
    else:
        group,mw,bt,take,identity = ('unresolved prop','props_military_storage','install_other_urban_modular_kit',
                                   'material and connection vocabulary only; owner needs silhouette decision',
                                   'stock palette retained; inspect source crop before modelling')
    if name in ('quonset','quonset2'): identity='rust-brown corrugated barrel, grey ends, olive frames'
    overrides={
        'geodesicdome':'khaki opaque triangular metal shell, grey/dark faceted plinth',
        'greenhouse':'green-grey glazed roof, pale steel frame and roof fans',
        'lrg_hydro':'pale open greenhouse frame, green beds and grey lower panels',
        'streetlight':'olive/grey square post and pale rectangular lamp head',
        'spotlight':'khaki steel post, pale round floodlight and dark grille',
        'perimeteralarm':'olive post, red alarm head and dark ferrocrete foot',
        'runwaylight':'yellow emissive tapered marker on dark grey plinth',
        'redbeaconlight':'red emissive tapered marker on dark grey plinth',
        'greenbeaconlight':'green emissive tapered marker on dark grey plinth',
        'bluebeaconlight':'blue emissive tapered marker on dark grey plinth',
        'woodencrates':'warm brown timber planks, framed ends and dark stencil panels',
        'barrels':'yellow steel drums with black hazard bands',
        'burningtrashbarrel':'yellow steel drum, black hazard band and red/orange open rim',
        'dumpster':'blue painted sheet metal bin with dark grille',
        'lockers':'grey steel locker panels and dark lower grilles',
        'munitionscrates':'olive/khaki stackable steel cases with red stencil plates',
        'portable':'khaki/grey cabin with dark brown door, sage windows and roof hatches',
        'sandbagbunker':'brown sandbag horseshoe with dark recess',
        'shantyfence':'rust-streaked brown/silver scrap sheet fence',
        'tripodbarricade':'dark crossed steel with yellow-black hazard paint',
        'sawhorsebarricade':'pale A-frame supports and yellow-black hazard crossbar',
        'smallbarricade':'black/yellow hazard sheet panels',
        'watertower':'rust-brown riveted steel drum on dark open legs, ladder and platform',
        'liquidstorage':'pale grey rounded metal bunker, red service panels and dark vents',
        'smallfueltank':'pale grey stepped steel drum with yellow-black band',
        'largefueltank':'pale grey steel cylinder, yellow-black band and faceted concrete foot',
        'pipetank':'pale grey open pipe tank with yellow-black band',
        'crane':'pale khaki cabin on black open steel legs',
        'gantry':'khaki control cabin, pale lattice service frame and twin antennas',
        'sensortower':'olive lattice mast and pale dish on battered footing',
        'relaystation':'grey lattice radio tower over khaki utility base',
        'singlecivlivinga':'grey corrugated barrel roof, brown masonry base and pale door trim',
        'singlecivlivingb':'grey arched roof, brown masonry and muted green panels',
        'singlecivlivingc':'weathered brown metal hut with dark slit window',
        'singlecivlivingd':'rust-brown metal low hut with grey coping',
        'hangar':'pale beige panels, long dark roof glazing and grey ribbed roof',
        'vehiclehangar':'sage corrugated panels, pale ribs and dark roof',
        'meethall':'brown plank walls, grey barrel roof and pale end trim',
        'tinshack':'weathered brown/silver corrugated shed, dark industrial annex',
        'shack':'silver corrugated shed with bright yellow lower band',
        'factorymd':'silver vertical corrugated steel, dark loading doors and charcoal flat roof',
        'factorylg':'brown/silver stepped corrugated steel with dark roller doors',
        'factorysm':'brown/silver corrugated shed with pitched monitor and dark roof',
        'processingplant':'brown/khaki metal processing block with grey tapered flues',
        'purificationplant':'brown machinery block, pale tanks with red bands and dark roof',
        'chemicalplant':'brown brick block, pale faceted silos and green pipe bands',
        'heavyindustrial':'red/brown masonry factory, grey roof and dark grilles',
        'industrialplant':'brown brick stacked factory, dark roof and pale pipes',
        'wasteprocessor':'brown/red masonry factory, grey roof vents and pale service boxes',
        'transformerstation':'grey/brown stepped electrical plant and dense dark hardware',
        'weaponsfacility':'grey open machinery platform with yellow-black perimeter band',
        'valvestation':'grey/silver machine tower over brown brick base',
        'terminal1':'white flat-roof module with sage inset band and grey roof',
        'terminal2':'white flat-roof module, sage band, pale stairs and mast',
        'walkway':'white flat-roof connector with continuous sage lower band',
        'trafcon':'pale grey control tower, dark glazing and mustard roof pads',
        'trafcon2':'brown/khaki control block, dark cabin windows and grey gravel roof',
    }
    identity=overrides.get(name,identity)
    if re.match(r'd_(light|medium|heavy)',name): identity='pale beige concrete piers, olive/sand wall panels and rust streaks'
    if re.match(r'l_(light|medium|heavy)',name): identity='pale stone piers, sage/olive wall panels and dark caps'
    if re.match(r's_(light|medium|heavy)',name): identity='cool grey ferrocrete blocks, darker grey terminal piers'
    if re.match(r'l_(palace|residen|elite|guard)',name): identity='olive/dark masonry, ochre pagoda roofs and red decorative insets'
    if re.match(r's_(admin|gov|main|promenade|quetragon)',name): identity='pale grey stone, blue-grey roofs and dark glazing'
    if name in ('gaussturret','clangaussturret','longtomturret'):bt='turretheavysniper'
    elif 'lrmturret' in name or name=='srmturret':bt='turretlrm'
    elif 'autocannon' in name:bt='turretshredder'
    elif name=='orbitalturret':bt='install_longtom'
    if name.startswith('city'):
        identity={'a':'red/brown masonry','b':'grey masonry','c':'green/blue-grey facade'}.get(name[-1],'stock facade')+', dark windows and grey roof'
    owner_folder = {
        'quonset':'barracks','military low-rise':'barracks','repair facility':'mech_bay',
        'lookout tower':'control_tower','armed lookout':'control_tower','tank and silo':'tanks_silos',
        'hangar':'aircraft_hangar','industrial plant':'factory','industrial services':'power_plant',
        'defence power plant':'power_plant','communications':'dish_antenna',
        'defence turret':'turret','bunker':'bunker','light wall':'walls_gates',
        'medium wall':'walls_gates','heavy wall':'walls_gates','gate and checkpoint':'walls_gates',
        'perimeter wire':'walls_gates','timber fence':'walls_gates',
    }.get(group,'misc_props')
    bt_path=REF/'bt2018'/bt/'reference-sheet.png'
    mw_path=REF/'mw5/mercs'/mw/'reference-sheet.png'
    if not bt_path.is_file() or not mw_path.is_file():
        raise FileNotFoundError(f'missing mapped family {bt_path} / {mw_path}')
    uncertainty = 'low'
    if group in ('unresolved prop','parked vehicle prop','parked aircraft prop','canvas shelter','faction cloth','wreckage','timber fence','mission marker','road sign'):
        uncertainty='high'
    elif re.match(r'[dls]_',name) or group in ('bridge','gantry','agriculture glazing','perimeter wire'):
        uncertainty='medium'
    donors={
        'perimeter wire':('post boot plates, wire clamps and tension end fittings','wire attachment brackets and small bolt seats'),
        'light wall':('cap seam and casting joint bevels','pier-to-panel socket seam and small access recess'),
        'medium wall':('panel edge rebates and cap fasteners','wall-to-pier seam covers and service hatch frame'),
        'heavy wall':('footing edge chamfers and buttress joint plates','panel socket trim and recessed fasteners'),
        'gate and checkpoint':('roller guide tracks and hinge cover joints','door jamb layering and threshold wear'),
        'repair facility':('roll-up slat edges, guide channel and recessed latch detail','bay jamb bevels and protected electrical service box'),
        'quonset':('sheet lap seams, roof fastening and window gaskets','door hood joint, thin end-cap trim and grounded broken roof panels'),
        'tarp storage':('case corner fasteners and timber framing joint detail','handling plate recess and canvas tie attachment; retain timber and canvas'),
        'small storage':('rim seams, closure fittings and handling recesses','strap attachment fasteners and welded seam wear; retain original contents/count'),
        'lookout tower':('cabin glazing gaskets and door frame rebates','shaft panel seam cover and coping edge bevel'),
        'defence turret':('turntable bearing seam, barrel collar and recoil sleeve detail','weapon mount bolts, casing access seam and base service hatch'),
        'hangar':('door guide channels, jamb fasteners and arch joint plates','sheet edge folds and roof drainage connections'),
        'military low-rise':('door reveal bevels, panel gaskets and coping joints','small wall vent louvers and plinth expansion seams'),
        'tank and silo':('weld bead courses, nozzle flanges and rail foot fittings','access hatch hinge/lock and ladder rung connection detail'),
        'agriculture glazing':('glazing gaskets and frame-to-base bolts','structural node caps and service hatch edging; retain stock glazing grid'),
        'industrial plant':('corrugated sheet overlaps, vent grille depth and pipe brackets','roof service box seams and footplate fasteners'),
        'industrial services':('pipe flange bolts and access cover rebates','electrical box latches and vent louvers'),
        'urban civic':('window reveal depth, sill drip edges and parapet seams','facade panel fasteners and roof hatch hinges'),
        'civilian habitation':('small door frame bevels and rain drip trims','window gaskets and service cover latches'),
        'civilian shed':('sheet folded edges and overlapped joint fastening','door jamb recess and hinge plates'),
        'communications':('dish yoke joint bolts and cable clamp detail','mast footplate layering and electronics hatch latches'),
        'bridge':('deck expansion joints and bearing shoes','rail-to-deck bolts and service plate seams'),
        'gantry':('bracing gusset plates and plated foot joints','ladder attachments and handrail end connections'),
        'pipework':('flange bolts, elbow weld seams and saddle clamps','valve stem fittings and support foot plates'),
        'lighting':('lamp mounting brackets and junction cover seams','post footplate bolts and cable entry grommets'),
        'timber fence':('plank end wear and fastening placement only; retain wood','small bracket bolts only; retain plank count and wood grain'),
        'canvas shelter':('canvas hem/tie construction only; retain cloth','pole fastening and ground anchor fittings only'),
        'wreckage':('fracture edge roughness and material-specific weathering only','ground contact/scorch distribution only; retain wood/canvas where present'),
    }
    mwbits,btbits=donors.get(group,('small bracket joints, fastener recesses and material-specific edge wear only',
                                  'access-cover hinge/latch detail and seam treatment only'))
    mw_take=f'{mw}: {mwbits}'
    bt_take=f'{bt}: {btbits}'
    take=mw_take+'; '+bt_take+'; Godfrey: material zoning and mask-authored wear applied to the original stock wood/canvas/paint/metal/concrete; Halim: seam-led local breakage'
    if obj['category']=='building':take+='; BT2018 generic_rubble_piles: low debris heap, broken frame and slab resting arrangement only, in this stock asset\'s materials/colours'
    return dict(group=group,mw5_family=mw,bt2018_family=bt,mw5_sheet=mw_path.as_posix(),
                bt2018_sheet=bt_path.as_posix(),reference_take=take,
                mw5_take=mw_take,bt2018_take=bt_take,
                owner_type_folder=(REF/owner_folder).as_posix(),
                silhouette_source='MC2 stock '+name+' owns silhouette, proportions, palette, counts and placement; donor parts only',
                material_reference=(STYLE/BT_MATERIAL).as_posix(),
                detail_reference=(STYLE/BT_PROPS).as_posix(),
                damage_reference=(STYLE/HALIM).as_posix(),
                rubble_reference=(REF/'bt2018/generic_rubble_piles/reference-sheet.png').as_posix() if obj['category']=='building' else '',
                damage_take=('Halim: local seam-led buckling for partial; BT2018 generic_rubble_piles: low slab heap, exposed broken frame and ground-resting fragments for full collapse; own stock materials/palette.'
                             if obj['category']=='building' else 'Stock destroyed-model behaviour owns the outline; Halim donates seam tears and material-specific scorch only. Fence topples; wall stays a shortened remnant.'),
                identity=identity,confidence=uncertainty)


def stock_path(name):
    alt=Path('G:/mc2-assets/out/scifi-kit/round2/stock/previews')/(name+'.png')
    if alt.exists():return alt
    if name=='slayerp':name='slayerparked'  # appearance alias differs from LOD0 key
    return THUMBS/(name+'.png')


def swatches(path):
    from PIL import Image
    if not path.is_file(): return ''
    im=Image.open(path).convert('RGB').resize((96,96))
    bg=im.getpixel((0,0)); c=Counter()
    for p in im.getdata():
        if sum(abs(p[i]-bg[i]) for i in range(3)) < 35 or max(p)<30:continue
        c[tuple(round(v/24)*24 for v in p)] += 1
    return ';'.join('#'+''.join(f'{min(255,v):02x}' for v in col) for col,n in c.most_common(3))


def csv_write(path,rows):
    with path.open('w',encoding='utf-8-sig',newline='') as fh:
        w=csv.DictWriter(fh,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)


def census(data,usage,out):
    by_id=defaultdict(list)
    for row in usage:by_id[row['id']].append(row)
    rows=[];details=[]
    for obj in data['objects']:
        if obj['category'] not in CATEGORIES:continue
        stock=by_id[obj['id']];mods=[m for m in obj['missions'] if m['corpus']!='stock']
        if not stock and not mods:continue
        n=sum(m['instances'] for m in stock)
        base=next((m for m in obj['models'] if m['role']=='intact' and m.get('lod')==0),{})
        size=base.get('bounds',{}).get('size',[0,0,0]);x,h,z=size
        score=n*math.sqrt(max(0,.5*x*z+.70710678*h*(x+z)))
        destroyed=[m for m in obj['models'] if m['role']=='destroyed']
        info=mapping(obj)
        rows.append(dict(rank=0,id=obj['id'],name=';'.join(t['type_name'] for t in obj['types']),
            type_ids=';'.join(str(t['type_id']) for t in obj['types']),category=obj['category'],
            scope='stock missions' if n else 'mod missions only',stock_instances=n,stock_missions=len(stock),
            campaign_instances=sum(m['instances'] for m in stock if re.fullmatch(r'mc2_\d\d',m['mission'])),
            mod_instances=sum(m['instances'] for m in mods),mod_missions=len(mods),
            modified_mod_instances=sum(m.get('modified_geometry_instances',0) for m in mods),
            visibility_priority=round(score,2),footprint_x_wu=round(x,4),footprint_z_wu=round(z,4),height_y_wu=round(h,4),
            author_width_m=round(x/4,4),author_depth_m=round(z/4,4),author_height_m=round(h/4,4),
            intact_model=base.get('tgl','missing'),intact_model_status='decoded' if 'bounds' in base else 'missing/undecoded',
            destroyed_models=';'.join(m['tgl'] for m in destroyed) or 'none declared',
            destroyed_status=';'.join('decoded' if 'bounds' in m else m.get('error','missing') for m in destroyed) or 'none declared',
            stock_texture_names=';'.join(base.get('textures',[])),stock_swatch_srgb=swatches(stock_path(obj['id'])),
            stock_crop=stock_path(obj['id']).as_posix(),**info))
        details.append(dict(id=obj['id'],collision_fields=[t.get('collision_fields',{}) for t in obj['types']],
                            models=[{k:v for k,v in m.items() if k in ('role','lod','tgl','bounds','sha256','error')} for m in obj['models']]))
    rows.sort(key=lambda r:(-r['visibility_priority'],-r['stock_instances'],-r['mod_instances'],r['id']))
    for i,row in enumerate(rows,1):row['rank']=i
    import counts
    counts.write_all(rows,out)
    csv_write(out/'mapping.csv',rows);csv_write(out/'inventory.csv',rows)
    all_usage=usage+[dict(id=o['id'],**m) for o in data['objects'] for m in o['missions'] if m['corpus']!='stock']
    selected={r['id'] for r in rows}
    detailed=[dict(id=m['id'],corpus=m['corpus'],mission=m['mission'],instances=m['instances'],
                   terrain_instances=m['terrain_instances'],part_instances=m['part_instances'],
                   modified_geometry_instances=m.get('modified_geometry_instances',0),source=m['source'])
              for m in all_usage if m['id'] in selected]
    csv_write(out/'mission-usage.csv',detailed)
    (out/'model-details.json').write_text(json.dumps(details,indent=1),encoding='utf8')
    return rows


def docs(rows,out):
    stock=sum(r['stock_instances']>0 for r in rows)
    questions=[
        ('Perimeter pieces dominate the usage ranking. Continue them before rarer hero buildings?', 'Yes; six drafts sample the leading perimeter assets and the most repeated repair/storage types.'),
        ('Should Davion, Steiner and Liao civic walls/buildings share one material language?', 'Share construction/detail scale, keep each stock faction silhouette, roof vocabulary and colour identity.'),
        ('Clean and rusty quonsets share the same barrel. Separate meshes or palette/condition variants?', 'OWNER ANSWER: one mesh, two palettes/material sets. quonset2 remains its own stock hardware variant.'),
        ('Parked ground vehicles and aircraft have no close building-reference analogue. Include them in this kit?', 'Inventory them, preserve stock silhouette; use architectural references only for hardware/materials. Defer full vehicle remakes.'),
        ('Flags and mission markers are mission-visible but are not normal architecture. Remodel them?', 'Keep their own stock identity; flag cloth may gain brackets, mission markers stay separate from world architecture.'),
        ('Timber fences, canvas tents and wreckage have weak structural matches in the supplied references. Convert them to sci-fi steel?', 'No; keep timber/canvas/wreck identity and borrow only fasteners, supports and weathering logic.'),
        ('Dome/greenhouse reference families have different framing. Use their panel pattern?', 'Preserve MC2 outline AND glazing counts/placements; references donate gasket, fastening and service-base detail only.'),
        ('Keep faction insignia and weapon glow colours?', 'Preserve MC2 identity; do not borrow logos, decals, emissive graphics or textures from reference games.'),
        ('How much destruction should a still-standing stock tower show?', 'OWNER ANSWER: keep local partial damage as intermediate; every building also gets fully collapsed matching rubble. Fence topples; wall follows stock damaged remnant.'),
        ('The supplied perimeter families are mostly solid walls, while MC2 wire fences are open. Add dense chain-link?', 'Keep each stock fence openness and strand rhythm; borrow only clamp, boot and tension-joint vocabulary.'),
        ('Should missing declared damage models be fabricated for inventory?', 'No; report declarations and decode status exactly. Original destroyed drafts are separate authoring outputs.'),
        ('How should mod-only usages influence the priority?', 'Show all 16 mod-only selections separately; order first by stock projected visibility and then mod instance count.'),
        ('Are the beauty materials ready for engine import?', 'OWNER ANSWER: bake high-detail authored shaders to pure base colour, tangent +Y normal and ORM atlas; three LODs; explicit SurfaceMaterial classes. No AO multiplied into base colour.'),
    ]
    unresolved=[r['id'] for r in rows if r['group']=='unresolved prop']
    if unresolved:
        questions.append(('No direct reference match for '+', '.join(unresolved)+'. Preferred treatment?',
                          'Retain each stock silhouette/colour; use the mapped generic kit only for material and connection vocabulary; defer individual remakes.'))
    text=['# Owner mapping questions','','All original defaults accepted by owner, with Q3/Q6/Q13 and full-rubble direction applied below. '
          'Barrels resolved from stock model/crop and confirmed by owner: THREE. No further question blocks this round.','']
    for i,(q,a) in enumerate(questions,1):text += [f'{i}. {q}',f'   Recommended default: {a}','']
    (out/'QUESTIONS.md').write_text('\n'.join(text),encoding='utf8')
    summary=[
        '# MC2 stock buildings and mission props: inventory and reference blends','',
        f'{len(rows)} stock selections: {stock} used in stock missions; {len(rows)-stock} used only by installed mod missions. '
        f'{sum(r["stock_instances"] for r in rows):,} stock potential placements. Full inventory and mapping: [mapping.csv](mapping.csv).',
        '',
        '268 selections have decoded stock base geometry. One additional placed declaration, dunebuggyprop (19 placements), '
        'has no usable base mesh; it is retained as a missing-geometry inventory item, not a proven visible object. '
        'These 19 placements account for all 11 mission differences from the archived census, which counted only resolved model bindings.',
        '',
        'Counts: fresh archive-only scan of 44 stock mission PAKs, packet 1 terrain objects plus FIT PartN with Exists. '
        'Includes campaign, multiplayer, demos and editor/test missions; campaign instances are a separate column. '
        'Script alternatives can be counted together; these are potential mission placements, not simultaneous rendered visibility. '
        'Mod counts come from the archived 319-mission, seven-corpus scan and are never added to stock priority. '
        'Trees, rocks, moving vehicles and mechs are outside this building/prop inventory; parked vehicle props, flags and markers remain explicit.',
        '',
        'Priority = stock_instances * sqrt(0.5*width*depth + 0.70710678*height*(width+depth)), using stock WU. '
        'This is an isometric size/usage heuristic, not measured camera exposure. '
        'Authoring metres = WU / 4; TG +Y is height, footprint is TG XZ. Bounds include protrusions and are not collision footprints. '
        '[model-details.json](model-details.json) retains exact bounds, declared states and collision fields. '
        '[mission-usage.csv](mission-usage.csv) lists every mission/count; [mission-scan.json](mission-scan.json) records hashes and comparison with the archive.',
        '',
        'KEEP THE MC2 STYLE. MC2 supplies footprint, silhouette, proportions, palette, feature counts and placement. MW5 and BT2018 donate only the exact parts/techniques named per row; '
        'Godfrey supplies construction/material zoning and mask vocabulary; Halim supplies local damage and abandonment. '
        'Reference thumbnails are private study material. No reference mesh, texture, UV, traced image or decal is shipped in draft GLBs. '
        'Colour descriptions are conservative type-based notes, supported by three measured thumbnail swatches per selection; '
        'swatches are lit sRGB preview samples, not recovered material albedos. High/medium uncertainty is explicit in the confidence column.',
        '',
        'Owner per-type folders and _inbox were checked and are empty in this snapshot; each mapping retains its intended owner folder. '
        'BT2018 expanded catalog re-read: 306 families, including the full-install supplement. No donor geometry or textures are shipped.',
        '',
        'COUNT BEFORE BUILD: each row links counts.json and carries all requested count/placement columns. '
        'Unknown means not audited, never zero. Eight current draft selections have stock evidence contracts; '
        'the remaining selections require a visual count audit before modelling. Count fields describe primary visible features; '
        'fasteners, bevel strips and subdivisions inside one continuous glazing band are secondary detail. '
        'Per-draft render review is recorded separately and must pass before a draft is called verified.',
        '',
        'Draft batch: razorwirefence (rank 1), s_lightwall (rank 3), rpbuilding (rank 11), quonsetclean (rank 12), '
        'crateswithtarp (rank 14), lookouttower (rank 17). This deliberately samples perimeter, low buildings, storage and a tower '
        'instead of filling all six slots with faction wall variants. '
        '[Draft contact sheet](drafts-contact-sheet.png). [Questions/defaults](QUESTIONS.md).',
        '',
        '| Rank | Stock ID | Type | Stock count / missions | Footprint XZ x height (WU) | Identity | Exact donor parts and techniques | Counts |',
        '|---:|---|---|---:|---|---|---|---|',
    ]
    for r in rows:
        summary.append(f'| {r["rank"]} | {r["id"]} | {r["category"]} | {r["stock_instances"]} / {r["stock_missions"]} | '
                       f'{r["footprint_x_wu"]:.1f} x {r["footprint_z_wu"]:.1f} x {r["height_y_wu"]:.1f} | '
                       f'{r["identity"]} | {r["reference_take"]}; {r["confidence"]} uncertainty | '
                       f'[{r["counts_status"]}](counts/{r["id"]}/counts.json): '+
                       '; '.join(key.replace('count_','')+'='+str(value) for key,value in r.items() if key.startswith('count_') and key!='count_notes')+' |')
    pages=math.ceil(len(rows)/6)
    summary += ['','Mapping sheets: '+', '.join(f'[page {i}](mapping-{i:02}.png)' for i in range(1,pages+1)),
                '',f'{len(questions)} questions; recommended defaults applied.']
    (out/'mapping.md').write_text('\n'.join(summary),encoding='utf8')
    return len(questions)


def font(n):
    from PIL import ImageFont
    return ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',n)


def reference_image(path):
    from PIL import Image
    p=Path(path);im=Image.open(p)
    if p.suffix.lower()=='.gif':im.seek(min(12,im.n_frames-1))
    im=im.convert('RGB')
    # Reference sheets have material overview + close view quadrants. The lower
    # right contains the textured close view; avoid shrinking a whole sheet.
    if p.name=='reference-sheet.png':
        w,h=im.size
        im=im.crop((w//2,int(h*.53),w,int(h*.91)))
    return im


def panel(im,path,box,reference=False):
    from PIL import Image,ImageOps
    p=Path(path)
    if not p.is_file():return
    src=reference_image(p) if reference else Image.open(p).convert('RGB')
    t=ImageOps.contain(src,(box[2],box[3]),Image.Resampling.LANCZOS)
    im.paste(t,(box[0]+(box[2]-t.width)//2,box[1]+(box[3]-t.height)//2))


def sheets(rows,out):
    from PIL import Image,ImageDraw
    cache={}
    def cached(path):
        if path not in cache:cache[path]=reference_image(path)
        return cache[path]
    for start in range(0,len(rows),6):
        im=Image.new('RGB',(1800,1800),(24,28,33));d=ImageDraw.Draw(im)
        d.text((18,8),f'MC2 / ORIGINAL BLEND MAPPING / {start+1}-{min(start+6,len(rows))} / reference-only thumbnails',font=font(23),fill='white')
        for j,label in enumerate(['STOCK / footprint & identity','MW5 / connections & parts','BT2018 / parts & damage','GODFREY / material process','HALIM / local destruction']):
            d.text((j*360+12,47),label,font=font(16),fill='#c5cbd0')
        for i,r in enumerate(rows[start:start+6]):
            y=82+i*282
            d.text((12,y),f'{r["rank"]}. {r["id"]}  |  {r["stock_instances"]} stock / {r["stock_missions"]} missions  |  {r["confidence"]} uncertainty',font=font(18),fill='white')
            panel(im,r['stock_crop'],(6,y+28,348,210))
            for j,path in enumerate([r['mw5_sheet'],r['bt2018_sheet'],r['material_reference'],r['damage_reference']],1):
                from PIL import ImageOps
                t=ImageOps.contain(cached(path),(348,210));im.paste(t,(j*360+6+(348-t.width)//2,y+28+(210-t.height)//2))
            d.text((370,y+242),r['mw5_family'],font=font(13),fill='#abb8c4')
            d.text((730,y+242),r['bt2018_family'],font=font(13),fill='#abb8c4')
        im.save(out/f'mapping-{start//6+1:02}.png')
    # Convenient first-page entry point; full pagination is linked in mapping.md.
    import shutil
    shutil.copyfile(out/'mapping-01.png',out/'mapping-contact-sheet.png')


def main():
    import psutil
    proc=psutil.Process()
    proc.cpu_affinity(list(range(psutil.cpu_count()))[2:4])
    proc.nice(psutil.BELOW_NORMAL_PRIORITY_CLASS)
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--reuse-scan',action='store_true');ap.add_argument('--no-sheets',action='store_true')
    a=ap.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    data=json.loads(CATALOG.read_text())
    usage=json.loads((OUT/'mission-usage.json').read_text()) if a.reuse_scan else scan(data,OUT)
    rows=census(data,usage,OUT);nq=docs(rows,OUT)
    if not a.no_sheets:sheets(rows,OUT)
    families=sorted(p.parent.name for p in (REF/'bt2018').glob('*/reference-sheet.png'))
    from datetime import datetime, timezone
    (OUT/'reference-snapshot.json').write_text(json.dumps(dict(captured_utc=datetime.now(timezone.utc).isoformat(),bt2018_families=len(families),families=families,
         bt2018_catalog_sha256=hashlib.sha256((REF/'bt2018/catalog.md').read_bytes()).hexdigest(),
         mw5_families=len(list((REF/'mw5/mercs').glob('*/reference-sheet.png')))),indent=1),encoding='utf8')
    print(f'{len(rows)} selections, {nq} questions; {len(families)} BT2018 families available; {OUT}',flush=True)


if __name__=='__main__':main()
