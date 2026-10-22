"""Named building emitters. Geometry uses metres/Z up; -Y is front."""

def light_specs(name, size, condition):
    if condition in ('destroyed','partial','abandoned'):return []
    w,d,h=size
    result=[]
    def add(label,pos,direction,kind='spot',colour=(1,.78,.48),intensity='door'):
        result.append(dict(name='LIGHT_'+label,position_m=pos,direction=direction,
                           type=kind,colour_linear=colour,intensity_class=intensity,
                           range_m=5 if intensity=='door' else 8,outer_cone_degrees=65,
                           active=True,lights_out_on_destroy=True))
    if name=='rpbuilding':
        add('door_front_01',(0,-d*.32,h*.70),(0,-1,-.6))
        add('door_rear_01',(0,d*.32,h*.70),(0,1,-.6))
    elif name in ('quonsetclean','quonset'):
        add('door_01',(0,-d*.47,h*.52),(0,-1,-.6))
        add('door_02',(0,d*.47,h*.52),(0,1,-.6))
        for end in (-1,1):
            for side in (-1,1):
                add(f'window_end_{end}_{side}',(side*w*.21,end*d*.47,h*.32),
                    (0,end,0),'point',intensity='window')
        for side in (-1,1):
            for i,y in enumerate((-.29*d,0,.29*d)):
                add(f'window_{"left" if side<0 else "right"}_{i+1:02}',
                    (side*w*.44,y,h*.43),(side,0,0),'point',intensity='window')
    elif name=='lookouttower':
        for side,(x,y) in zip(('front','right','rear','left'),((0,-1),(1,0),(0,1),(-1,0))):
            add(f'door_{side}_01',(x*w*.49,y*d*.49,2.7),(x,y,-.7))
            for j in (-1,0,1):
                add(f'window_{side}_{j+2:02}',(x*w*.40+y*j*w*.20,y*d*.40-x*j*d*.20,h*.90),
                    (x,y,0),'point',intensity='window')
    return result


def from_kit(k,condition):
    """Snap emitter positions to actual authored lamp/glass pieces, after fitting."""
    from geo import centroid
    specs=light_specs(k.meta['stock'],k.meta['size'],condition)
    if not specs:return []
    lamps=[centroid(p) for p in k.pieces if p['m'].startswith('lamp')]
    panes=[centroid(p) for p in k.pieces if p.get('pane')]
    for s in specs:
        target=s['position_m']
        if s['intensity_class']=='door' and lamps:
            s['position_m']=min(lamps,key=lambda p:sum((p[i]-target[i])**2 for i in range(3)))
        elif s['intensity_class']=='window' and panes:
            if k.meta['stock']=='lookouttower':
                top=[p for p in panes if p[2]>k.meta['size'][2]*.75]
                s['position_m']=min(top,key=lambda p:sum((p[i]-target[i])**2 for i in range(3)))
            else:
                s['position_m']=min(panes,key=lambda p:sum((p[i]-target[i])**2 for i in range(3)))
            s['position_m']=tuple(s['position_m'][i]-.025*s['direction'][i] for i in range(3))
        length=sum(v*v for v in s['direction'])**.5
        s['direction']=tuple(v/length for v in s['direction'])
    return specs
