"""Regional land use, settlement rank and terrain-aware transport for large maps.

The small-map planner never enters this path. All coordinates here are movement
cells; native asset placement remains the responsibility of placement.py.
"""
import math
import random
import heapq
import numpy as np
from tools.mapgen.common import components, distance_field

REGIONS = ('wilderness', 'farmland', 'industrial', 'military', 'urban_core')
STEP = 8


def valley_route(start, goal, allowed, costs):
    """Eight-way A*, forbidding diagonal corner cuts across blocked blocks."""
    start,goal = tuple(start),tuple(goal)
    frontier = [(math.dist(start,goal),0.,start)]
    best,parent = {start:0.},{}
    rows,cols = allowed.shape
    while frontier:
        _,cost,p = heapq.heappop(frontier)
        if cost!=best.get(p):
            continue
        if p==goal:
            path = [p]
            while p!=start:
                p = parent[p]
                path.append(p)
            return path[::-1]
        for dr,dc in ((-1,0),(0,-1),(0,1),(1,0),(-1,-1),(-1,1),(1,-1),(1,1)):
            r,c = p[0]+dr,p[1]+dc
            if not (0<=r<rows and 0<=c<cols and allowed[r,c]):
                continue
            if dr and dc and not (allowed[p[0],c] and allowed[r,p[1]]):
                continue
            new = cost+math.hypot(dr,dc)*(1+float(costs[r,c]))
            q = (r,c)
            if new<best.get(q,math.inf):
                best[q],parent[q] = new,p
                heapq.heappush(frontier,(new+math.dist(q,goal),new,q))
    raise ValueError('no dry regional road corridor')


def noise(shape, seed, wavelength):
    rng = np.random.default_rng(seed)
    rows, cols = shape
    nr, nc = max(3,math.ceil(rows/wavelength)+1), max(3,math.ceil(cols/wavelength)+1)
    values = rng.random((nr,nc))
    x, y = np.linspace(0,nc-1,cols), np.linspace(0,nr-1,rows)
    # Smoothstep interpolation avoids the visible grid of bilinear noise.
    ix, iy = np.minimum(x.astype(int),nc-2), np.minimum(y.astype(int),nr-2)
    tx, ty = x-ix, y-iy
    tx, ty = tx*tx*(3-2*tx), ty*ty*(3-2*ty)
    a = values[iy[:,None],ix[None,:]]*(1-tx)+values[iy[:,None],ix[None,:]+1]*tx
    b = values[iy[:,None]+1,ix[None,:]]*(1-tx)+values[iy[:,None]+1,ix[None,:]+1]*tx
    return a*(1-ty[:,None])+b*ty[:,None]


def plan(mainland, heights, slopes, wet, seed, count):
    size = len(mainland)
    rng = random.Random(seed ^ 0x4D414352)
    # Every coarse routing block is wholly dry/flat, including its swept width.
    n = size//STEP
    allowed = mainland[:n*STEP,:n*STEP].reshape(n,STEP,n,STEP).all(axis=(1,3))
    groups = components(allowed)
    if not groups:
        raise ValueError('no dry regional road corridor')
    allowed[:] = False
    ar,ac = np.asarray(groups[0]).T
    allowed[ar,ac] = True
    h = heights[:n*STEP,:n*STEP].reshape(n,STEP,n,STEP).mean(axis=(1,3))
    s = slopes[:n*STEP,:n*STEP].reshape(n,STEP,n,STEP).max(axis=(1,3))
    field = noise((n,n),seed ^ 0x123, max(12,n/4))
    moisture = noise((n,n),seed ^ 0x456, max(10,n/6))
    relief = (h-h.min())/max(1,float(np.ptp(h)))
    suitability = field - .35*relief - s/20
    shore = distance_field(wet)[STEP//2:n*STEP:STEP,STEP//2:n*STEP:STEP]
    suitability += .12*np.exp(-shore/35)
    interior = np.zeros_like(allowed)
    margin = max(10,round(n*.13))
    interior[margin:-margin,margin:-margin] = True
    candidates = np.argwhere(allowed & interior & (suitability >= np.quantile(suitability[allowed],.48)))
    rng.shuffle(candidates)
    majors = max(3,min(9,round(size/430)))
    hubs = []
    for cell in candidates:
        p = tuple(map(int,cell))
        if all(math.dist(p,q)>n*.20 for q in hubs):
            hubs.append(p)
            if len(hubs)==majors:
                break
    if len(hubs)<2:
        raise ValueError('insufficient separated regional settlement centres')
    regions = np.zeros((n,n),np.uint8)
    regions[(suitability > np.quantile(suitability[allowed],.48)) & allowed] = 1
    rr,cc = np.mgrid[:n,:n]
    hub_regions = (4,3,2,4,2,3,4,2,3)
    for i,(r,c) in enumerate(hubs):
        influence = ((rr-r)/(n*.075))**2+((cc-c)/(n*.10))**2
        regions[(influence < 1+.5*moisture) & allowed] = hub_regions[i]
    nodes = list(hubs)
    owners = list(range(len(hubs)))
    for _ in range(count*150):
        if len(nodes)>=count:
            break
        owner = rng.randrange(len(hubs))
        hub = hubs[owner]
        # Dense satellites plus occasional remote hamlets; no global parcel grid.
        sigma = n*rng.choice((.025,.045,.085))
        if len(nodes)%9==0:
            p = (rng.randrange(3,n-3),rng.randrange(3,n-3))
        else:
            p = (round(rng.gauss(hub[0],sigma)),round(rng.gauss(hub[1],sigma)))
        if not (2<=p[0]<n-2 and 2<=p[1]<n-2 and allowed[p]):
            continue
        if regions[p]==0 and len(nodes)%9!=0 and rng.random()>.08:
            continue
        if min(math.dist(p,q) for q in nodes)<rng.uniform(2.8,5.5):
            continue
        nodes.append(p)
        owners.append(owner)
    # Prim MST on major settlements, then redundant major links and branches.
    edges, joined = [], {0}
    while len(joined)<len(hubs):
        _,a,b = min((math.dist(hubs[a],hubs[b]),a,b) for a in joined
                    for b in range(len(hubs)) if b not in joined)
        edges.append((a,b,'trunk'))
        joined.add(b)
    pairs = sorted((math.dist(hubs[a],hubs[b]),a,b) for a in range(len(hubs))
                   for b in range(a+1,len(hubs))
                   if not any({a,b}=={x,y} for x,y,_ in edges))
    for _,a,b in pairs[:max(1,len(hubs)//3)]:
        edges.append((a,b,'loop'))
    for i in range(len(hubs),len(nodes)):
        parent = min(range(i),key=lambda j:math.dist(nodes[i],nodes[j]))
        edges.append((parent,i,'branch'))
    costs = s/8 + relief*.5 + noise((n,n),seed ^ 0x789,max(4,n/18))*.6

    def world(p):
        return (p[0]*STEP+STEP//2,p[1]*STEP+STEP//2)

    def routed(a,b):
        coarse = valley_route(a,b,allowed,costs)
        # Refine each coarse step within guaranteed traversable blocks.
        path = [world(coarse[0])]
        for p in coarse[1:]:
            goal = world(p)
            r,c = path[-1]
            while (r,c)!=goal:
                if r!=goal[0]:
                    r += 1 if r<goal[0] else -1
                if c!=goal[1]:
                    c += 1 if c<goal[1] else -1
                path.append((r,c))
        return path

    paths = [(routed(nodes[a],nodes[b]),64. if kind!='branch' else 40.) for a,b,kind in edges]
    specs = []
    for i,p in enumerate(nodes):
        rank = i+1
        population = (round(150/rank**.86) if i<len(hubs) else
                      max(1,round(65/(1+(i-len(hubs))*220/count)**.95)))
        region = int(regions[p])
        # The first major is always a compound; other forms follow land use.
        kind = 'walled_compound' if i==0 else (
            'town' if region in (1,4) else 'industrial' if region==2 else
            'walled_compound' if region==3 and population>=12 else 'outpost')
        if population<5:
            kind = 'outpost'
        aspect = rng.uniform(.7,1.4)
        side = max(11,math.sqrt(population)*6.0)
        minimum = 24 if kind=='walled_compound' else 11
        width = max(minimum,round(side*aspect))
        height = max(minimum,round(side/aspect))
        specs.append(dict(anchor=list(world(p)),region=REGIONS[region],rank=rank,
                          population=population,kind=kind,width=min(76,width),height=min(96,height),
                          owner=owners[i]))
    metadata = dict(version=1,cell_step=STEP,regions=regions.tolist(),region_names=list(REGIONS),
                    major_count=len(hubs),nodes=[list(world(p)) for p in nodes],
                    edges=[dict(a=a,b=b,kind=kind) for a,b,kind in edges],
                    rank_exponent=.86)
    return metadata,specs,paths,moisture
