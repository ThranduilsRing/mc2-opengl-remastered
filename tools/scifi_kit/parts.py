"""Original parameterized kit geometry. Coordinates are metres, Z up, front -Y.

Parts emit into one mesh to keep headless assembly cheap. A bevel modifier adds
the source edge catches; game LODs are derived from that evaluated source.
"""
import math
from collections import Counter
from contextlib import contextmanager


class MeshKit:
    def __init__(self, dimensions=None):
        self.vertices, self.faces, self.materials = [], [], []
        self.origin, self.yaw = (0, 0, 0), 0
        self.counts = Counter()
        self.dimensions = dimensions or {}

    def dimension(self, key, default):
        return self.dimensions.get(key, {}).get('value', default)

    @contextmanager
    def at(self, origin, yaw=0):
        old = self.origin, self.yaw
        self.origin, self.yaw = origin, yaw
        try:
            yield
        finally:
            self.origin, self.yaw = old

    def mesh(self, vertices, faces, material):
        start = len(self.vertices)
        c, s = math.cos(self.yaw), math.sin(self.yaw)
        ox, oy, oz = self.origin
        self.vertices.extend((ox+c*x-s*y, oy+s*x+c*y, oz+z) for x, y, z in vertices)
        self.faces.extend(tuple(start+i for i in face) for face in faces)
        self.materials.extend([material]*len(faces))

    def box(self, center, size, material='armour'):
        x, y, z = center
        w, d, h = [max(0.005, n)/2 for n in size]
        self.mesh([(x-w,y-d,z-h),(x+w,y-d,z-h),(x+w,y+d,z-h),(x-w,y+d,z-h),(x-w,y-d,z+h),(x+w,y-d,z+h),(x+w,y+d,z+h),(x-w,y+d,z+h)], [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)], material)

    def cylinder(self, center, radius, height, material='steel', sides=12):
        x, y, z = center
        vs = [(x+radius*math.cos(i*math.tau/sides), y+radius*math.sin(i*math.tau/sides), z+dz) for dz in [-height/2,height/2] for i in range(sides)]
        fs = [tuple(reversed(range(sides))),tuple(range(sides,2*sides))]+[(i,(i+1)%sides,(i+1)%sides+sides,i+sides) for i in range(sides)]
        self.mesh(vs,fs,material)

    def wall_panel(self, width=3, height=3.6, window=False, gap=0.055):
        self.counts['wall_panel'] += 1
        # Local front -Y. Split the actual wall around the glazing aperture.
        bottom, top = 0.50, height
        if window and height > 2.2 and width > 1.6:
            window_width,window_height=self.dimension('window_m',[1.6,.65])
            ww, wh = min(window_width,width*0.65), min(window_height,height*0.19)
            wz = height*0.65
            self.box((0,0.20,(bottom+wz-wh/2)/2),(width-gap,.34,wz-wh/2-bottom),'concrete')
            self.box((0,0.20,(wz+wh/2+top)/2),(width-gap,.34,top-wz-wh/2),'armour')
            sw = (width-ww)/2
            for sign in [-1,1]:
                self.box((sign*(ww/2+sw/2),.20,wz),(sw-gap,.34,wh),'armour')
            self.window_slit(ww,wh,wz)
        else:
            self.box((0,.24,(bottom+top)/2),(width-gap,.30,top-bottom),'concrete')
            rows = max(1,round((height-.5)/2.8))
            for j in range(rows):
                ph = (height-.5)/rows
                self.box((0,.035,.5+ph*(j+.5)),(width-gap,.18,ph-gap),'armour')
        self.box((0,-.045,.72),(width-gap,.18,.23),'trim')
        self.box((0,-.02,height-.28),(width-gap,.20,.18),'trim')

    def window_slit(self, width=1.6, height=.65, z=2.4):
        self.counts['window_slit'] += 1
        self.box((0,self.dimension('reveal_m',.24)+.005,z),(width,.075,height),'glass')
        for x in [-width/2,width/2]:
            self.box((x,.075,z),(.11,.36,height+.12),'steel')
        self.box((0,-.04,z-height/2),(width+.32,.48,.12),'concrete')
        self.box((0,-.04,z+height/2),(width+.32,.44,.13),'trim')
        self.box((0,.16,z),(.055,.16,height),'steel')
        self.box((0,.035,z+height*.34),(width,.12,.13),'trim')

    def bay_door(self, width=10, height=13, rollup=False):
        self.counts['rollup_door' if rollup else 'bay_door'] += 1
        reveal=self.dimension('reveal_m',.24)
        self.box((0,reveal+.18,height/2+.32),(width,.16,height),'recess')
        # Leaves are recessed .24m behind jambs, never pasted onto a solid wall.
        rows = max(3,round(height/(.48 if rollup else 1.7)))
        for j in range(rows):
            hh = height/rows
            for sign in [-1,1]:
                self.box((sign*width*.25,reveal+.07,.32+hh*(j+.5)),(width/2-.045,.10,hh-.045),'roof' if rollup else 'trim')
        self.box((0,.19,height/2+.32),(.13,.16,height),'steel')
        for sign in [-1,1]:
            self.box((sign*(width/2+.18),.02,height/2+.32),(.36,.63,height+.35),'concrete')
            self.box((sign*(width/2+.43),-.16,height/2+.32),(.15,.23,height+.45),'steel')
            self.box((sign*(width/2+.30),-.30,height*.45),(.18,.12,min(height*.6,2.8)),'accent')
        self.box((0,-.10,height+.55),(width+1.15,.86,.40),'trim')
        self.box((0,-.29,.34),(width+.7,.65,.20),'hazard')
        for x in [-width*.38,width*.38]:
            self.floodlight(x,height+.88)

    def infantry_door(self, width=1.25, height=2.4):
        self.counts['infantry_door'] += 1
        self.bay_door(width,height,False)
        self.box((width*.28,.20,height*.5),(.10,.12,.32),'accent')

    def buttress(self, height=6, width=.5, depth=.7):
        self.counts['buttress'] += 1
        w=width/2
        self.mesh([(-w,-depth,0),(w,-depth,0),(w,.15,0),(-w,.15,0),(-w,-depth*.25,height),(w,-depth*.25,height),(w,.15,height),(-w,.15,height)],[(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)],'concrete')
        self.box((0,-depth*.65,.68),(width+.12,depth*.7,.24),'trim')

    def flat_roof(self,width=12,depth=18,z=4,parapet=.55):
        self.counts['flat_roof'] += 1
        self.box((0,0,z),(width,depth,.22),'roof')
        for sign in [-1,1]:
            for cx,cy,sx,sy in [(0,sign*(depth/2-.15),width,.30),(sign*(width/2-.15),0,.30,depth)]:
                self.box((cx,cy,z+parapet/2),(sx,sy,parapet),'armour')
                self.box((cx,cy,z+parapet),(sx+.14,sy+.14,.12),'trim')

    def barrel_roof(self,width=24,depth=30,z=12,rise=4):
        self.counts['barrel_roof'] += 1
        n=16
        arc=[(-width/2+width*i/n,z+rise*math.sin(math.pi*i/n)) for i in range(n+1)]
        vs=[(x,y,zz) for y in [-depth/2,depth/2] for x,zz in arc]
        self.mesh(vs,[(i,i+1,i+n+2,i+n+1) for i in range(n)],'roof')
        for y in [-depth/2,depth/2]:
            self.mesh([(x,y,zz) for x,zz in arc]+[(width/2,y,z),(-width/2,y,z)],[tuple(range(n+3))] if y>0 else [tuple(reversed(range(n+3)))],'armour')
        for y in [-depth/2,0,depth/2]:
            # Following strips form real ribs, not texture lines.
            for i in range(n):
                x1,z1=arc[i];x2,z2=arc[i+1]
                self.mesh([(x1,y-.12,z1),(x2,y-.12,z2),(x2,y+.12,z2),(x1,y+.12,z1),(x1,y-.12,z1+.13),(x2,y-.12,z2+.13),(x2,y+.12,z2+.13),(x1,y+.12,z1+.13)],[(0,1,5,4),(3,7,6,2),(4,5,6,7)],'trim')
        for sign in [-1,1]:
            self.box((sign*width/2,0,z),(.28,depth+.20,.32),'trim')

    def roof_truss(self,width=6,height=1.5):
        self.counts['roof_truss'] += 1
        self.box((0,0,0),(width,.18,.18),'steel')
        for sign in [-1,1]:
            self.mesh([(0,-.09,height),(sign*width/2,-.09,0),(sign*width/2,-.09,.18),(0,-.09,height+.18),(0,.09,height),(sign*width/2,.09,0),(sign*width/2,.09,.18),(0,.09,height+.18)],[(0,1,2,3),(4,7,6,5),(0,4,5,1),(3,2,6,7)],'steel')

    def dome(self,radius=2,height=1.4):
        self.counts['dome'] += 1
        n=16;vs=[(radius*math.cos(i*math.tau/n)*math.cos(j*math.pi/8),radius*math.sin(i*math.tau/n)*math.cos(j*math.pi/8),height*math.sin(j*math.pi/8)) for j in range(5) for i in range(n)]
        self.mesh(vs,[(j*n+i,j*n+(i+1)%n,(j+1)*n+(i+1)%n,(j+1)*n+i) for j in range(4) for i in range(n)],'armour')

    def vent(self,width=1.6,depth=1.5,height=.9):
        self.counts['vent'] += 1
        self.box((0,0,height/2),(width,depth,height),'trim')
        self.box((0,-depth/2-.02,height*.55),(width*.82,.08,height*.65),'recess')
        for i in range(5):
            self.box((0,-depth/2-.08,height*(.3+i*.12)),(width*.84,.16,.045),'steel')
        self.box((0,0,height+.08),(width+.18,depth+.18,.16),'concrete')
        self.cylinder((0,0,height+.20),min(width,depth)*.30,.12,'steel',16)
        for i in range(4):
            a=i*math.pi/2
            with self.at((self.origin[0],self.origin[1],self.origin[2]),self.yaw+a):
                self.box((width*.12,0,height+.28),(width*.34,.09,.04),'trim')

    def pipe(self,height=4,radius=.10):
        self.counts['pipe'] += 1
        self.cylinder((0,0,height/2),radius,height,'steel')
        for z in [.3,height*.5,height-.2]:
            self.cylinder((0,0,z),radius*1.4,.10,'trim')

    def antenna(self,height=2.5):
        self.counts['antenna'] += 1
        self.cylinder((0,0,.14),.32,.28,'trim')
        self.cylinder((0,0,height/2),.055,height,'steel',8)
        self.box((0,0,height*.82),(1.1,.06,.06),'steel')
        self.box((0,0,height*.55),(.7,.06,.06),'steel')
        self.cylinder((0,0,height),.10,.14,'accent',8)

    def catwalk(self,width=6,depth=1.1,z=4):
        self.counts['catwalk'] += 1
        self.box((0,0,z),(width,depth,.13),'steel')
        for x in [-width/2+i*width/max(1,round(width/1.5)) for i in range(round(width/1.5)+1)]:
            self.box((x,-depth/2,z+.52),(.055,.055,1.05),'trim')
        for h in [.52,1.05]:
            self.box((0,-depth/2,z+h),(width,.055,.055),'accent' if h>1 else 'trim')
        for i in range(max(2,round(width/.20))):
            self.box((-width/2+i*.20,0,z+.085),(.035,depth,.025),'trim')

    def ladder(self,height=4,width=.48):
        self.counts['ladder'] += 1
        for x in [-width/2,width/2]:
            self.box((x,0,height/2),(.065,.12,height),'accent')
        for i in range(int(height/.30)):
            self.box((0,-.045,.20+i*.30),(width,.08,.055),'steel')

    def stairs(self,width=1.5,rise=.6,run=1.4):
        self.counts['stairs'] += 1
        n=max(2,round(rise/.15))
        for i in range(n):
            self.box((0,-run+(i+.5)*run/n,(i+1)*rise/n/2),(width,run/n,(i+1)*rise/n),'concrete')
        for sign in [-1,1]:
            for i in [0,n-1]:
                self.box((sign*width*.47,-run+(i+.5)*run/n,(i+1)*rise/n+.48),(.055,.055,.96),'steel')
            # Four short horizontal segments keep rail geometry stable at RTS scale.
            for i in range(n):
                self.box((sign*width*.47,-run+(i+.5)*run/n,(i+1)*rise/n+.96),(.055,run/n+.03,.055),'steel')

    def signage(self,width=3,height=.65,z=3):
        self.counts['signage'] += 1
        self.box((0,-.20,z),(width,.12,height),'steel')
        self.box((0,-.275,z),(width-.15,.04,height-.13),'accent')

    def floodlight(self,x=0,z=3):
        self.counts['floodlight'] += 1
        self.box((x,-.35,z),(.42,.36,.23),'steel')
        self.box((x,-.545,z),(.32,.04,.12),'lamp')

    def pad(self,width=12,depth=18):
        self.counts['pad'] += 1
        self.box((0,0,.09),(width,depth,.18),'concrete')


def assemble(job,style):
    import random
    rng=random.Random(job['seed'])
    kit=MeshKit(style['dimensions']);r=job['rule'];h=job['height_m']
    for wing_index,(x0,y0,x1,y1) in enumerate(job['wings_m']):
        cx,cy=(x0+x1)/2,(y0+y1)/2
        lotw,lotd=x1-x0,y1-y0
        margin=min(.95,lotw*.09,lotd*.09)
        w,d=lotw-2*margin,lotd-2*margin
        wh=max(2.0,h*r['height_fraction'])
        roof_top=wh
        with kit.at((cx,cy,0)):
            kit.pad(lotw,lotd)
            kit.box((0,0,.27),(w+.35,d+.35,.25),'concrete')
            kit.box((0,0,.48),(w+.16,d+.16,.17),'trim')
        front_width=w
        dw,dh=style['dimensions'][r['door']+'_door_m']['value']
        count=min(r['door_count'],max(1,int(w/(dw+1.7))))
        dw=min(dw,(front_width-1.2)/count-1.0)
        dh=min(dh,wh-(.95 if r['door']=='infantry' else 1.45))
        dw,dh=max(.8,dw),max(1.3,dh)
        openings=[((i-(count-1)/2)*(dw+1.5),dw) for i in range(count)]
        # Four segmented walls; no full solid mesh lies behind an opening.
        for edge,(origin,angle,length) in enumerate([((cx,cy-d/2,0),0,w),((cx+w/2,cy,0),math.pi/2,d),((cx,cy+d/2,0),math.pi,w),((cx-w/2,cy,0),-math.pi/2,d)]):
            with kit.at(origin,angle):
                if edge==0:
                    previous=-length/2
                    for x,ow in openings:
                        left=x-ow/2-.50
                        if left>previous:
                            kit.box(((previous+left)/2,.20,(wh+.5)/2),(left-previous,.34,wh-.5),'armour')
                        kit.box((x,.20,(wh+dh+.85)/2),(ow+1,.34,wh-dh-.85),'armour')
                        if r['door']=='infantry':
                            # Local placement is already world oriented; offset along wall.
                            ox,oy,oz=kit.origin
                            with kit.at((ox+x*math.cos(angle),oy+x*math.sin(angle),oz),angle):
                                kit.infantry_door(dw,dh)
                                kit.stairs(min(1.5,dw+.4),.32,margin*.8)
                        else:
                            ox,oy,oz=kit.origin
                            with kit.at((ox+x*math.cos(angle),oy+x*math.sin(angle),oz),angle):
                                kit.bay_door(dw,dh,r['door']=='vehicle')
                        previous=x+ow/2+.50
                    if previous<length/2:
                        kit.box(((previous+length/2)/2,.20,(wh+.5)/2),(length/2-previous,.34,wh-.5),'armour')
                    kit.signage(min(length*.5,7),min(.7,wh*.10),wh-r.get('sign_drop_m',.48))
                    # Heavy front shoulders and layered lintel keep the bay
                    # legible when glazing and rails fall below a pixel.
                    for sign in [-1,1]:
                        ox,oy,oz=kit.origin
                        x=sign*(length/2-.38)
                        with kit.at((ox+x,oy,oz),angle):
                            kit.buttress(wh-.15,.72,min(.74,margin*.75))
                    kit.box((0,-.13,wh-.08),(length+.18,.55,.32),'concrete')
                    kit.box((0,-.24,wh+.08),(length+.24,.67,.12),'trim')
                else:
                    n=max(1,round(length/style['dimensions']['module_m']['value']))
                    pw=length/n
                    for i in range(n):
                        x=-length/2+(i+.5)*pw
                        ox,oy,oz=kit.origin
                        with kit.at((ox+x*math.cos(angle),oy+x*math.sin(angle),oz),angle):
                            kit.wall_panel(pw,wh,r['windows'],style['dimensions']['panel_gap_m']['value'])
                    bn=max(1,round(length/r['buttress_every_m']))
                    for i in range(bn+1):
                        x=-length/2+.30+(length-.60)*i/bn
                        ox,oy,oz=kit.origin
                        with kit.at((ox+x*math.cos(angle),oy+x*math.sin(angle),oz),angle):
                            kit.buttress(wh*.95,.42,min(.64,margin*.68))
                kit.box((0,0,wh-.12),(length+.12,.30,.24),'trim')
        with kit.at((cx,cy,0)):
            if r['roof']=='barrel':
                rise=h*.21
                kit.barrel_roof(w+.12,d+.12,wh,rise);roof_top=wh+rise
                kit.box((0,0,roof_top+.35),(min(3.8,w*.13),d*.57,.70),'trim')
                kit.box((0,0,roof_top+.76),(min(4.1,w*.14),d*.59,.16),'concrete')
                for sign in [-1,1]:
                    kit.box((sign*min(1.91,w*.066),0,roof_top+.38),(.08,d*.53,.28),'recess')
                for i in range(7):
                    kit.box((0,(-.23+i*.077)*d,roof_top+.36),(min(4.0,w*.14),.10,.62),'armour')
                for sign in [-1,1]:
                    kit.box((sign*(w/2-1.5),0,wh+.15),(2.4,d*.75,.36),'trim')
            else:
                kit.flat_roof(w+.10,d+.10,wh,min(kit.dimension('parapet_m',.55),h*.09));roof_top=wh+.6
                if r['roof']=='clerestory':
                    kit.box((0,0,wh+h*.10),(w*.48,d*.82,h*.20),'armour')
                    kit.box((0,0,wh+h*.205),(w*.51,d*.85,.22),'trim');roof_top=wh+h*.21
                    for i in range(6):
                        kit.box((-w*.247,-d*.34+i*d*.135,wh+h*.12),(.08,d*.10,h*.06),'glass')
                    kit.cylinder((w*.34,d*.32,wh+h*.11),.60,h*.22,'trim')
                    kit.cylinder((w*.34,d*.32,wh+h*.23),.78,.22,'steel')
            for i in range(r['roof_plant']):
                vx=(-.28+i*.25)*w;vy=d*(.15+rng.uniform(-.06,.06))
                if r['roof']=='clerestory':
                    # Mount on the clear shoulder decks, outside the raised
                    # central mass and away from its chimney service zone.
                    vx=(-.36 if i%2==0 else .36)*w
                    vy=d*(-.25+(i//2)*.42+rng.uniform(-.035,.035))
                # Roof plant on shoulders avoids floating on a vault.
                vz=wh+(h*.21*math.sin(math.pi*(vx/w+.5)) if r['roof']=='barrel' else 0)+.14
                ox,oy,oz=kit.origin
                with kit.at((cx+vx,cy+vy,vz)):
                    kit.vent(min(2.6,w*.17),min(2.2,d*.15),min(1.3,h*.09))
            if r['antenna']:
                az=wh+(h*.21*math.sin(math.pi*.22) if r['roof']=='barrel' else .2)
                with kit.at((cx-w*.28,cy+d*.28,az)):
                    kit.antenna(min(2.1,max(.5,h-az-.2)))
        if r['catwalk'] and w>8:
            with kit.at((cx+w/2+margin*.30,cy,0),math.pi/2):
                kit.catwalk(d*.58,min(1.0,margin*.8),wh*.50)
            with kit.at((cx+w/2+margin*.68,cy-d*.25,.4),math.pi/2):
                kit.ladder(wh*.50-.35)
        with kit.at((cx-w/2-margin*.30,cy+d*.25,.5)):
            kit.pipe(wh*.8,.10)
    return kit
