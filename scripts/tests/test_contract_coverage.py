"""Every check and C++ test must have an executable route or a reasoned exemption."""
import ast
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools import callers
from scripts import console_child


def entries(text, separator):
    return {name.strip(): reason.strip() for line in text.splitlines()
            if line.strip() and not line.lstrip().startswith("#")
            for name, _separator, reason in [line.partition(separator)]}


def coverage_problems(checks, inventory, manual, board, harnesses, registered, debt, tracked=None):
    issues = []
    items = {item["path"]: item for item in inventory["scripts"]}
    rows = re.findall(r'^run_check(?:_tri)?\s+"([^"]+)"\s+"([^"]+)"', board, re.M)
    if tracked is None:
        tracked = items
    for label, command in rows:
        for path in re.findall(r'(?<![\w./-])((?:scripts|tools|tests)/[\w./+-]+\.(?:py|sh|ps1))(?![\w./-])',
                               command):
            if path not in tracked:
                issues.append(f"board row {label} references untracked script: {path}")
    for column, label in ((0, "label"), (1, "command")):
        values = [row[column] for row in rows]
        if len(values) != len(set(values)):
            issues.append(f"duplicate board {label}")
    for path in checks:
        name = Path(path).name
        item = items.get(path, {})
        hits = item.get("callers_by_basename", []) + item.get("callers_by_stem", [])
        wired = any(hit["path"] == "scripts/check-contracts.sh" for hit in hits)
        if not hits and name not in manual:
            issues.append(f"unwired check: {path}")
        if name in manual and (wired or not manual[name].strip()):
            issues.append(f"invalid manual exemption: {name}")
    for name in manual:
        if name not in {Path(path).name for path in checks}:
            issues.append(f"stale manual exemption: {name}")
    for name in harnesses:
        if name not in registered and (name not in debt or not debt[name].strip()):
            issues.append(f"unregistered harness: {name}")
    for name in debt:
        if name not in harnesses or name in registered:
            issues.append(f"stale harness debt: {name}")
    return issues


def ownership_problems(modules, ownership, board):
    """Each discovered Python test module has one contract-row owner, or scripts_tests."""
    issues = []
    rows = re.findall(r'^run_check(?:_tri)?\s+"([^"]+)"\s+"([^"]+)"', board, re.M)
    commands = {label: command for label, command in rows}
    tests = {}
    for entry in ownership.get("tests", []):
        module = entry.get("module")
        owners = entry.get("owners", [])
        if not isinstance(module, str) or not isinstance(owners, list) or not owners \
                or not all(isinstance(owner, str) for owner in owners):
            issues.append(f"invalid ownership entry: {entry!r}")
            continue
        if module in tests:
            issues.append(f"duplicate ownership entry: {module}")
            continue
        tests[module] = owners
        if len(owners) != len(set(owners)):
            issues.append(f"duplicate owner: {module}")
        for owner in owners:
            if owner not in commands:
                issues.append(f"missing contract row for owner: {owner} ({module})")
            elif owner != ownership.get("aggregate_owner") and module not in commands[owner]:
                issues.append(f"row {owner} does not name its owned module: {module}")
    aggregate = ownership.get("aggregate_owner")
    if aggregate not in commands:
        issues.append(f"missing aggregate contract row: {aggregate}")
    for module in modules:
        if module not in tests:
            issues.append(f"zero owners: {module}")
    for module in tests:
        if module not in modules:
            issues.append(f"stale ownership entry: {module}")
        owners = tests[module]
        if aggregate in owners and len(owners) > 1:
            issues.append(f"duplicate execution: {module} (aggregate and direct owners)")
    return issues


def disabled_counts(text):
    stack = []
    disabled = 0
    for line in text.splitlines():
        if re.match(r'^\s*#\s*(?:if|ifdef|ifndef)\b', line):
            stack.append(bool(re.match(r'^\s*#\s*if\s+0\s*$', line)))
        elif re.match(r'^\s*#\s*endif\b', line):
            if stack:
                stack.pop()
        elif any(stack) and re.match(r'^\s*(?:DOCTEST_)?TEST_CASE\s*\(', line):
            disabled += 1
    skipped = len(re.findall(r'TEST_CASE\s*\([^)]*doctest::skip\s*\(\s*true\s*\)', text))
    return disabled, skipped


def tier_problems(sources, cmakes, runners, pins=(6, 1)):
    cmakes = {path: re.sub(r'#[^\n]*', '', text) for path, text in cmakes.items()}
    engine_sources = callers.driven_target_sources(cmakes.get("CMakeLists.txt", ""), runners)
    issues = [f"orphan test source: {name}" for name in sources
              if name not in cmakes["tests/unit/CMakeLists.txt"] and name not in engine_sources]
    for path, text in cmakes.items():
        if path == "CMakeLists.txt":
            continue
        targets = re.findall(r'\badd_executable\s*\(\s*([\w]+)', text)
        targets += re.findall(r'\badd_test\s*\(\s*NAME\s+([\w]+)', text)
        for target in targets:
            if target != "mc2_modlint" and target not in runners:
                issues.append(f"undriven test target: {target}")
    totals = tuple(sum(counts[i] for counts in map(disabled_counts, sources.values())) for i in (0, 1))
    if totals != pins:
        issues.append(f"disabled test debt: {totals}, expected {pins}")
    return issues


def test_contract_and_test_tier_coverage():
    read = lambda path: (ROOT / path).read_text(encoding="utf-8")
    inventory = callers.build_inventory(ROOT)
    # HEAD is the clone's ledger; staging and delivery-only inventory entries are insufficient.
    tracked = set(subprocess.run(["git", "ls-tree", "-r", "--name-only", "HEAD"], cwd=ROOT,
                                capture_output=True, text=True, check=True,
                                **console_child.child_kwargs(redirected=True)).stdout.splitlines())
    checks = [item["path"] for item in inventory["scripts"]
              if (ROOT / item["path"]).is_file() and (
                  re.fullmatch(r'scripts/check[-_].*\.(?:py|sh)', item["path"])
                  or re.fullmatch(r'tools/(?:check_[\w-]+|glsl_port_diff)\.py', item["path"]))
              and item["path"] != "scripts/check-contracts.sh"]
    registered = set()
    native_contracts = set()
    for node in ast.walk(ast.parse(read("tools/build_contract_harnesses.py"))):
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in
                {"REGISTERED_HARNESSES", "PY_HARNESSES"} for t in node.targets):
            values = ast.literal_eval(node.value)
            registered.update(value.split("/")[1] if value.startswith("tools/") else value
                              for value in values)
            if any(t.id == "REGISTERED_HARNESSES" for t in node.targets if isinstance(t, ast.Name)):
                native_contracts.update(values)
    harnesses = {path.parent.name for path in (ROOT / "tools").glob("*_harness/CMakeLists.txt")}
    issues = coverage_problems(checks, inventory, entries(read("scripts/manual-checks.allowlist"), ":"),
                               read("scripts/check-contracts.sh"), harnesses, registered,
                               entries(read("scripts/unregistered-harness-debt.txt"), "#"), tracked)
    modules = [f"scripts/tests/{path.name}" for path in (ROOT / "scripts/tests").glob("test_*.py")]
    issues += ownership_problems(modules, json.loads(read("scripts/test_ownership.json")),
                                 read("scripts/check-contracts.sh"))
    cmakes = {path: read(path) for path in ("CMakeLists.txt", "tests/unit/CMakeLists.txt",
              "tests/model_override/CMakeLists.txt", "tests/static_natural_skip/CMakeLists.txt")}
    sources = {path.name: path.read_text(encoding="utf-8")
               for path in (ROOT / "tests/unit").glob("test_*.cpp")}
    for name in native_contracts:
        source = sources.get(f"test_{name}.cpp", "")
        if not re.search(r'TEST_CASE(?:_FIXTURE)?\s*\([^\n]*"' + re.escape(name) + r'/', source):
            issues.append(f"missing migrated contract cases: {name}")
    runners = read("scripts/run-unit-tests.sh") + read("tools/build_contract_harnesses.py")
    issues += tier_problems(sources, cmakes, runners)
    assert not issues, "\n".join(issues)


def test_unwired_check_and_stale_exemptions_fail():
    inventory = {"scripts": []}
    assert coverage_problems(["scripts/check-lost.py"], inventory, {}, "", set(), set(), {})
    assert coverage_problems([], inventory, {"check-lost.py": "manual"}, "", set(), set(), {})
    assert coverage_problems(["scripts/check-lost.py"], inventory, {"check-lost.py": ""}, "", set(), set(), {})


def test_duplicate_rows_and_harness_debt_fail():
    row = 'run_check "same" "py -3 scripts/check-x.py"\n'
    assert coverage_problems([], {"scripts": []}, {}, row * 2, set(), set(), {})
    assert coverage_problems([], {"scripts": []}, {}, "", {"lost_harness"}, set(), {})
    assert coverage_problems([], {"scripts": []}, {}, "", {"paid_harness"}, {"paid_harness"}, {"paid_harness": "old"})


def test_ownership_rejects_missing_duplicate_and_stale_owners():
    board = 'run_check "direct" "py -3 -m pytest scripts/tests/test_direct.py"\n' \
            'run_check "scripts_tests" "py -3 scripts/run_scripts_tests.py"\n'
    modules = ["scripts/tests/test_direct.py", "scripts/tests/test_aggregate.py"]
    good = {"aggregate_owner": "scripts_tests", "tests": [
        {"module": "scripts/tests/test_direct.py", "owners": ["direct"]},
        {"module": "scripts/tests/test_aggregate.py", "owners": ["scripts_tests"]}]}
    assert ownership_problems(modules, good, board) == []
    missing = {"aggregate_owner": "scripts_tests", "tests": good["tests"][:1]}
    assert ownership_problems(modules, missing, board) == ["zero owners: scripts/tests/test_aggregate.py"]
    duplicate = {"aggregate_owner": "scripts_tests", "tests": [
        *good["tests"], {"module": "scripts/tests/test_direct.py", "owners": ["direct"]}]}
    assert ownership_problems(modules, duplicate, board) == [
        "duplicate ownership entry: scripts/tests/test_direct.py"]
    doubly_owned = {"aggregate_owner": "scripts_tests", "tests": [
        {"module": "scripts/tests/test_direct.py", "owners": ["direct", "scripts_tests"]},
        {"module": "scripts/tests/test_aggregate.py", "owners": ["scripts_tests"]}]}
    assert ownership_problems(modules, doubly_owned, board) == [
        "duplicate execution: scripts/tests/test_direct.py (aggregate and direct owners)"]
    stale = {"aggregate_owner": "scripts_tests", "tests": [
        *good["tests"], {"module": "scripts/tests/test_gone.py", "owners": ["gone"]}]}
    stale_board = board + 'run_check "gone" "py -3 -m pytest scripts/tests/test_gone.py"\n'
    assert ownership_problems(modules, stale, board) == [
        "missing contract row for owner: gone (scripts/tests/test_gone.py)",
        "stale ownership entry: scripts/tests/test_gone.py"]
    assert ownership_problems(modules, stale, stale_board) == [
        "stale ownership entry: scripts/tests/test_gone.py"]


def test_board_missing_script_fails_and_tracked_twin_passes():
    tracked = "scripts/check-present.py"
    inventory = {"scripts": [{"path": tracked}]}
    for runner in ("run_check", "run_check_tri"):
        for missing in ("scripts/check-missing.py", "tools/nested/check_missing.py", "tests/check_missing.py"):
            row = f'{runner} "fixture" "py -3 {tracked} && py -3 {missing}"\n'
            assert coverage_problems([], inventory, {}, row, set(), set(), {}) == [
                f"board row fixture references untracked script: {missing}"]
            clean = row.replace(f" && py -3 {missing}", "")
            assert coverage_problems([], inventory, {}, clean, set(), set(), {}) == []


def test_inventory_entry_cannot_substitute_for_head_tracking():
    path = "scripts/check-present.py"
    inventory = {"scripts": [{"path": path}]}
    row = f'run_check "fixture" "py -3 {path}"\n'
    assert coverage_problems([], inventory, {}, row, set(), set(), {}, tracked=set()) == [
        f"board row fixture references untracked script: {path}"]
    assert coverage_problems([], inventory, {}, row, set(), set(), {}, tracked={path}) == []


def test_orphan_source_and_undriven_target_fail():
    cmakes = {"tests/unit/CMakeLists.txt": "add_executable(lost test_old.cpp)"}
    issues = tier_problems({"test_new.cpp": "TEST_CASE(\"x\") {}"}, cmakes, "", pins=(0, 0))
    assert "orphan test source: test_new.cpp" in issues
    assert "undriven test target: lost" in issues


def test_root_seam_sources_require_a_built_and_run_target():
    sources = {"test_authored_order_seam.cpp": "", "test_authored_movement_fixture.cpp": "",
               "test_combat_retirement_seam.cpp": ""}
    cmakes = {
        "tests/unit/CMakeLists.txt": "",
        "CMakeLists.txt": """add_executable(engine engine.cpp)
            add_executable(order_seam EXCLUDE_FROM_ALL ${engine_sources}
                tests/unit/test_authored_order_seam.cpp
                tests/unit/test_authored_movement_fixture.cpp
                tests/unit/test_combat_retirement_seam.cpp)""",
    }
    build = '[cmake, "--build", bdir, "--target", "order_seam"]'
    run = 'exe = bdir / "RelWithDebInfo/order_seam.exe"'
    assert tier_problems(sources, cmakes, build + "\n" + run, pins=(0, 0)) == []
    for runners in (build, run, '# "order_seam"'):
        issues = tier_problems(sources, cmakes, runners, pins=(0, 0))
        assert all(f"orphan test source: {name}" in issues for name in sources)


def test_disabled_case_debt_is_two_way():
    cmakes = {"tests/unit/CMakeLists.txt": "test_x.cpp"}
    text = '#if 0\n#ifdef FOO\nTEST_CASE("x") {}\n#endif\n#endif\n'
    assert disabled_counts(text) == (1, 0)
    assert tier_problems({"test_x.cpp": text}, cmakes, "", pins=(0, 0))
    assert tier_problems({"test_x.cpp": ""}, cmakes, "", pins=(1, 0))
