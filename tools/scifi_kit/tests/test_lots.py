"""Footprint regressions: reentrant lots cannot be replaced by their AABB."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from core import inside, plan_wings


@pytest.mark.parametrize('poly',[
    [[0,0],[18,0],[18,6],[6,6],[6,18],[0,18]],
    [[0,0],[18,0],[14,12],[6,17],[0,12]],
    [[0,0],[18,0],[18,18],[12,18],[12,6],[6,6],[6,18],[0,18]],
])
def test_wings_never_bridge_concavities(poly):
    wings=plan_wings(poly)
    assert wings
    for x0,y0,x1,y1 in wings:
        for i in range(21):
            for j in range(21):
                assert inside([x0+(x1-x0)*i/20,y0+(y1-y0)*j/20],poly)


def test_reject_bowtie_and_small_lot():
    for poly in [[[0,0],[10,10],[0,10],[10,0]],[[0,0],[1,0],[1,1],[0,1]]]:
        with pytest.raises(ValueError):plan_wings(poly)
