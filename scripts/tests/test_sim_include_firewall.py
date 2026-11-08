"""The S1 source firewall rejects forbidden ingress and ratchets exact findings."""
import copy
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from scripts import check_sim_include_firewall as firewall


def scope(pch=None):
    return {"schema_version": 1, "translation_units": ["Sim/kernel.cpp"],
            "include_dirs": ["Sim", "Core", "."], "pch": pch}


def scan(files, pch=None):
    return firewall.scan(firewall.Sources(ROOT, files), scope(pch))


# Each pair retains the same interface shape; only the forbidden ingress changes.
FIXTURES = [
    ("direct-camera", "camera",
     {"Sim/kernel.cpp": '#include "camera.h"\n', "Sim/camera.h": "class Camera {};"},
     {"Sim/kernel.cpp": '#include "snapshot.h"\n', "Sim/snapshot.h": "struct Snapshot {};"}, None),
    ("transitive-camera", "camera",
     {"Sim/kernel.cpp": '#include "Core/bridge.h"\n',
      "Core/bridge.h": '#include "camera.h"\n', "Core/camera.h": "class Camera {};"},
     {"Sim/kernel.cpp": '#include "Core/bridge.h"\n',
      "Core/bridge.h": '#include "snapshot.h"\n', "Core/snapshot.h": "struct Snapshot {};"}, None),
    ("timing-alias", "wall-clock",
     {"Sim/kernel.cpp": '#include "Core/time_view.h"\nauto now = TickNow();\n',
      "Core/time_view.h": "#define TickNow timeGetTime\n"},
     {"Sim/kernel.cpp": '#include "Core/time_view.h"\nauto now = TickNow(tick);\n',
      "Core/time_view.h": "unsigned TickNow(unsigned tick) { return tick; }\n"}, None),
    ("vm-wall-seed", "wall-clock",
     {"Sim/kernel.cpp": "void seedVm() { srand(time(nullptr)); }\n"},
     {"Sim/kernel.cpp": "void seedVm(unsigned missionSeed) { vm.setSeed(missionSeed); }\n"}, None),
    ("pch-leakage", "wall-clock",
     {"Sim/kernel.cpp": "void step(unsigned tick) {}\n", "Core/pch.h": "#include <chrono>\n"},
     {"Sim/kernel.cpp": "void step(unsigned tick) {}\n", "Core/pch.h": "#include <cstdint>\n"}, "Core/pch.h"),
    ("extern-clock-shim", "extern-shim",
     {"Sim/kernel.cpp": "extern double readNow();\nvoid step() { auto t = readNow(); }\n"},
     {"Sim/kernel.cpp": "double readNow(unsigned tick) { return tick / 30.0; }\nvoid step(unsigned tick) { auto t = readNow(tick); }\n"}, None),
]


@pytest.mark.parametrize("name,rule,negative,clean,pch", FIXTURES, ids=[f[0] for f in FIXTURES])
def test_design_negative_and_clean_twin(name, rule, negative, clean, pch):
    findings = scan(negative, pch)
    assert rule in {finding["rule"] for finding in findings}, name
    assert not scan(clean, pch), name
    assert all(f["tu"] == "Sim/kernel.cpp" and f["file"] and f["chain"] for f in findings)


def test_transitive_chain_and_pch_are_visible():
    _, _, negative, _, _ = FIXTURES[1]
    finding = next(f for f in scan(negative) if f["file"] == "Core/bridge.h")
    assert finding["file"] == "Core/bridge.h"
    assert finding["chain"] == ["Sim/kernel.cpp", "Core/bridge.h", "Core/camera.h"]
    _, _, negative, _, pch = FIXTURES[4]
    assert scan(negative, pch)[0]["chain"] == ["Sim/kernel.cpp", "<PCH>", "Core/pch.h", "<chrono>"]


@pytest.mark.parametrize("header,rule", [("GameOS/hidden.h", "gameos"),
    ("RenderCore/hidden.h", "rendercore"), ("SDL2/SDL.h", "sdl"),
    ("windows.h", "windows"), ("vulkan/vulkan.h", "gpu"),
    ("timing.h", "wall-clock"), ("sys/time.h", "wall-clock")])
def test_forbidden_headers_need_no_sdk(header, rule):
    assert scan({"Sim/kernel.cpp": f"#include <{header}>\n"})[0]["rule"] == rule


def test_resolved_location_matters_more_than_header_spelling():
    config = scope()
    config["include_dirs"].insert(0, "GameOS/include")
    sources = firewall.Sources(ROOT, {"Sim/kernel.cpp": '#include "innocent.h"\n',
                                    "GameOS/include/innocent.h": "struct Device {};"})
    assert firewall.scan(sources, config)[0]["rule"] == "gameos"


@pytest.mark.parametrize("source", ["using TickClock = std::chrono::steady_clock;",
    "typedef Clock TickClock;", "#define TickNow gos_GetElapsedTime",
    "auto read = &QueryPerformanceCounter;", "extern double (*readNow)();",
    "extern unsigned wallNow;", "void seed() { gos_srand(1); gos_rand(); }"])
def test_aliases_pointers_and_clock_data_cannot_hide(source):
    assert scan({"Sim/kernel.cpp": source})


def test_comments_literals_and_line_splices():
    clean = '// Camera\n/* #include "camera.h" */\nconst char* msg = "timeGetTime";\n'
    clean += 'const char* raw = R"tag(Camera /* chrono */)tag";\n'
    assert not scan({"Sim/kernel.cpp": clean})
    assert scan({"Sim/kernel.cpp": '#inc\\\nlude "camera.h"\n'})[0]["rule"] == "camera"
    assert scan({"Sim/kernel.cpp": 'timeGet\\\nTime();\n'})[0]["rule"] == "wall-clock"


def test_line_after_include_is_not_swallowed():
    # Blanking "x.h" left `#include` + spaces; the old \s-based INCLUDE then ate the next line.
    for tail in ("class Camera;\n", "auto t = timeGetTime();\n"):
        assert scan({"Sim/kernel.cpp": '#include "Core/x.h"\n' + tail, "Core/x.h": "struct X {};\n"}), tail


def test_stored_time_member_is_data_but_clock_calls_and_addresses_remain_forbidden():
    assert not scan({"Sim/kernel.cpp": "archive(orders.time, ptr->time);"})
    for source in ("time(nullptr);", "auto clockSource = &time;",
                   "device.time();", "device->time();", "std::time(nullptr);"):
        assert any(f["rule"] == "wall-clock" for f in scan({"Sim/kernel.cpp": source}))


def test_inactive_branch_is_conservative_and_header_macro_resolves():
    source = '#define VIEW "camera.h"\n#if 0\n#include VIEW\n#endif\n'
    assert scan({"Sim/kernel.cpp": source})[0]["rule"] == "camera"


@pytest.mark.parametrize("files,pch", [({}, None),
    ({"Sim/kernel.cpp": '#include "missing.h"\n'}, None),
    ({"Sim/kernel.cpp": '#include UNKNOWN_HEADER\n'}, None),
    ({"Sim/kernel.cpp": '#include "../.. /outside.h"\n'}, None),
    ({"Sim/kernel.cpp": "void step() {}"}, "Core/missing_pch.h")])
def test_missing_or_unresolved_inputs_fail_closed(files, pch):
    with pytest.raises(firewall.InputError):
        scan(files, pch)


def test_cycles_and_duplicate_paths_are_deterministic():
    files = {"Sim/kernel.cpp": '#include "Core/a.h"\n#include "Core/b.h"\n',
             "Core/a.h": '#include "b.h"\n',
             "Core/b.h": '#include "a.h"\n#include "camera.h"\n'}
    first = scan(files)
    assert len([f for f in first if f["subject"] == "camera.h"]) == 1
    assert scan(dict(reversed(list(files.items())))) == first
    direct = next(f for f in first if f["subject"] == "camera.h")
    assert direct["chain"] == ["Sim/kernel.cpp", "Core/b.h", "<camera.h>"]


def test_second_route_to_existing_violation_is_new():
    files = {"Sim/kernel.cpp": '#include "Core/bad.h"\n',
             "Core/bad.h": '#include "camera.h"\n'}
    old = scan(files)
    files["Sim/kernel.cpp"] += '#include "Core/bridge.h"\n'
    files["Core/bridge.h"] = '#include "bad.h"\n'
    added, removed = firewall.delta(scan(files), old)
    assert not removed
    assert any(f["file"] == "Core/bridge.h" and f["rule"] == "camera" for f in added)


def test_removing_route_does_not_turn_an_alternate_witness_into_growth():
    files = {"Sim/kernel.cpp": '#include "Core/a.h"\n#include "Core/b.h"\n',
             "Core/a.h": '#include "leaf.h"\n', "Core/b.h": '#include "leaf.h"\n',
             "Core/leaf.h": '#include "camera.h"\n'}
    old = scan(files)
    files["Sim/kernel.cpp"] = '#include "Core/b.h"\n'
    added, removed = firewall.delta(scan(files), old)
    assert not added and removed


def test_header_macro_redefinition_cannot_hide_earlier_camera_include():
    source = '#define VIEW "camera.h"\n#include VIEW\n#undef VIEW\n#define VIEW "snapshot.h"\n'
    files = {"Sim/kernel.cpp": source, "Sim/snapshot.h": "struct Snapshot {};"}
    assert any(f["rule"] == "camera" for f in scan(files))
    files["Sim/kernel.cpp"] += "#undef VIEW\n#define VIEW OPAQUE_MACRO\n"
    with pytest.raises(firewall.InputError):
        scan(files)


def test_new_occurrence_and_new_tu_are_new_violations():
    old = scan({"Sim/kernel.cpp": "timeGetTime();\n"})
    current = scan({"Sim/kernel.cpp": "timeGetTime();\ntimeGetTime();\n"})
    added, removed = firewall.delta(current, old)
    assert len(added) == 1 and not removed
    other = copy.deepcopy(old)
    other[0]["tu"] = "Sim/another.cpp"
    assert firewall.delta(old + other, old)[0] == other


def test_whitespace_and_comments_do_not_need_a_new_baseline():
    old = scan({"Sim/kernel.cpp": "timeGetTime();\n"})
    current = scan({"Sim/kernel.cpp": "// unrelated comment\n\n  timeGetTime(); // rationale\n"})
    assert firewall.delta(current, old) == ([], [])


def test_pch_exemption_is_explicit_and_missing_skip_list_fails():
    files = {"Sim/kernel.cpp": "void step() {}", "Core/pch.h": "#include <chrono>\n",
             "Core/skip.cmake": 'set(SKIP "Sim/kernel.cpp")\n'}
    config = scope("Core/pch.h")
    config["pch_skip"] = "Core/skip.cmake"
    assert not firewall.scan(firewall.Sources(ROOT, files), config)
    del files["Core/skip.cmake"]
    with pytest.raises(firewall.InputError):
        firewall.scan(firewall.Sources(ROOT, files), config)


class MemoryPath:
    """CLI storage fixture; no pytest temporary-directory plugin or git writes."""
    def __init__(self, value=None):
        self.value = value

    def exists(self):
        return self.value is not None

    def read_text(self, **kwargs):
        return self.value

    def write_text(self, value, **kwargs):
        self.value = value


def cli(monkeypatch, current, baseline, rebaseline=False, config=None):
    config = config or scope()
    args = type("Args", (), {"root": ROOT, "scope": MemoryPath(json.dumps(config)),
                            "baseline": baseline, "rebaseline": rebaseline})()
    monkeypatch.setattr(firewall.argparse.ArgumentParser, "parse_args", lambda *_: args)
    monkeypatch.setattr(firewall, "scan", lambda *_: current)
    return firewall.main([])


def test_rebaseline_refuses_growth_accepts_only_explicit_shrink(monkeypatch, capsys):
    old = scan({"Sim/kernel.cpp": "timeGetTime();"})
    original = json.dumps({"schema_version": 1, "scope": scope(), "violations": old})
    baseline = MemoryPath(original)
    new = scan({"Sim/kernel.cpp": "timeGetTime();\nclock();"})
    assert cli(monkeypatch, new, baseline, True) == 1
    assert baseline.value == original
    assert "delta: +1 -0" in capsys.readouterr().out
    assert cli(monkeypatch, [], baseline) == 1
    assert baseline.value == original
    assert "REMOVED wall-clock" in capsys.readouterr().out
    assert cli(monkeypatch, [], baseline, True) == 0
    assert "delta: +0 -1" in capsys.readouterr().out
    assert json.loads(baseline.value)["violations"] == []
    assert cli(monkeypatch, [], baseline) == 0


def test_initial_baseline_is_explicit_and_scope_cannot_silently_shrink(monkeypatch):
    baseline = MemoryPath()
    assert cli(monkeypatch, [], baseline) == 2
    assert not baseline.exists()
    assert cli(monkeypatch, [], baseline, True) == 0
    original = baseline.value
    altered = scope()
    altered["translation_units"] = ["Sim/other.cpp"]
    assert cli(monkeypatch, [], baseline, True, altered) == 2
    assert baseline.value == original


def test_committed_baseline_matches_current_tree():
    config = json.loads(firewall.SCOPE.read_text(encoding="utf-8"))
    baseline = json.loads(firewall.BASELINE.read_text(encoding="utf-8"))
    current = firewall.scan(firewall.Sources(ROOT), config)
    assert baseline["scope"] == config
    assert firewall.delta(current, baseline["violations"]) == ([], [])


@pytest.mark.parametrize("declaration", [
    "",
    '# set_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)',
    'set(UNUSED "\nset_source_files_properties(Sim/kernel.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n")',
    '#[[\nset_source_files_properties(Sim/kernel.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n]]',
    'set_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS OFF)',
    'if(FALSE)\nset_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\nendif()',
    'function(unused)\nset_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\nendfunction()',
    'set_source_files_properties("Sim/other.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)',
    'set_source_files_properties("Sim/kernel.cpp" "Sim/other.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)',
    'set_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n'
    'set_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS OFF)',
    'set_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n'
    'set_property(SOURCE "Sim/kernel.cpp" PROPERTY SKIP_PRECOMPILE_HEADERS OFF)',
    'set_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n'
    'set_property(SOURCE ${sim_sources} PROPERTY SKIP_PRECOMPILE_HEADERS OFF)',
    'set_source_files_properties("Sim/kernel.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n'
    'set_source_files_properties(${sim_sources} PROPERTIES SKIP_PRECOMPILE_HEADERS OFF)',
])
def test_explicit_pch_exclusion_requires_matching_actual_cmake(declaration):
    config = scope("Core/pch.h")
    config["pch_excluded"] = ["Sim/kernel.cpp"]
    files = {"Sim/kernel.cpp": "void step() {}", "Core/pch.h": "#include <chrono>\n",
             "CMakeLists.txt": declaration}
    with pytest.raises(firewall.InputError):
        firewall.scan(firewall.Sources(ROOT, files), config)


@pytest.mark.parametrize("path", ["Sim/kernel.cpp", "./Sim/kernel.cpp", "Sim/../Sim/kernel.cpp",
                                  "Sim\\kernel.cpp", (ROOT / "Sim/kernel.cpp").as_posix()])
@pytest.mark.parametrize("command", ["set_source_files_properties", "set_property"])
def test_equivalent_source_path_cannot_override_pch_exclusion(path, command):
    config = scope("Core/pch.h")
    config["pch_excluded"] = ["Sim/kernel.cpp"]
    override = (f'{command}("{path}" PROPERTIES SKIP_PRECOMPILE_HEADERS OFF)' if
                command == "set_source_files_properties" else
                f'{command}(SOURCE "{path}" PROPERTY SKIP_PRECOMPILE_HEADERS OFF)')
    files = {"Sim/kernel.cpp": "void step() {}", "Core/pch.h": "#include <chrono>\n",
             "CMakeLists.txt": 'set_source_files_properties("Sim/kernel.cpp" '
                               'PROPERTIES SKIP_PRECOMPILE_HEADERS ON)\n' + override}
    with pytest.raises(firewall.InputError):
        firewall.scan(firewall.Sources(ROOT, files), config)


@pytest.mark.parametrize("path", ["Sim/kernel.cpp", "./Sim/kernel.cpp", "Sim/../Sim/kernel.cpp"])
def test_explicit_pch_exclusion_passes_only_for_exact_unconditional_source_properties(path):
    config = scope("Core/pch.h")
    config["pch_excluded"] = ["Sim/kernel.cpp"]
    files = {"Sim/kernel.cpp": "void step() {}", "Core/pch.h": "#include <chrono>\n",
             "CMakeLists.txt": f'set_source_files_properties(\n "{path}"\n'
                               ' PROPERTIES SKIP_PRECOMPILE_HEADERS ON\n)\n'}
    assert not firewall.scan(firewall.Sources(ROOT, files), config)
    files["Sim/kernel.cpp"] = "timeGetTime();"
    assert {f["rule"] for f in firewall.scan(firewall.Sources(ROOT, files), config)} == {"wall-clock"}
    config["pch_excluded"].append("Sim/kernel.cpp")
    with pytest.raises(firewall.InputError):
        firewall.scan(firewall.Sources(ROOT, files), config)


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-q", "-p", "no:cacheprovider", "-p", "no:tmpdir"]))
