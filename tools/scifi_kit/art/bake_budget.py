"""LOD0 triangle budgets (game carrier, after bevel). Pure Python: tests import it.

Props <= 3k, small buildings <= 8k, large buildings <= 15k (owner art bar 2026-10-22).
"""
BUDGETS = {'rpbuilding': 15000, 'quonsetclean': 8000, 'quonset': 8000,
           'lookouttower': 8000, 'razorwirefence': 3000, 's_lightwall': 3000,
           'crateswithtarp': 3000, 'barrels': 3000}


def budget(name):
    return BUDGETS.get(name, 3000)
