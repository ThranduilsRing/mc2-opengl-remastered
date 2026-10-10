# Phase split spec for executeSpecialBody_Apply (code/brain_special_dispatch.cpp). Line numbers are 1-based lines of the
# file at BASE.
BASE = '4713de12a'
SRC = 'code/brain_special_dispatch.cpp'
FUNC_SIG = r'^bool executeSpecialBody_Apply\('
INDENT = '    '
DEDENT = ''
STRUCTS = {'f': 'BrainVerbFrame'}
ALIAS_PER_LINE = 3
FRAME_HEADER = 'code/brain_special_frame.h'
FRAME_HEADER_DOC = 'The resolved verb spellings one dispatch step hands to the effect handlers.'
INTERNAL_INCLUDES = ['#include "brain_special_internal.h"']
APPLY = 'code/brain_special_apply.cpp'
FILE_HEADS = {APPLY: ['// Brain special dispatch: the effect handlers of executeSpecialBody_Apply -- power, eject, guard, move, attack,',
                      '// retreat, clear-move, target priority, patrol and the FSM verbs. The dispatch loop is in brain_special_dispatch.cpp.']}

# The verb strings reach the effect handlers through the frame so no helper takes a const char* parameter (cxx17 LG06).
MEMBERS = [
    ('vp', 'const char*', 'f', [0]),
    ('vpCanon', 'const char*', 'f', [0]),
]
EDITS = {}
ORCH_BEFORE = {1210: '        BrainVerbFrame f;\n        f.vp = vp;\n        f.vpCanon = vpCanon;'}
ORCH_EXTRA = ['f.vp = vp;', 'f.vpCanon = vpCanon;']
FRAME_DOC = '// The resolved verb spellings one dispatch step hands to the effect handlers.'

# The orchestrator keeps the gates, the per-verb prelude (alias resolution, call chain, var verbs, flow verbs) and the
# effect chain skeleton; each effect handler body is a helper that gets the dispatch state it names.
PARAMS = [
    ('body', 'const BrainSpecialBody&'),
    ('warrior', 'MechWarrior*'),
    ('wid', 'int'),
    ('varStore', 'VarStore*'),
    ('index', 'const SpecialIndex*'),
    ('runtime', 'MechBrainRuntime*'),
    ('appliedEffect', 'bool&'),
    ('stateGateOpen', 'bool&'),
    ('varGate', 'bool'),
    ('callGate', 'bool'),
    ('fsmGate', 'bool'),
    ('flowActive', 'bool'),
    ('flowRt', 'MechBrainRuntime*'),
    ('verbIdx', 'int'),
]

ORCH_DOC = ('// Applies the effect verbs of one special body. The gates, the per-verb prelude and the effect chain skeleton stay\n'
            '// here; each effect handler body is a helper above, run in the original order. A handler that ends the body\n'
            '// returns true and the orchestrator returns appliedEffect.')

D = {'dedent': '        ', 'file': APPLY}
X = {'dedent': '        ', 'exits': 'appliedEffect', 'file': APPLY}
PHASES = [
    ('applyCorePower', 1211, 1225, 'Switch the warrior power state (Brain.CorePower false).', D),
    ('applyUnitEject', 1227, 1241, 'Eject the pilot (Unit.Eject).', D),
    ('applyCoreGuard', 1243, 1257, 'Order the unit to guard (OPORD.CoreGuard).', D),
    ('applyCoreMoveTo', 1259, 1320, 'Order the unit to move to a point (OPORD.CoreMoveTo).', D),
    ('applyCoreAttack', 1324, 1394, 'Order the unit to attack a target (OPORD.CoreAttack).', D),
    ('applyUnitRetreat', 1396, 1410, 'Order the unit to retreat (Unit.Retreat).', D),
    ('applyClearMoveOrders', 1413, 1432, 'Scrub the engine movement queue (Unit.ClearMoveOrders).', D),
    ('applySetTargetPriority', 1435, 1475, 'Write a target-priority list entry (UnitQuery.SetTargetpriority).', D),
    ('applyCorePatrol', 1478, 1589, 'Start a patrol over up to eight waypoints (OPORD.CorePatrol).', D),
    ('applySetState', 1592, 1613, 'Transition the brain state and end the body (Unit.SetState).', X),
    ('applySetStatePrev', 1615, 1628, 'Swap back to the previous brain state and end the body (Unit.SetStatePrev).', X),
    ('applyInState', 1631, 1645, 'Close the state gate unless the brain is in the named state (Unit.InState).', D),
    ('applyNotInState', 1648, 1662, 'Close the state gate when the brain is in the named state (Unit.NotInState).', D),
    ('applySetStateIf', 1665, 1739, 'Transition the brain state when a unit variable matches (Unit.SetStateIf).', X),
]
