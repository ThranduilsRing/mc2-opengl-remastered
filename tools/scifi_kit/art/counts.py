"""Stock evidence and count contracts. Unknown values are never replaced by zero.

Counts describe primary visible features, not bevel strips, fasteners, glass
sub-panes, corrugation waves or new concealed reinforcement. Front is -Y in
authoring space. Render review is a separate, explicitly recorded step.
"""
import hashlib
import json
from functools import lru_cache
from pathlib import Path

CATALOG=Path('E:/mc2-archive-2026-10/asset-modernize/docs/assets/catalog.json')
STOCK=Path('G:/mc2-assets/out/scifi-kit/round2/stock')
FIELDS=('doors','windows','openings_bays','panels_sections','crates','barrels',
        'tanks','arches_ribs','legs_supports','antennas_dishes','stairs_ladders')


def known(**features):
    result={key:dict(count=0,placements=[]) for key in FIELDS}
    for key,placements in features.items():
        result[key]=dict(count=len(placements),placements=placements)
    return result


CONTRACTS={
 'razorwirefence':known(panels_sections=['one open wire section'],
     legs_supports=['-Y end post','+Y end post']),
 's_lightwall':known(panels_sections=[f'wall face panel {i+1}/8 along Y' for i in range(8)],
     legs_supports=['single +Y terminal pier']),
 'rpbuilding':known(doors=['front centered entry door','rear centered entry door',
                         'left wide elevated service closure','left narrow elevated service closure',
                         'right wide elevated service closure','right narrow elevated service closure'],
     openings_bays=['front centered bay mouth','rear centered bay mouth'],
     panels_sections=['left roof lane','center roof lane','right roof lane',
                      'front left cladding','front right cladding','rear left cladding','rear right cladding',
                      'left wide service panel','left narrow service panel','right wide service panel','right narrow service panel'],
     arches_ribs=['left outer longitudinal rib','left inner longitudinal rib',
                  'right inner longitudinal rib','right outer longitudinal rib'],
     legs_supports=[f'{side} end of rib {i+1}' for i in range(4) for side in ('front','rear')]),
 'quonsetclean':known(doors=['front centered door','rear centered door'],
     windows=['front left','front right','rear left','rear right',
              'left side front','left side middle','left side rear',
              'right side front','right side middle','right side rear'],
     openings_bays=['front centered entry','rear centered entry'],
     panels_sections=['continuous corrugated barrel','front end cap','rear end cap'],
     arches_ribs=['one continuous barrel vault']),
 'crateswithtarp':known(crates=['left case under its own tarp','right case under its own tarp'],
     panels_sections=['left canvas cover','right canvas cover']),
 'lookouttower':known(doors=[f'{side} centered base door' for side in ('front','right','rear','left')],
     windows=[f'{side} cabin window {j+1}/3' for side in ('front','right','rear','left') for j in range(3)],
     openings_bays=[f'{side} base entry' for side in ('front','right','rear','left')],
     panels_sections=['front centered shaft inset','rear centered shaft inset',
                      'left forward shaft strip','left rear shaft strip','right forward shaft strip','right rear shaft strip'],
     legs_supports=['single tapered enclosed shaft']),
 'barrels':known(barrels=['front drum, offset left','middle drum, offset right','rear drum, offset left']),
}
CONTRACTS['quonset']=CONTRACTS['quonsetclean']
NOTES={
 'razorwirefence':'Stock alpha texture: two end posts, nine horizontal strand rows and a wire crown. Approved shape retained; extra middle post removed to meet count rule.',
 's_lightwall':'Four geometric wall spans carry paired visible face panels (eight); one terminal pier. Destroyed stock is a short standing jagged remnant, not a rubble pile.',
 'rpbuilding':'ONE centered entry per end face (front and rear), not two side-by-side entries. Stock four-side projection proves this. Doors01 has two end closures; Doors has four elevated side service closures (one wide + one narrow per side), not four more ground-level bays. Four longitudinal ribs define three roof lanes; no rib intersects either central entry. Evidence: studies/rpbuilding-stock-four-sides.png.',
 'quonsetclean':'Stock LitWin has four planes: two end planes and two side planes. End texture has two windows flanking one door and is used on both ends; side texture has three windows. Door glazing is part of door, not another window. New hidden framing and fine sheet lap seams are secondary detail.',
 'crateswithtarp':'Owner and stock agree: two case bodies, each with one canvas drape. Planks and fasteners are secondary detail, not extra crates.',
 'lookouttower':'Stock main triangle pairs 16-19 repeat UV [0.616,0.813]-[0.996,0.993] on ALL FOUR base faces: four doors. Cabin pairs 5-8 repeat UV [0.345,0.858]-[0.591,0.990], containing THREE separately framed windows: twelve total. LitWin has four broad overlay planes, not four windows. Shaft has one broad front/rear inset and two narrow side strips: six panels. Evidence: studies/lookout-used-uv-regions.png and lookouttower-stock-four-sides.png. No staircases in stock; compact thresholds only. Internal frame does not add visible tower legs.',
 'barrels':'Stock mesh has three top-cap regions; stock crop also shows three lids. OWNER CONFIRMED: match stock, three barrels.',
}
NOTES['quonset']=NOTES['quonsetclean']+' Shares exactly the clean geometry; material set only.'
CONTRACTS.update({
 'streetlight':known(legs_supports=['one square pole'],panels_sections=['one inclined lamp head']),
 'woodencrates':known(crates=['left case','right case']),
 'runwaylight':known(panels_sections=['one triangular tapered yellow marker']),
 'redbeaconlight':known(panels_sections=['one triangular tapered red marker']),
 'tripodbarricade':known(legs_supports=[f'tripod {i+1}/5 along Y' for i in range(5)],
     panels_sections=[f'hazard X {i+1}/5 along Y' for i in range(5)]),
 'corrigatedmetalbarricade':known(legs_supports=['left A-frame','middle A-frame','right A-frame'],
     panels_sections=['short left sheet','long right sheet']),
 'portable':known(doors=['left centered door'],
     windows=['front band','rear band','left forward window','left rear window',
              'right forward window','right middle window','right rear window'],
     panels_sections=['front unit','rear unit','left three-bay wall','right three-bay wall'],
     stairs_ladders=['one left porch with four treads']),
 'singlecivlivinga':known(doors=['left rear entry'],
     windows=['front three-pane band','rear three-pane band','left forward roof window',
              'left rear roof window','right forward roof window','right rear roof window'],
     panels_sections=['polygonal arched shell']+
         [f'{side} lower wall panel {i+1}/8' for side in ('front','rear') for i in range(8)]+
         ['left rear entry block','right rear base extension'],
     arches_ribs=['one polygonal barrel roof']),
})
NOTES.update({
 'streetlight':'Five stock sides: one square pole with two dark collars; one inclined rectangular head. No extra arm or base plinth.',
 'woodencrates':'Five stock sides: two side-by-side cases; every face has one mid rail, long faces also have one middle post, no diagonal. The uncovered short ends expose their mid rails. Same case construction as covered crates, without cloth or straps.',
 'runwaylight':'Five stock sides: one low triangular frustum, yellow luminous faces over one dark triangular foot. No invented pole or lamp cage.',
 'redbeaconlight':'Same triangular stock frustum and foot as runwaylight; red face palette only.',
 'tripodbarricade':'Five stock sides: five separate tripods along Y; each has one diagonal timber brace and two crossed yellow-black members. No continuous horizontal rail.',
 'corrigatedmetalbarricade':'Five stock sides: two inclined corrugated sheet sections joined at about one third of width, supported by three A-frames. Keep the stock low ramp outline.',
 'portable':'Five stock sides: one door on left long face; two window units flanking it, three opposite, one on each end (seven total). Door glass belongs to the door. Three low roof fixtures. Lot includes one left porch and four treads; room is 7.4 m wide within the 9.8 m lot.',
 'singlecivlivinga':'Five stock sides: one arched house, one left rear door, one horizontal three-pane band per end, two roof window units per side (six units total). Door glass is not another window. Eight lower wall panels per end (nine edge/joint battens). One rear chimney, left rear entry block and right rear base extension; polygonal roof stays stock-shaped.',
})
ORIGINS={
 'razorwirefence':(0,-5.335487589240074,0),
 's_lightwall':(.00950002670288086,4.07346248626709,-.0037249999586492777),
 'rpbuilding':(.1059121903963387,-.02738739736378193,-.02044999971985817),
 'quonsetclean':(0,-5.312649726867676,.021774999797344208),
 'crateswithtarp':(.006024923175573349,.005249999929219484,.0019499994814395905),
 'lookouttower':(.09718751907348633,.3088374137878418,0),
 'barrels':(.12242499180138111,-.05808753985911608,0),
 'quonset':(0,-5.329537391662598,-.005750000011175871),
 'streetlight':(-0.004774993984028697, -0.19372499742894433, 0.014174999669194221),
 'woodencrates':(0.006024923175573349, -0.005249999929219484, 0.0019499994814395905),
 'runwaylight':(-0.08568750345148146, 0.0012499999720603228, 0.0013749999925494194),
 'tripodbarricade':(0.038849979639053345, 0.005325288744643331, 0.0),
 'redbeaconlight':(-0.08568750345148146, 0.0012499999720603228, 0.0015999999595806003),
 'corrigatedmetalbarricade':(0.03281241375952959, 0.36480002058669925, 0.0),
 'singlecivlivinga':(-0.019162416458129883, 0.031599998474121094, -4.999999873689376e-05),
 'portable':(-0.03495001792907715, 5.373312592506409, -0.05612500011920929),
}


@lru_cache(maxsize=1)
def catalog():
    return {obj['id']:obj for obj in json.loads(CATALOG.read_text())['objects']}


def origin(name):
    # Measured catalog offsets keep the generators independent of external data.
    return ORIGINS[name]


def code_hash():
    here=Path(__file__).parent
    h=hashlib.sha256()
    for name in ('drafts.py','batch2.py','geo.py','bake.py','blend.py','content.py','lights.py','lang.json','counts.py'):
        h.update((here/name).read_bytes())
    return h.hexdigest()


def write_all(rows,out):
    pending=0
    for row in rows:
        name=row['id'];obj=catalog()[name]
        models=[m for m in obj['models'] if m['role']=='intact' and m.get('lod')==0]
        model=models[0] if models else {}
        source=STOCK/(Path(model.get('tgl',name)).stem+'.json')
        decoded=json.loads(source.read_text()) if source.is_file() else {}
        verified=name in CONTRACTS
        counts=CONTRACTS.get(name,{key:dict(count=None,placements=None) for key in FIELDS})
        note=NOTES.get(name,'Not yet visually audited on all sides. Model node counts are evidence only; they do not prove painted feature counts. Must audit before authoring this selection.')
        record=dict(schema='mc2-stock-counts/1',asset=name,
                    status='stock evidence reviewed; render review required' if verified else 'pending visual count audit',
                    definition=__doc__,counts=counts,notes=note,
                    source_model=model.get('tgl'),source_sha256=model.get('sha256'),
                    decoded_evidence=source.as_posix() if source.is_file() else None,
                    nodes=decoded.get('nodes',[]),
                    texture_evidence=[dict(path=t,sha256=hashlib.sha256(Path(t).read_bytes()).hexdigest())
                                      for t in decoded.get('texture_paths',[]) if t and Path(t).is_file()],
                    stock_crop=row['stock_crop'],render_review='pending')
        dest=out/'counts'/name/'counts.json';dest.parent.mkdir(parents=True,exist_ok=True)
        reviewed=dest.with_name('review.json')
        if verified and reviewed.is_file():
            review=json.loads(reviewed.read_text())
            if review.get('status')=='passed' and review.get('code_sha256')==code_hash():
                record['render_review']=review
                record['status']='verified on stock evidence and final renders'
        dest.write_text(json.dumps(record,indent=2),encoding='utf8')
        row['counts_path']=dest.as_posix();row['counts_status']=record['status']
        row['count_notes']=note
        for key,value in counts.items():
            row['count_'+key]='unknown' if value['count'] is None else value['count']
            row['placement_'+key]='unreviewed' if value['placements'] is None else '; '.join(value['placements']) or 'none'
        if not verified:pending+=1
    return pending
