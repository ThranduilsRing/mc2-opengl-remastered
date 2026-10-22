"""Six original stock-identity blends. Reference images are review-only inputs.

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
}


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
    for y in (-length/2, 0, length/2):
        k.box((0,y,.14),(.65,.65,.28),'ferro','mass',ch=.08,top=.05)
        k.box((0,y,h/2),(.15,.18,h),'metal','frame',ch=.025)
        for s in (-1,1):
            k.beam((0,y,h-.3),(s*.95,y,h+.7),.09,'metal','frame')
        for z in (.35,2.8,5.5):
            k.box((0,y,z),(.24,.25,.14),'paint.trim','detail',ch=.025)
            k.box((-.14,y,z),(.08,.09,.08),'metal','detail')
    # Stock retains the open horizontal wire rhythm; modest twisted strands
    # and diagonal tension ties replace its texture planes.
    for z in [.55+j*.71 for j in range(8)]:
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
            end_wall(k,4.85,.45,door=sign<0)
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
    w,d,h=28.5,24.8,6.6
    for i,(o,yaw,length) in enumerate(sides(w,d)):
        holes = [(-9.8,0,-3.2,4.7),(3.2,0,9.8,4.7)] if i==0 else ([(-1.0,.45,1.0,3.2)] if i==1 else [])
        with k.at(o,yaw=yaw):
            geo.wall_segment(k,-length/2,length/2,h,holes,style='corr',mat='corr.main')
            if i==0:
                for x in (-6.5,6.5):
                    with k.at((x,0,0)):rollup(k,6.6,4.7)
            elif i==1:
                infantry_door_parts(k,2.0,2.75)
            else:
                k.service_panel(-length*.24,2.6,w=1.4,h=.9)
    # Low double-pitch roof: three stock-readable ribs run front to back.
    for sx in (-1,1):
        for j in range(8):
            x0=sx*w*j/16;x1=sx*w*(j+1)/16
            z0=h+1.05*(1-abs(x0)/(w/2));z1=h+1.05*(1-abs(x1)/(w/2))
            # Straight manufactured corrugated sheets, authored on this slope.
            with k.at(((x0+x1)/2,0,(z0+z1)/2),roll=math.atan2(z0-z1,abs(x1-x0))*(1 if sx>0 else -1)):
                k.box((0,0,-.08),(math.hypot(x1-x0,z1-z0)+.04,d,.32),'corr.main','roof',ch=.02,removable=True,slab=(sx,j))
                for yy in range(48):
                    k.box((0,-d/2+(yy+.5)*d/48,.11),(abs(x1-x0)+.025,.075,.06),'metal','detail',ch=.02)
    # Close the end gables under the pitch; roof sheets are supported at eaves
    # and ridge rather than floating above a rectangular wall.
    for y in (-d/2, d/2):
        k.prism([(-w/2,h-.05),(w/2,h-.05),(0,h+1.05)],y-.16,y+.16,
                'corr.main','skin',axis='y',removable=True)
    for x in (-w/2,-w/6,w/6,w/2):
        z=h+1.05*(1-abs(x)/(w/2))+.12
        k.box((x,0,z),(.42,d+.3,.36),'paint.main','frame',ch=.09,top=.08)
        for y in (-d/2,d/2):
            with k.at((x,y,0)):
                k.prism(rect_poly(-.55,-.7,.55,.7,.20),0,h,'paint.main','frame',top=.13)
                k.prism(rect_poly(-.85,-1.1,.85,1.1,.20),0,1.0,'ferro','mass',top=.2)
                # Load-bearing brace at end, sloping toward the building.
                k.beam((0,(-1 if y<0 else 1)*2.0,.7),(0,0,h-.25),.52,'paint.main','frame',h=.7)
    k.box((0,0,.06),(w-.8,d-.8,.12),'ferro','mass')
    k.hits.extend([
        dict(p=(-7,-12.4,3.5),d=(0,1,0),r=2.8,depth=.8,sag=.65,rebar=True,debris=['ferro','corr.main','metal']),
        dict(p=(-5,-4,7.3),d=(0,0,-1),r=3.8,depth=1.1,sag=1.2,rebar=False,debris=['corr.main','metal'],collapse='roof',floor=0.0)])


def tarp_crates(k):
    # Three timber cases under one military canvas: retain the green/brown read.
    for b in range(3):
        x=(b-1)*3.45
        k.box((x,0,1.55),(3.35,3.6,3.1),'wood.main','mass',ch=.07,top=.04)
        if b in (0,2):
            endx=x+(-1.71 if b==0 else 1.71)
            for j in range(9):
                k.box((endx,0,.2+j*.30),(.10,3.5,.26),'wood.main','skin',ch=.02,removable=True)
            for yy in (-1.48,1.48):
                k.box((endx,yy,1.55),(.16,.17,2.95),'wood.second','frame',ch=.025)
        for side in (-1,1):
            for j in range(9):
                zz=.2+j*.30
                k.box((x,side*1.82,zz),(3.18,.10,.26),'wood.main','skin',ch=.02,removable=True)
            for xx in (-1.48,1.48):
                k.box((x+xx,side*1.91,1.55),(.19,.15,2.95),'wood.second','frame',ch=.035)
            k.beam((x-1.35,side*1.91,.35),(x+1.35,side*1.91,2.8),.12,'wood.second','frame')
            k.box((x,side*1.94,1.55),(.8,.025,.32),'paint.trim','detail',ch=.02)
            for xx in (-1.48,1.48):
                for z in (.35,1.55,2.8):
                    k.box((x+xx,side*2.0,z),(.055,.035,.055),'metal','detail')
        # Canvas with broad tension folds and a sewn hem, each case one mesh.
        verts=[];faces=[];nx,ny=28,24
        for j in range(ny+1):
            y=-1.95+3.9*j/ny
            for i in range(nx+1):
                xx=-1.72+3.44*i/nx
                edge=max(abs(y)/1.95,abs(xx)/1.72)
                z=3.8-.10*(xx/1.72)**2-1.2*max(0,(edge-.65)/.35)**1.6
                z+=.055*math.cos(xx*13+y*2)*edge**3
                verts.append((x+xx,y,z))
        for j in range(ny):
            for i in range(nx):
                a=j*(nx+1)+i;faces.append((a,a+1,a+nx+2,a+nx+1))
        k.add(verts,faces,'canvas.corr','skin',removable=True,canvas=True)
        for side in (-1,1):
            pts=[verts[(0 if side<0 else ny)*(nx+1)+i] for i in range(nx+1)]
            k.tube(pts,.025,'canvas.corr','detail',sides=8,canvas=True)
            for xx in (-1.4,1.4):
                k.tube([(x+xx,side*1.94,2.5),(x+xx,side*1.96,.35)],.025,'rubber','detail')
    k.hits.append(dict(p=(-2,-1.8,2),d=(0,1,0),r=2.2,depth=.6,sag=.5,rebar=False,debris=['paint.main','metal']))


def lookout(k):
    from assemble import sides, roof_deck, infantry_door_parts
    w,d=9.8,8.9
    k.prism(rect_poly(-w/2,-d/2,w/2,d/2,.4),0,3.0,'ferro','mass',top=.85,top_h=2.5,rebar=True)
    k.box((0,0,3.05),(8.25,7.3,.35),'paint.trim','frame',ch=.25,top=.08)
    k.prism(rect_poly(-3.35,-2.9,3.35,2.9,.28),3.2,15.1,'ferro','mass',top=1.05,top_h=11.9,rebar=True)
    # A real surviving load path inside the cladding, exposed by local damage.
    for x in (-1.8,1.8):
        for y in (-1.5,1.5):
            k.beam((x,y,3.2),(x*.8,y*.8,15.2),.24,'metal','frame')
    for z in (3.4,7.0,10.6,14.2):
        k.box((0,0,z),(3.9,3.3,.18),'metal','frame',ch=.1)
        for x in (-1.8,1.8):
            k.beam((x,-1.5,z),(x*.9,1.4,min(15.0,z+3.5)),.12,'metal','frame')
    # Battered sage inserts, side ribs and expansion joints keep stock identity.
    tilt=math.atan(1.05/11.9)
    for i,(o,yaw,length) in enumerate(sides(6.7,5.8)):
        with k.at((o[0],o[1],3.3),yaw=yaw,pitch=-tilt):
            k.slab(-.9,.9,.3,11.5,-.065,.02,'paint.second','skin',front=.04,removable=True)
            for x in (-1.12,1.12):
                k.slab(x-.12,x+.12,.2,11.6,-.14,.03,'paint.main','frame',front=.035)
            for z in (1.2,3.2,5.2,7.2,9.2,11.0):
                k.slab(-.8,.8,z,z+.06,-.10,-.06,'metal','detail')
            k.service_panel(0,1.9,w=.8,h=.55)
    for i,(o,yaw,length) in enumerate(sides(8.3,7.4)):
        with k.at((o[0],o[1],15.0),yaw=yaw):
            geo.wall_segment(k,-length/2,length/2,2.55,openings=[(-length*.40,.65,length*.40,1.9)],plinth=False)
            k.window(-length*.40,length*.40,.65,1.9,0,mullions=4)
    roof_deck(k,8.3,7.4,17.3,(2,2))
    k.box((0,0,17.65),(9.8,8.9,.35),'paint.main','roof',ch=.22,top=.07)
    k.box((0,0,17.85),(8.95,8.05,.06),'gravel','roof',ch=.16)
    for x in (-4.65,4.65):
        k.box((x,0,17.91),(.22,8.55,.16),'paint.trim','detail',ch=.05)
    for y in (-4.2,4.2):
        k.box((0,y,17.91),(9.3,.22,.16),'paint.trim','detail',ch=.05)
    with k.at((1.6,1.2,17.85)): k.hvac(1.4,1.0,.48)
    with k.at((0,-4.62,0)):infantry_door_parts(k,1.2,2.25)
    k.hits.extend([
        dict(p=(-3.0,-1.5,8.0),d=(1,0,0),r=1.45,reach=1.3,depth=.45,sag=.2,rebar=True,debris=['ferro','paint.second','metal']),
        dict(p=(-2,-3.7,16.5),d=(0,1,0),r=1.7,reach=1.8,depth=.5,sag=.35,rebar=True,debris=['ferro','metal'])])


DRAFTS = {
    'razorwirefence': (fence,'galvanised'),
    's_lightwall': (light_wall,'wall_cast'),
    'rpbuilding': (repair_facility,'repair_sand'),
    'quonsetclean': (clean_quonset,'quonset_clean'),
    'crateswithtarp': (tarp_crates,'canvas_timber'),
    'lookouttower': (lookout,'barracks_sage'),
}


def fit_stock(k,name):
    pts=[v for pc in k.pieces for v in pc['v']]
    lo=[min(v[i] for v in pts) for i in range(3)]
    hi=[max(v[i] for v in pts) for i in range(3)]
    scales=[SIZES[name][i]/(hi[i]-lo[i]) for i in range(3)]
    center=[(lo[i]+hi[i])/2 for i in range(2)]+[lo[2]]
    def point(v): return tuple((v[i]-center[i])*scales[i] for i in range(3))
    for pc in k.pieces: pc['v']=[point(v) for v in pc['v']]
    for hit in k.hits:
        hit['p']=point(hit['p']);hit['r']*=sum(scales)/3
    k.moss_sites=[tuple(point(v) for v in pair) for pair in k.moss_sites]
    k.meta['fit_scale_xyz']=scales


def build(name,condition,seed=17):
    fn,palette=DRAFTS[name]
    k=Kit(name,palette,seed);fn(k);fit_stock(k,name)
    if condition=='abandoned':geo.apply_abandoned(k,geo.LANG['conditions'][condition])
    if condition=='destroyed':
        if name=='razorwirefence':
            # Tension loss: a local breach folds wire and the middle post toward
            # the ground. Thin wire uses authored collapse, not solid booleans.
            for pc in k.pieces:
                if pc['m']=='ferro':continue
                pc['v']=[(x+1.7*max(0,1-abs(y+1.8)/4)*(z/6),y,
                          max(.06,z*(1-.82*max(0,1-abs(y+1.8)/4)))) for x,y,z in pc['v']]
            k.meta['authored_damage']=True
        elif name=='crateswithtarp':
            rng=random.Random(seed)
            for pc in list(k.pieces):
                c=geo.centroid(pc)
                if c[0]<-.5:
                    if pc.get('canvas'):
                        pc['v']=[(x,y,max(.15,z*.28+.12*math.sin(x*3))) for x,y,z in pc['v']]
                    elif pc['role']=='skin' and c[1]<0:
                        k.pieces.remove(pc)
                        geo.lay_flat(k,pc,(c[0],-2.5,0),rng.uniform(-.6,.6),rng)
                        k.pieces.append(pc)
                    else:
                        pc['v']=[(x+.3*z,y,max(.08,z*.35)) for x,y,z in pc['v']]
            k.meta['authored_damage']=True
        else:geo.apply_destroyed(k)
        k.pieces=[pc for pc in k.pieces if not pc.get('pane')]
    k.meta.update(name=name,stock=name,condition=condition,palette=palette,
                  size=SIZES[name],draft=True,original_geometry=True,
                  ref='alina-godfrey-battletech-urbanwarfare-props-5.jpg',round1=None)
    return k
