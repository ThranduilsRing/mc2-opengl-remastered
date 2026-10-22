"""Comparable scale diagnostics; sample land on a fixed 64 by 64 grid."""
from collections import Counter
import numpy as np
from tools.mapgen.analyze import clusters, structural, valid_objects
from tools.mapgen.common import components, mask_of


def measure_scale(layout):
    groups = clusters(valid_objects(layout))
    centres = np.array([np.mean([o['cell'] for o in g], axis=0) for g in groups])
    counts = np.array([len(g) for g in groups])
    rows, cols = layout['size']['rows'], layout['size']['cols']
    nn = []
    if len(centres) > 1:
        d = np.sqrt(np.square(centres[:, None]-centres[None, :]).sum(axis=2))
        np.fill_diagonal(d, np.inf)
        nn = d.min(axis=1)
    grid = np.array([(r,c) for r in np.linspace(0,rows-1,64).astype(int)
                     for c in np.linspace(0,cols-1,64).astype(int)])
    dry = ~mask_of(layout,'water')[grid[:,0],grid[:,1]]
    empty = np.ones(len(grid), bool)
    for centre in centres:
        empty &= np.square(grid-centre).sum(axis=1) > (min(rows,cols)*.1)**2
    roads = mask_of(layout,'roads')
    road_groups = components(roads, diagonal=True)
    trees = Counter((int(o['cell'][0]*8/rows),int(o['cell'][1]*8/cols))
                    for o in layout['objects'] if o['class']=='tree')
    tree_bins = np.array([trees[r,c] for r in range(8) for c in range(8)])
    result = dict(sites=len(groups), nn_median_cells=float(np.median(nn)) if len(nn) else 0,
                  nn_cv=float(np.std(nn)/np.mean(nn)) if len(nn) else 0,
                  size_p10_p50_p90=np.percentile(counts,[10,50,90]).tolist() if len(counts) else [],
                  empty_land_fraction=float(empty[dry].mean()),
                  road_components=len(road_groups),
                  road_largest_fraction=len(road_groups[0])/max(1,int(roads.sum())) if road_groups else 0,
                  tree_bin_cv=float(tree_bins.std()/max(1,tree_bins.mean())))
    if layout.get('bases'):
        areas = [(b['bounds'][2]-b['bounds'][0])*(b['bounds'][3]-b['bounds'][1]) for b in layout['bases']]
        result['parcel_area_p10_p50_p90'] = np.percentile(areas,[10,50,90]).tolist()
        result['parcel_area_max_min'] = max(areas)/min(areas)
        members = Counter(o.get('base_id') for o in structural(layout['objects']))
        sizes = [members[b['id']] for b in layout['bases']]
        result['parcel_structures_p10_p50_p90'] = np.percentile(sizes,[10,50,90]).tolist()
        result['tiny_site_fraction'] = sum(n<=3 for n in sizes)/len(sizes)
        result['largest_site_structures'] = max(sizes)
    return result
