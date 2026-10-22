"""Eight stock selections, seven authored meshes. References donate detail only.

No source mesh, texture, UV or vertex data is read here. Dimensions are catalog
bounds / 4 (authoring metres); generators reuse the kit's language and parts.
"""
import math
import random

import geo
from geo import Kit, rect_poly

SIZES = {
    'razorwirefence': (2.15365, 10.77768, 6.83865),
    's_lightwall': (5.04235, 34.52278, 5.57268),
    'rpbuilding': (30.82582, 26.51732, 8.5168),
    'quonsetclean': (10.52915, 21.05695, 5.26457),
    'crateswithtarp': (10.6255, 3.77875, 4.24323),
    'lookouttower': (9.98818, 9.07228, 18.6375),
    'barrels': (1.81450, 3.69078, 1.73127),
    'quonset': (10.52915, 21.05695, 5.26457),
}
FIT_CACHE={}


def light_wall(k):
    # Narrow cast wall run with a single terminal pier, like the stock lot.
    length, width, h = 34.0, 1.75, 4.65
    for j in range(8):
        cy = -length/2+(j+.5)*length/8
        k.box((0, cy, h/2), (width, length/8-.05, h), 'ferro', 'mass', ch=.12, top=.16, rebar=True)
        k.box((0, cy, h+.12), (width+.12, length/8-.075, .24), 'paint.main', 'skin', ch=.08, top=.05)
        for s in (-1, 1):
            k.box((s*(width/2+.025),cy,.40),(.09,length/8-.06,.28),'paint.second','detail')
            for yy in (cy-length/16+.3, cy+length/16-.3):
                k.box((s*(width/2+.035),yy,h-.28),(.07,.10,.10),'metal','detail')
        # Recessed casting seams rather than decorative oversized panels.
    py = length/2-1.55
    k.prism(rect_poly(-2.5,py-2.0,2.5,py+2.0,.42),0,2.1,'ferro','mass',top=.32,rebar=True)
    k.prism(rect_poly(-2.18,py-1.68,2.18,py+1.68,.36),2.1,5.5,'ferro','mass',top=.45,rebar=True)
    k.box((0,py,5.47),(3.45,2.6,.18),'paint.trim','detail',ch=.22,top=.05)
    with k.at((-2.19,py,2.65),yaw=-math.pi/2):
        k.service_panel(w=.72,h=.44)
    k.hits.append(dict(p=(0,-5,2.4),d=(1,0,0),r=2.3,depth=.65,sag=.5,rebar=True,debris=['ferro','metal']))


def fence(k):
    length, h = 10.6, 5.7
    for y in (-length/2, length/2):
        k.box((0,y,.14),(.65,.65,.28),'ferro','mass',ch=.08,top=.05)
        k.box((0,y,h/2),(.15,.18,h),'metal','frame',ch=.025)
        for s in (-1,1):
            k.beam((0,y,h-.3),(s*.95,y,h+.7),.09,'metal','frame')
        for z in (.35,2.8,5.5):
            k.box((0,y,z),(.24,.25,.14),'paint.trim','detail',ch=.025)
            k.box((-.14,y,z),(.08,.09,.08),'metal','detail')
    # Stock retains the open horizontal wire rhythm; modest twisted strands
    # and diagonal tension ties replace its texture planes.
    for z in [.55+j*.62 for j in range(9)]:
        for s in (-1,1):
            pts=[(s*.025*math.cos(j*.6),-length/2+length*j/90,z+.025*math.sin(j*.6)) for j in range(91)]
            k.tube(pts,.023,'metal','skin',sides=8,removable=True)
    for x in (-.9,-.45,0,.45,.9):
        k.tube([(x,-length/2,h+.7),(x,length/2,h+.7)],.027,'metal','skin',sides=10,removable=True)
    for y in [-length/2+j*.48 for j in range(23)]:
        k.tube([(-.94,y,h+.72),(.94,min(length/2,y+.35),h+.72)],.022,'metal','skin',sides=8,removable=True)
        for x in (-.9,0,.9):
            k.beam((x-.09,y-.05,h+.72),(x+.09,y+.05,h+.82),.026,'metal','detail')
    for z0,z1 in ((.55,2.8),(2.8,5.5)):
        k.tube([(0,-length/2,z0),(0,0,z1),(0,length/2,z0)],.022,'metal','skin',sides=8,removable=True)
    k.hits.append(dict(p=(0,-2.5,3.1),d=(1,0,0),r=2.5,depth=.4,sag=.5,rebar=False,debris=['metal']))


def clean_quonset(k):
    from assemble import end_wall
    geo.p_quonset_shell(k,length=21.0,radius=4.85,window_stride=2)
    k.hits.clear()
    for sign in (-1,1):
        with k.at((0,sign*10.5,0),yaw=0 if sign<0 else math.pi):
            end_wall(k,4.85,.45,door=True)
            # Recessed high end ventilator with drip hood.
            k.service_panel(0,3.75,w=.75,h=.42)
    # Lap fasteners along arch straps; no giant greeble roof silhouette.
    for y in (-10.5,-7.5,-4.5,-1.5,1.5,4.5,7.5,10.5):
        for j in range(1,16):
            a=math.pi*j/16
            k.box((4.94*math.cos(a),y,.45+4.94*math.sin(a)),(.045,.045,.045),'metal','detail')
    k.hits.extend([
        dict(p=(-2.6,-2.0,4.0),d=(.5,0,-1),r=2.0,depth=.7,sag=.65,rebar=False,debris=['corr.corr','metal'],floor=0.0),
        dict(p=(1.6,-10.5,2.2),d=(0,1,0),r=1.15,depth=.4,sag=.3,rebar=False,debris=['corr.main','paint.second'])])


def repair_facility(k):
    from assemble import sides, rollup, infantry_door_parts
    w,d,h=26.4,17.0,6.8
    rise=1.50
    for i,(o,yaw,length) in enumerate(sides(w,d)):
        holes = [(-2.87,0,2.87,6.5)] if i in (0,2) else []
        with k.at(o,yaw=yaw):
            geo.wall_segment(k,-length/2,length/2,h,holes,style='corr',mat='corr.main')
            if i in (0,2):
                for x in (0,):
                    with k.at((x,0,0)):rollup(k,5.74,6.5)
            else:
                # The stock Doors node contains four elevated service closures,
                # two per side: one wide and one narrow. They are not extra
                # ground-level entry bays.
                for cx,pw in ((-2.5,8.6),(5.2,4.4)):
                    k.slab(cx-pw/2-.18,cx+pw/2+.18,2.25,6.80,-.14,.08,'paint.trim','frame',front=.06)
                    k.slab(cx-pw/2,cx+pw/2,2.40,6.60,-.18,-.13,'paint.second','skin',front=.03)
                    for zz in (2.9,4.4,5.9):
                        k.slab(cx-pw*.43,cx+pw*.43,zz,zz+.12,-.24,-.17,'metal','frame',front=.02)
    # Low double-pitch roof: three stock-readable ribs run front to back.
    for sx in (-1,1):
        for j in range(8):
            x0=sx*w*j/16;x1=sx*w*(j+1)/16
            z0=h+rise*(1-abs(x0)/(w/2));z1=h+rise*(1-abs(x1)/(w/2))
            # Straight manufactured corrugated sheets, authored on this slope.
            with k.at(((x0+x1)/2,0,(z0+z1)/2),roll=math.atan2(z0-z1,abs(x1-x0))*(1 if sx>0 else -1)):
                k.box((0,0,-.08),(math.hypot(x1-x0,z1-z0)+.04,d,.32),'corr.main','roof',ch=.02,removable=True,slab=(sx,j))
                for yy in range(48):
                    k.box((0,-d/2+(yy+.5)*d/48,.11),(abs(x1-x0)+.025,.075,.06),'metal','detail',ch=.02)
    # Close the end gables under the pitch; roof sheets are supported at eaves
    # and ridge rather than floating above a rectangular wall.
    for y in (-d/2, d/2):
        k.prism([(-w/2,h-.05),(w/2,h-.05),(0,h+rise)],y-.16,y+.16,
                'corr.main','skin',axis='y',removable=True)
    for x in (-w/2,-w/6,w/6,w/2):
        z=h+rise*(1-abs(x)/(w/2))+.12
        k.box((x,0,z),(.42,d+.3,.36),'paint.main','frame',ch=.09,top=.08)
        for y in (-d/2,d/2):
            with k.at((x,y,0)):
                k.prism(rect_poly(-.55,-.7,.55,.7,.20),0,h,'paint.main','frame',top=.13)
                outer=abs(x)>w*.4
                sx=(-1 if x<0 else 1)*1.25 if outer else 0
                sy=(-1 if y<0 else 1)*4.1 if outer else 0
                k.prism(rect_poly(sx-1.0,sy-.65,sx+1.0,sy+.65,.20),0,1.0,'ferro','mass',top=.2)
                if outer:
                    # Stock has a solid splayed buttress, not an extra open
                    # A-frame leg beside each arch end.
                    length=math.hypot(sx,sy);nx,ny=-sy/length,sx/length
                    profile=[(0,0,0),(sx,sy,0),(0,0,h-.25)]
                    vv=[(px+side*.40*nx,py+side*.40*ny,pz)
                        for side in (-1,1) for px,py,pz in profile]
                    k.add(vv,[(2,1,0),(3,4,5),(0,1,4,3),(1,2,5,4),(2,0,3,5)],
                          'paint.main','frame')
    k.box((0,0,.06),(w-.8,d-.8,.12),'ferro','mass')
    k.hits.extend([
        dict(p=(-7,-8.5,3.5),d=(0,1,0),r=2.8,depth=.8,sag=.65,rebar=True,debris=['ferro','corr.main','metal']),
        dict(p=(-5,-4,7.7),d=(0,0,-1),r=3.8,depth=1.1,sag=1.2,rebar=False,debris=['corr.main','metal'],collapse='roof',floor=0.0)])


def tarp_crates(k):
    # Two separate timber cases and draped canvases, matching the stock pair.
    for b in range(2):
        x=(b-.5)*5.25
        k.box((x,0,1.55),(5.1,3.6,3.1),'wood.main','mass',ch=.07,top=.04)
        if b in (0,1):
            endx=x+(-2.59 if b==0 else 2.59)
            for j in range(9):
                k.box((endx,0,.2+j*.30),(.10,3.5,.26),'wood.main','skin',ch=.02,removable=True)
            for yy in (-1.48,1.48):
                k.box((endx,yy,1.55),(.16,.17,2.95),'wood.second','frame',ch=.025)
        for side in (-1,1):
            for j in range(9):
                zz=.2+j*.30
                k.box((x,side*1.82,zz),(4.94,.10,.26),'wood.main','skin',ch=.02,removable=True)
            for xx in (-2.35,2.35):
                k.box((x+xx,side*1.91,1.55),(.19,.15,2.95),'wood.second','frame',ch=.035)
            k.beam((x-2.22,side*1.91,.35),(x+2.22,side*1.91,2.8),.12,'wood.second','frame')
            k.box((x,side*1.94,1.55),(.8,.025,.32),'paint.trim','detail',ch=.02)
            for xx in (-2.35,2.35):
                for z in (.35,1.55,2.8):
                    k.box((x+xx,side*2.0,z),(.055,.035,.055),'metal','detail')
        # Canvas with broad tension folds and a sewn hem, each case one mesh.
        verts=[];faces=[];nx,ny=28,24
        for j in range(ny+1):
            y=-1.95+3.9*j/ny
            for i in range(nx+1):
                xx=-2.67+5.34*i/nx
                edge=max(abs(y)/1.95,abs(xx)/2.67)
                z=3.22-.045*(xx/2.67)**2-.70*max(0,(edge-.94)/.06)**1.1
                z+=.055*math.cos(xx*13+y*2)*edge**3
                verts.append((x+xx,y,z))
        for j in range(ny):
            for i in range(nx):
                a=j*(nx+1)+i;faces.append((a,a+1,a+nx+2,a+nx+1))
        # Keep both covers draped in the abandoned condition too. Generic
        # fallen-panel dressing leaves a detached cover beside a false third
        # cover outline when its sewn hem remains on the case.
        k.add(verts,faces,'canvas.corr','skin',canvas=True)
        for side in (-1,1):
            pts=[verts[(0 if side<0 else ny)*(nx+1)+i] for i in range(nx+1)]
            k.tube(pts,.025,'canvas.corr','detail',sides=8,canvas=True)
            for xx in (-1.4,1.4):
                k.tube([(x+xx,side*1.94,2.5),(x+xx,side*1.96,.35)],.025,'rubber','detail')
    k.hits.append(dict(p=(-2,-1.8,2),d=(0,1,0),r=2.2,depth=.6,sag=.5,rebar=False,debris=['paint.main','metal']))


def hazard_barrels(k):
    # Three drums are visible in the stock model: a shallow stagger, not a stack.
    for x,y in ((-.22,-1.10),(.22,0),(-.22,1.10)):
        with k.at((x,y,0)):
            k.cyl((0,0,.84),.69,1.68,'paint.main','mass',n=48)
            for z in (.08,.34,1.30,1.61):
                k.cyl((0,0,z),.72,.075,'paint.second','frame',n=48)
            # Authored diagonal paint, with no copied label or decal.
            k.cyl((0,0,.84),.701,.55,'hazard','skin',sides=48)
            k.cyl((0,0,1.70),.62,.025,'paint.main','skin',n=48)
            k.cyl((.29,.1,1.725),.07,.025,'metal','detail',n=16)
            k.cyl((-.2,-.15,1.725),.035,.025,'metal','detail',n=12)


def lookout(k):
    from assemble import sides, roof_deck
    # Measured stock height bands: flared base 0..4.47, shaft 4.49..15.21,
    # cabin 15.2..17.94, roof 18.16..18.64. Keep the smaller stock crown.
    w,d=9.8,8.9
    def loft(bw,bd,tw,td,z0,z1,mat,role):
        bottom=rect_poly(-bw/2,-bd/2,bw/2,bd/2,.25)
        top=rect_poly(-tw/2,-td/2,tw/2,td/2,.18)
        vv=[(x,y,z0) for x,y in bottom]+[(x,y,z1) for x,y in top]
        ff=[tuple(reversed(range(8))),tuple(range(8,16))]
        ff += [(i,(i+1)%8,(i+1)%8+8,i+8) for i in range(8)]
        k.add(vv,ff,mat,role,rebar=True)
    loft(w,d,5.90,5.33,0,4.47,'ferro','mass')
    k.box((0,0,4.49),(7.91,7.20,.16),'paint.trim','frame',ch=.22,top=.05)
    loft(5.90,5.33,3.89,3.50,4.60,15.20,'ferro','mass')
    for x in (-1.8,1.8):
        for y in (-1.5,1.5):
            k.beam((x,y,4.6),(x*.8,y*.8,15.2),.24,'metal','frame')
    for z in (4.8,8.1,11.4,14.7):
        k.box((0,0,z),(3.9,3.3,.18),'metal','frame',ch=.1)
        for x in (-1.8,1.8):
            k.beam((x,-1.5,z),(x*.9,1.4,min(15.2,z+3.3)),.12,'metal','frame')
    for i,(o,yaw,length) in enumerate(sides(5.90,5.33)):
        batter=(1.005 if i%2 else .915)/10.6
        with k.at((o[0],o[1],4.6),yaw=yaw,pitch=-math.atan(batter)):
            # Front/rear texture has one inset; left/right has two narrow ones.
            for cx in ((-.62,.62) if i%2 else (0,)):
                half=.275 if i%2 else .80
                k.slab(cx-half,cx+half,.3,10.3,-.065,.02,'paint.second','skin',front=.04,removable=True)
                for x in (cx-half-.12,cx+half+.12):
                    k.slab(x-.08,x+.08,.2,10.4,-.14,.03,'paint.main','frame',front=.035)
                for z in (1.1,3.0,4.9,6.8,8.7,10.0):
                    k.slab(cx-half+.05,cx+half-.05,z,z+.06,-.10,-.06,'metal','detail')
            k.service_panel(0,1.7,w=.5,h=.4)
    for o,yaw,length in sides(5.90,5.37):
        with k.at((o[0],o[1],15.2),yaw=yaw):
            step=length*.8/3
            holes=[(j*step-step*.41,.65,j*step+step*.41,1.9) for j in (-1,0,1)]
            geo.wall_segment(k,-length/2,length/2,2.74,openings=holes,plinth=False)
            for x0,z0,x1,z1 in holes:k.window(x0,x1,z0,z1,0,mullions=0)
    roof_deck(k,5.90,5.37,17.94,(2,2))
    k.box((0,0,18.39),(7.55,6.87,.48),'paint.main','roof',ch=.18,top=.06)
    k.box((0,0,18.64),(6.95,6.27,.035),'gravel','roof',ch=.12)
    for x in (-3.60,3.60):
        k.box((x,0,18.66),(.16,6.57,.10),'paint.trim','detail',ch=.03)
    for y in (-3.25,3.25):
        k.box((0,y,18.66),(7.2,.16,.10),'paint.trim','detail',ch=.03)
    k.box((.6,.45,18.67),(1.05,.75,.04),'grate','detail',ch=.06)
    # Source repeats one door-bearing UV rectangle on all four battered faces.
    # Follow that plane; compact thresholds do not introduce staircases.
    for i,(o,yaw,length) in enumerate(sides(w,d)):
        batter=((w-5.90) if i%2 else (d-5.33))/2/4.47
        with k.at(o,yaw=yaw):
            with k.at((0,batter*.25,.25),pitch=-math.atan(batter)):
                k.reveal(-1.0,1.0,0,3.6,-.03,depth=.25,sill=False)
                k.slab(-.98,.98,0,3.6,-.11,-.04,'paint.second','skin',front=.02)
                k.slab(-.4,.4,2.4,3.0,-.14,-.12,'glass','glass',pane=True)
                k.slab(-1.15,1.15,-.10,0,-.17,.04,'ferro','mass',front=.02)
                k.lamp_head(0,3.9,.05,w=.40)
    k.hits.extend([
        dict(p=(-2.7,-1.5,8.5),d=(1,0,0),r=1.4,reach=1.3,depth=.45,sag=.2,rebar=True,debris=['ferro','paint.second','metal']),
        dict(p=(-2,-2.7,16.5),d=(0,1,0),r=1.5,reach=1.8,depth=.5,sag=.35,rebar=True,debris=['ferro','metal'])])


DRAFTS = {
    'razorwirefence': (fence,'galvanised'),
    's_lightwall': (light_wall,'wall_cast'),
    'rpbuilding': (repair_facility,'repair_sand'),
    'quonsetclean': (clean_quonset,'quonset_clean'),
    'crateswithtarp': (tarp_crates,'canvas_timber'),
    'lookouttower': (lookout,'barracks_sage'),
    'barrels': (hazard_barrels,'hazard_yellow'),
    'quonset': (clean_quonset,'quonset_rust'),
}


def full_rubble(k, name):
    """A grounded collapsed assembly, with debris in its own material palette."""
    rng=random.Random(k.seed)
    w,d,h=SIZES[name]
    original=list(k.pieces)
    k.pieces=[];k.hits=[]
    # Large broken slabs/panels form a low central heap, not a scaled building.
    slabmat='corr.corr' if name.startswith('quonset') else ('corr.main' if name=='rpbuilding' else 'ferro')
    heap_h=min(h*.16,2.6) if name=='lookouttower' else min(h*.12,1.3)
    # Continuous aggregate under the fragments supplies their load path. A low
    # radial mound avoids isolated slabs hovering above otherwise bare ground.
    verts=[(0,0,heap_h*.58)];faces=[];n=24
    for ring,radius in enumerate((.33,.66,1.0)):
        for j in range(n):
            a=j*math.tau/n
            jitter=.91+.08*math.sin(j*7.13)
            verts.append((w*.49*radius*jitter*math.cos(a),d*.49*radius*jitter*math.sin(a),
                          max(.025,heap_h*.58*(1-radius)**1.1)))
    for j in range(n):faces.append((0,1+j,1+(j+1)%n))
    for r in range(2):
        a=1+r*n;b=a+n
        for j in range(n):faces.append((a+j,b+j,b+(j+1)%n,a+(j+1)%n))
    k.add(verts,faces,'ferro' if name=='lookouttower' else 'gravel','mass')
    for i in range(72):
        x=rng.uniform(-w*.43,w*.43);y=rng.uniform(-d*.43,d*.43)
        t=max(0,1-math.hypot(x/(w*.55),y/(d*.55)))
        z=heap_h*.54*t**1.1
        rw=rng.uniform(.25,max(.4,w*.14));rd=rng.uniform(.25,max(.4,d*.12))
        mat=slabmat if i%3 else ('paint.second' if name=='lookouttower' else 'paint.main')
        with k.at((x,y,z),yaw=rng.uniform(-math.pi,math.pi)):
            poly=[(rw*math.cos(a)*rng.uniform(.72,1),rd*math.sin(a)*rng.uniform(.72,1))
                  for a in [j*math.tau/5 for j in range(5)]]
            thickness=rng.uniform(.10,.32) if name=='lookouttower' else rng.uniform(.025,.075)
            k.prism(poly,0,thickness,mat,'mass',top=.012)
    if name!='lookouttower':
        # Bent, visibly corrugated roofing identifies the metal buildings among
        # the smaller substrate fragments. These are new authored sheets.
        for i in range(16):
            x=rng.uniform(-w*.34,w*.34);y=rng.uniform(-d*.34,d*.34)
            t=max(0,1-math.hypot(x/(w*.55),y/(d*.55)))
            sw=rng.uniform(w*.06,w*.16);sd=rng.uniform(d*.04,d*.12)
            vv=[];ff=[];nx,ny=32,4
            for j in range(ny+1):
                yy=(j/ny-.5)*sd
                for q in range(nx+1):
                    xx=(q/nx-.5)*sw
                    zz=.07+.035*math.sin(q*math.pi/2)+.22*abs(yy)+.08*xx
                    vv.append((xx,yy,zz))
            for j in range(ny):
                for q in range(nx):
                    a=j*(nx+1)+q;ff.append((a,a+1,a+nx+2,a+nx+1))
            with k.at((x,y,heap_h*.54*t**1.1),yaw=rng.uniform(-math.pi,math.pi)):
                k.add(vv,ff,'corr.corr' if name.startswith('quonset') else 'corr.main','skin')
    # Recognisable authored panels and structural members are laid on that heap.
    candidates=[p for p in original if p['role'] in ('roof','frame','skin')
                and len(p['v'])<1500 and not p.get('pane')]
    for p in rng.sample(candidates,min(38,len(candidates))):
        p=dict(p,v=list(p['v']));c=geo.centroid(p)
        geo.lay_flat(k,p,(rng.uniform(-w*.30,w*.30),rng.uniform(-d*.30,d*.30),.1),
                     rng.uniform(-math.pi,math.pi),rng)
        # Reject whole-building members; retain local broken pieces only.
        lo=[min(v[i] for v in p['v']) for i in range(3)]
        hi=[max(v[i] for v in p['v']) for i in range(3)]
        if hi[0]-lo[0]>w*.55 or hi[1]-lo[1]>d*.55:continue
        k.pieces.append(p)
    for i in range(22):
        x=rng.uniform(-w*.40,w*.40);y=rng.uniform(-d*.40,d*.40)
        with k.at((x,y,.2),yaw=rng.uniform(-math.pi,math.pi),pitch=rng.uniform(-.18,.18)):
            k.box((0,0,.06),(rng.uniform(.5,min(3,w*.4)),.12,.12),'metal','frame',ch=.02)
    # All pieces touch ground or lie within the pile; no unsupported roof islands.
    for p in k.pieces:
        p['v']=[(max(-w*.5,min(w*.5,x)),max(-d*.5,min(d*.5,y)),max(.025,z)) for x,y,z in p['v']]
    k.hits=[dict(p=(0,0,.3),d=(0,0,-1),r=min(w,d)*.18,depth=0,sag=0)]
    k.meta['authored_damage']=True
    k.meta['collapse']='full-rubble'


def fit_stock(k,name):
    pts=[v for pc in k.pieces for v in pc['v']]
    lo=[min(v[i] for v in pts) for i in range(3)]
    hi=[max(v[i] for v in pts) for i in range(3)]
    scales=[SIZES[name][i]/(hi[i]-lo[i]) for i in range(3)]
    center=[(lo[i]+hi[i])/2 for i in range(2)]+[lo[2]]
    if geo.GAME_MESH and name in FIT_CACHE:
        # The game carrier must occupy the same surface as the bake source.
        # Removing an aerial vent must not rescale the entire roof upwards.
        scales,center=FIT_CACHE[name]
    elif not geo.GAME_MESH:FIT_CACHE[name]=(scales,center)
    def point(v): return tuple((v[i]-center[i])*scales[i] for i in range(3))
    for pc in k.pieces: pc['v']=[point(v) for v in pc['v']]
    for hit in k.hits:
        hit['p']=point(hit['p']);hit['r']*=sum(scales)/3
    k.moss_sites=[tuple(point(v) for v in pair) for pair in k.moss_sites]
    k.meta['fit_scale_xyz']=scales


def build(name,condition,seed=17):
    import counts
    if name not in counts.CONTRACTS:
        raise ValueError(f'{name}: record stock model/texture counts before building')
    fn,palette=DRAFTS[name]
    k=Kit(name,palette,seed);fn(k);fit_stock(k,name)
    if condition=='abandoned':geo.apply_abandoned(k,geo.LANG['conditions'][condition])
    buildings={'rpbuilding','quonsetclean','quonset','lookouttower'}
    if condition=='destroyed' and name in buildings:
        full_rubble(k,name)
    elif condition in ('partial','destroyed'):
        if name=='razorwirefence':
            # The entire stock section topples, retaining posts and wire.
            for pc in k.pieces:
                pc['v']=[(z*.98+x*.1,y,max(.07,.14+z*.08-x*.35)) for x,y,z in pc['v']]
            k.meta['authored_damage']=True
        elif name=='s_lightwall' and condition=='destroyed':
            # Stock damage shortens the run to a breached stub beside its pier.
            edge=SIZES[name][1]/2-10.5
            k.pieces=[p for p in k.pieces if max(v[1] for v in p['v'])>edge]
            for p in k.pieces:
                p['v']=[(x,max(edge,y),min(z,2.0+abs(max(edge,y)-edge)*1.5+.2*math.sin(y*7+x*3)))
                        if y<edge+2 else (x,y,z) for x,y,z in p['v']]
            k.hits=[dict(p=(0,edge,2.1),d=(0,1,0),r=2.2,depth=0,sag=0)]
            k.meta['authored_damage']=True
        elif name=='crateswithtarp':
            rng=random.Random(seed)
            for pc in list(k.pieces):
                c=geo.centroid(pc)
                if condition=='destroyed' or c[0]<-.5:
                    if pc.get('canvas'):
                        pc['v']=[(x,y,max(.15,z*(.18 if condition=='destroyed' else .28)+.12*math.sin(x*3))) for x,y,z in pc['v']]
                    elif pc['role']=='skin' and c[1]<0:
                        k.pieces.remove(pc)
                        geo.lay_flat(k,pc,(c[0],-2.5,0),rng.uniform(-.6,.6),rng)
                        k.pieces.append(pc)
                    else:
                        pc['v']=[(x+.3*z,y,max(.08,z*(.23 if condition=='destroyed' else .35))) for x,y,z in pc['v']]
            k.meta['authored_damage']=True
        elif name=='barrels':
            # Keep the three drums recognisable, but crush their shells and fold
            # their rims as linked assemblies rather than scattering whole lids.
            for pc in k.pieces:
                pc['v']=[(x+.24*z,y+.10*z*math.sin(y*2),
                          max(.04,z*.28+.04*math.sin(x*7+y*3))) for x,y,z in pc['v']]
            k.hits=[dict(p=(0,0,.3),d=(0,0,-1),r=2.5,depth=0,sag=0)]
            k.meta['authored_damage']=True
        else:geo.apply_destroyed(k)
        k.pieces=[pc for pc in k.pieces if not pc.get('pane')]
    k.meta.update(name=name,stock=name,condition=condition,palette=palette,
                  size=SIZES[name],draft=True,original_geometry=True,
                  ref='alina-godfrey-battletech-urbanwarfare-props-5.jpg',round1=None)
    k.meta['stock_origin_offset_m']=counts.origin(name)
    return k
