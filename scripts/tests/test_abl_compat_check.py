"""Pin the stock/Magic ABI and scripted-flow census on small source trees."""
from tools import abl_compat_check as compat


STOCK = """library corebrain;
const TACORDER_PARAM_TURRET = 9;
type
order corePatrol(integer a, integer b, integer contact);
order coreWait(real seconds, integer scan, integer contact);
order coreEscort(integer target, real range, real angle, integer scan);
order coreAttack(integer target, integer tactic);
"""


def test_profile_internal_calls_match_restored_signatures():
    text = compat.read(compat.MAGIC_LIBRARY)
    assert not compat.compile_errors({compat.COREBRAIN: text}, [compat.COREBRAIN],
                                     compat.identity, compat.library(text), set(), set())


def tree(tmp_path, brain):
    (tmp_path / "warriors").mkdir()
    (tmp_path / "corebrain.abx").write_text(STOCK)
    (tmp_path / "warriors/unit.abl").write_text(brain)
    (tmp_path / "mc2_01.fit").write_text('''[Warrior0]
st Brain = "unit"
[Part1]
l Pilot = 0
l TeamId = 0
f PositionX = 10
f PositionY = 20
[Team0Objective0Condition0]
st ConditionSpeciesString = "DestroySpecificUnit"
f PositionX = 10
f PositionY = 20
''')
    return tmp_path


def test_contact_escort_ambush_and_hold_census(tmp_path):
    root = tree(tmp_path, '''fsm unit;
eternal boolean wave;
code
attack = getStateHandle("fight");
setTargetPriority(0, CONTACT_CRITERIA_ENEMY);
state start;
corePower(false);
corePatrol(s, p, -1);
coreWait(10.0, -1, attack);
coreEscort(1, 40.0, 135.0, -1);
coreAttack(0, TACORDER_PARAM_STOP_AND_FIRE);
endstate;
state fight;
wave = true;
endstate;
''')
    result = compat.check(root)
    classes = result["missions"]["mc2_01"]["classes"]
    # These classes disappear when FixABL retains the stock arguments.
    migrated = compat.fixabl("corePatrol(s, p, -1);")
    keeps_contact = len(compat.calls(migrated)[0][1]) == 3
    assert bool(classes["ignore_engage"]) != keeps_contact
    assert bool(classes["ignore_engage_objective_or_ally"]) != keeps_contact
    assert bool(classes["flow_trigger_in_contact_state"]) != keeps_contact
    assert bool(classes["pd_ambush"]) != keeps_contact
    assert bool(classes["escort"]) != keeps_contact
    assert classes["hold_mobile_units"] == 1
    assert not result["exit_fail"]


def test_campaign_arity_and_removed_constant_fail(tmp_path):
    result = compat.check(tree(tmp_path, "corePatrol(s); coreAttack(0, TACORDER_PARAM_TURRET);"), shipped=True)
    kinds = {e["kind"] for e in result["brains"]["data/missions/warriors/unit.abl"]["errors"]}
    assert kinds == {"arity", "symbol-removed", "constant-removed"}
    assert result["exit_fail"]


def test_includes_and_unused_errors(tmp_path):
    root = tree(tmp_path, '#include_ "shared.abi"\n// corePatrol(x);\nprint("corePatrol(x)");')
    (root / "warriors/shared.abi").write_text("corePatrol(s);")
    (root / "warriors/unused.abl").write_text("corePatrol(s);")
    result = compat.check(root)
    assert result["exit_fail"]
    assert "data/missions/warriors/unused.abl" in result["unused_with_errors"]
    assert result["brains"]["data/missions/warriors/unit.abl"]["errors"][0]["kind"] == "arity"


def test_calls_ignore_comments_and_balance_nested_arguments():
    assert [(n, a) for n, a, _, _ in compat.calls('/* f(x) */ coreWait(f(1, 2), -1, h); "g(x)"')] == [
        ("corewait", ["f(1, 2)", "-1", "h"]), ("f", ["1", "2"])]
