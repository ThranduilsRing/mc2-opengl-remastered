"""The scene gate must reject storage and readiness regressions."""
import importlib.util
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("native_scene_targets", ROOT / "scripts/check-native-scene-targets.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def test_current_native_owners_satisfy_the_gate():
    assert checker.audit(checker.snapshot()) == []


@pytest.mark.parametrize("mutation, expected", [
    (lambda files: files.__setitem__(checker.PREFIX + "gos_postprocess.cpp", files[checker.PREFIX + "gos_postprocess.cpp"] + "\nvoid bad() { glGenTextures(1, &old); }\n"), "legacy GL"),
    (lambda files: files.__setitem__(checker.PREFIX + "gos_postprocess_targets.cpp", files[checker.PREFIX + "gos_postprocess_targets.cpp"].replace("return sharedSceneColor_ != nullptr;", "return true;")), "real owner"),
    (lambda files: files.__setitem__(checker.PREFIX + "gos_postprocess_targets.cpp", files[checker.PREFIX + "gos_postprocess_targets.cpp"].replace("mc2SharedSceneDepthDestroy(sharedSceneDepth_);", "")), "depth owner teardown"),
    (lambda files: files.__setitem__(checker.PREFIX + "engine_present_vk.h", files[checker.PREFIX + "engine_present_vk.h"] + "\nunsigned int directFbo_;\n"), "framebuffer sentinel"),
])
def test_gate_rejects_real_regressions(mutation, expected):
    files = checker.snapshot()
    mutation(files)
    assert any(expected in problem for problem in checker.audit(files))


def test_comments_and_diagnostic_strings_are_not_executable_gl():
    files = checker.snapshot()
    files[checker.PREFIX + "gos_postprocess.cpp"] += '\n// glGenTextures(1,&old);\nconst char* history="glBindFramebuffer(old)";\n'
    assert checker.audit(files) == []
