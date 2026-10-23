"""LOD0 triangle budgets (game carrier, after bevel). Pure Python: tests import it.

Props <= 3k, small buildings <= 8k, large buildings <= 15k (owner art bar 2026-10-22).
"""
BUDGETS = {'rpbuilding': 15000, 'quonsetclean': 8000, 'quonset': 8000,
           'lookouttower': 8000, 'razorwirefence': 3000, 's_lightwall': 3000,
           'crateswithtarp': 3000, 'barrels': 3000,
           'singlecivlivinga': 8000, 'portable': 8000,
           'streetlight': 3000, 'woodencrates': 3000, 'runwaylight': 3000,
           'redbeaconlight': 3000, 'tripodbarricade': 3000,
           'corrigatedmetalbarricade': 3000}


# Sonnet runner (building / turret / wall) classes: large 15k, small 8k, props 3k.
BUDGETS.update({'genericmilitary': 15000, 'liquidstorage': 15000, 'largefueltank': 15000,
                'geodesicdome': 8000, 'ammodump': 8000})


def budget(name):
    return BUDGETS.get(name, 3000)
