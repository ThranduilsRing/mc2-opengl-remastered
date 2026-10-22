"""Reproducible flat playable demos and capture poses; never launches the game."""
import argparse
from pathlib import Path

import numpy as np

from tools.mapgen.common import output_path, write_json
from tools.mapgen.data import Install
from tools.mapgen.generate import generate
from tools.mapgen.mission_writer import STOCK, emit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--source',type=Path,default=STOCK)
    parser.add_argument('--size',type=int,choices=(180,360,720),action='append')
    args = parser.parse_args()
    out = output_path(args.out,[args.source])
    source = Install(args.source)
    for size in args.size or (180,720):
        seed = {180:7,360:17,720:23}[size]
        layout = generate(size,seed,np.full((size,size),1000.))
        name = f'mapgen_{size}_{seed}'
        write_json(out/'layouts'/(name+'.json'),layout)
        report = emit(layout,source,out/'missions',name)
        poses = []
        for i, base in enumerate(layout['bases'][:2]):
            r0,c0,r1,c1 = base['bounds']
            x,y = ((c0+c1)/2-size/2)*128/3,(size/2-(r0+r1)/2)*128/3
            poses.append({'name':f'base{i}_topdown','pos':[x,y,1000],
                          'rot':0,'proj':75,'alt':4000,'covers':['generated_base'],
                          '_note':'Generated inspection pose from parcel bounds; no golden/parity claim.'})
        poses.append({'name':'player_lance','pos':layout['player_start_wu'],
                      'rot':0,'proj':55,'alt':3000,'covers':['generated_player_units']})
        write_json(out/'bookmarks'/(name+'.json'),{'schema_v':1,'mission':name,'bookmarks':poses},pretty=True)
        print(report,flush=True)


if __name__ == '__main__':
    main()
