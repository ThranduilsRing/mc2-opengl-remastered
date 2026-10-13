"""Every check and C++ test must have an executable route or a reasoned exemption."""
import ast
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools import callers


def entries(text, separator):
    return {name.strip(): reason.strip() for line in text.splitlines()
            if line.strip() and not line.lstrip().startswith("#")
            for name, _separator, reason in [line.partition(separator)]}


def coverage_problems(checks, inventory, manual, board, harnesses, registered, debt):
    issues = []
    items = {item["path"]: item for item in inventory["scripts"]}
    rows = re.findall(r'^run_check(?:_tri)?\s+"([^"]+)"\s+"([^"]+)"', board, re.M)
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
    issues = [f"orphan test source: {name}" for name in sources
              if name not in cmakes["tests/unit/CMakeLists.txt"]]
    for text in cmakes.values():
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
    checks = [item["path"] for item in inventory["scripts"]
              if (ROOT / item["path"]).is_file() and (
                  re.fullmatch(r'scripts/check[-_].*\.(?:py|sh)', item["path"])
                  or re.fullmatch(r'tools/(?:check_[\w-]+|glsl_port_diff)\.py', item["path"]))
              and item["path"] != "scripts/check-contracts.sh"]
    registered = set()
    for node in ast.walk(ast.parse(read("tools/run_contract_tests.py"))):
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id in
                {"REGISTERED_HARNESSES", "PY_HARNESSES"} for t in node.targets):
            registered.update(value.split("/")[1] if value.startswith("tools/") else value
                              for value in ast.literal_eval(node.value))
    harnesses = {path.parent.name for path in (ROOT / "tools").glob("*_harness/CMakeLists.txt")}
    issues = coverage_problems(checks, inventory, entries(read("scripts/manual-checks.allowlist"), ":"),
                               read("scripts/check-contracts.sh"), harnesses, registered,
                               entries(read("scripts/unregistered-harness-debt.txt"), "#"))
    cmakes = {path: read(path) for path in ("tests/unit/CMakeLists.txt",
              "tests/model_override/CMakeLists.txt", "tests/static_natural_skip/CMakeLists.txt")}
    sources = {path.name: path.read_text(encoding="utf-8")
               for path in (ROOT / "tests/unit").glob("test_*.cpp")}
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


def test_orphan_source_and_undriven_target_fail():
    cmakes = {"tests/unit/CMakeLists.txt": "add_executable(lost test_old.cpp)"}
    issues = tier_problems({"test_new.cpp": "TEST_CASE(\"x\") {}"}, cmakes, "", pins=(0, 0))
    assert "orphan test source: test_new.cpp" in issues
    assert "undriven test target: lost" in issues


def test_disabled_case_debt_is_two_way():
    cmakes = {"tests/unit/CMakeLists.txt": "test_x.cpp"}
    text = '#if 0\n#ifdef FOO\nTEST_CASE("x") {}\n#endif\n#endif\n'
    assert disabled_counts(text) == (1, 0)
    assert tier_problems({"test_x.cpp": text}, cmakes, "", pins=(0, 0))
    assert tier_problems({"test_x.cpp": ""}, cmakes, "", pins=(1, 0))
