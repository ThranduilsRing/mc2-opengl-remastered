"""Planted-violation fixtures for scripts/check_boundaries.py (one fixture per carried assertion).

CASES is also consumed by the old-vs-new proof (the retired shell rows were run on the same fixtures). A case is
(name, files-to-write {rel: text}, files-to-delete [rel], must_fail). The clean baseline satisfies every corpus floor."""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import check_boundaries as cb  # noqa: E402
import console_child  # noqa: E402

COPIED = ["scripts/check-include-firewall.allowlist", "scripts/check-unified-projection-retirement.allowlist",
          "scripts/check-gate-echo-reads.py", "scripts/check_boundaries.py", "scripts/boundary_rules.json"]

DUMMY = "int f() { return 0; }\n"
BASELINE = {f"dummy/d{i}.cpp": DUMMY for i in range(520)}
BASELINE.update({f"mclib/particles/p{i}.cpp": "void p() {}\n" for i in range(3)})
BASELINE["mclib/model_override_registry.cpp"] = '#include <nlohmann/json.hpp>\n'
BASELINE["shaders/ok.vert"] = "void main() {}\n"
BASELINE["RenderCore/ok.cpp"] = DUMMY
BASELINE["Visibility/ok.h"] = DUMMY
BASELINE["editor/ok.cpp"] = DUMMY
BASELINE["code/ok.cpp"] = DUMMY

RAW_DECISION = 'void f() { if (mc2::gate::raw<"MC2_A">()) go(); }\n'
CASES = [
    # include_firewall: headers, symbols, editor, allowlist, comment filters
    ("fw header include", {"RenderCore/x.cpp": '#include "mech3d.h"\n'}, [], True),
    ("fw glew include", {"RenderCore/x.cpp": "#include <GL/glew.h>\n"}, [], True),
    ("fw symbol decl", {"Visibility/x.h": "class Mission;\n"}, [], True),
    ("fw symbol in signature", {"RenderWorld/x.h": "void f(Appearance* a);\n"}, [], True),
    ("fw editor gpu include", {"editor/x.cpp": '#include "gos_mech_batcher.h"\n'}, [], True),
    ("fw allowlisted carve-out", {"RenderWorld/legacy/static_prop_backend.cpp": '#include "gos_static_prop_batcher.h"\n'}, [], False),
    ("fw comment line ok", {"RenderCore/x.cpp": "// include mech3d.h and Mission\n"}, [], False),
    ("fw enumerator and lifetime ok", {"RenderCore/x.h": "enum E {\n Mission = 3,\n};\nauto a = RenderResourceLifetime::Mission;\nconst char* s = \"Mission\";\n"}, [], False),
    ("fw editor may use game symbols", {"editor/x.cpp": "Mission* m;\n"}, [], False),
    # sim_no_audio_reads
    ("audio read in sim", {"code/x.cpp": "int m = gosAudio_GetChannelPlayMode(1);\n"}, [], True),
    ("audio read in gui", {"gui/x.cpp": "gosAudio_GetVolume (2);\n"}, [], True),
    ("audio read allowed in soundsys", {"mclib/soundsys.cpp": "gosAudio_GetChannelPlayMode(1);\n"}, [], False),
    ("audio read in comment ok", {"code/x.cpp": "// gosAudio_GetVolume(2)\n"}, [], False),
    # no_gl_census
    ("gl header include", {"code/x.cpp": "#include <GL/glew.h>\n"}, [], True),
    ("gl call", {"code/x.cpp": "void f() { glClear(0); }\n"}, [], True),
    ("gl call in comment/string ok", {"code/x.cpp": '// glClear(0)\nconst char* s = "glClear(";\n/* glFoo(1)\n * glBar(2)\n */\n'}, [], False),
    ("gl in 3rdparty ok", {"3rdparty/gl.cpp": "void f() { glClear(0); }\n"}, [], False),
    ("gl blind corpus", {}, ["__few__"], True),
    # json_isolation
    ("json include elsewhere", {"code/x.cpp": "#include <nlohmann/json.hpp>\n"}, [], True),
    ("json include allowed basename", {"mclib/sub/fx_def_registry.cpp": "#include <nlohmann/json.hpp>\n"}, [], False),
    ("json blind corpus", {}, ["mclib/model_override_registry.cpp"], True),
    # particles_no_cpu_projection
    ("particles cpu projection", {"mclib/particles/a.cpp": "auto m = worldToClipMatrix;\n"}, [], True),
    ("particles blind corpus", {}, ["mclib/particles/p0.cpp", "mclib/particles/p1.cpp"], True),
    # unified_projection_retirement
    ("terrainMVP in shader", {"shaders/x.vert": "uniform mat4 terrainMVP;\n"}, [], True),
    ("terrainMVP string in cpu", {"code/x.cpp": 'auto l = glGetLoc("u_terrainMVP");\n'}, [], True),
    ("abs w kludge", {"shaders/x.vert": "float a = abs(clip.w);\n"}, [], True),
    ("terrainViewport in cpu", {"mclib/x.cpp": "int terrainViewport = 0;\n"}, [], True),
    ("screen rhw round trip", {"shaders/x.vert": "float a = z * rhw + 1.0;\n"}, [], True),
    ("bare mvp uniform", {"shaders/y.vert": "uniform mat4 mvp;\n"}, [], True),
    ("bare mvp allowlisted hud shader", {"shaders/gos_text.vert": "uniform mat4 mvp;\n"}, [], False),
    ("terrainMVP in shader comment ok", {"shaders/x.vert": "// terrainMVP retired\n"}, [], False),
    # gate_echo_reads
    ("raw decision", {"code/x.cpp": RAW_DECISION}, [], True),
    ("envText elsewhere", {"code/x.cpp": 'void f() { if (mc2::gate::envText("MC2_A")) go(); }\n'}, [], True),
    ("boolState elsewhere", {"code/x.cpp": "void f() { if (mc2::gate::boolState(n) == 1) go(); }\n"}, [], True),
    ("raw in printf ok", {"code/x.cpp": 'void f() { printf("%s", mc2::gate::raw<"MC2_A">()); }\n'}, [], False),
]


def build_fixture(root: Path) -> None:
    for rel, text in BASELINE.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8", newline="\n")
    for rel in COPIED:
        (root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / rel, root / rel)
    subprocess.run(["git", "init", "-q", str(root)], check=True, **console_child.child_kwargs())


def apply_case(root: Path, files: dict, deletes: list) -> list[Path]:
    touched = []
    for d in deletes:
        if d == "__few__":
            for p in sorted((root / "dummy").glob("*.cpp"))[:60]:
                p.unlink()
            continue
        (root / d).unlink()
    for rel, text in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8", newline="\n")
        touched.append(p)
    return touched


@pytest.fixture(scope="module")
def tree(tmp_path_factory):
    root = tmp_path_factory.mktemp("boundary")
    build_fixture(root)
    return root


def test_clean_baseline_passes(tree):
    errs, _ = cb.run(tree, cb.DEFAULT_TABLE)
    assert errs == []


@pytest.mark.parametrize("name,files,deletes,must_fail", CASES, ids=[c[0] for c in CASES])
def test_planted_case(tmp_path_factory, name, files, deletes, must_fail):
    root = tmp_path_factory.mktemp("case")
    build_fixture(root)
    apply_case(root, files, deletes)
    errs, _ = cb.run(root, cb.DEFAULT_TABLE)
    assert bool(errs) == must_fail, errs


def test_every_rule_has_a_planted_failure():
    import json
    ids = {r["id"].split(".")[0] for r in json.loads(cb.DEFAULT_TABLE.read_text())["rules"]}
    covered = {"include_firewall", "sim_no_audio_reads", "no_gl", "json_isolation", "particles_no_cpu_projection",
               "unified_projection", "gate_echo_reads"}
    assert ids == covered
