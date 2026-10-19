#!/usr/bin/env python3
"""Guard native scene ownership after retiring the parallel GL attachments."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
PREFIX = "GameOS/gameos/"


def code(source):
    return re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', " ", source, flags=re.S)


def snapshot(root=ROOT):
    files = list((root / PREFIX).glob("gos_postprocess*.cpp")) + list((root / PREFIX).glob("gos_postprocess*.h"))
    files += [root / PREFIX / "CMakeLists.txt", root / PREFIX / "engine_present_vk.cpp", root / PREFIX / "engine_present_vk.h"]
    return {p.relative_to(root).as_posix(): p.read_text(encoding="utf-8") for p in files}


def audit(files):
    problems = []
    for path, source in files.items():
        if "gos_postprocess" not in path:
            continue
        tokens = code(source)
        if re.search(r"\bgl[A-Z]\w*\s*\(|\b(?:GLuint|GLint|GLenum|GLboolean)\b", tokens):
            problems.append(f"{path}: legacy GL call/type in native scene owner")
        if re.search(r'^\s*#\s*include\s*[<"](?:GL/|utils/gl_utils|utils/shader_builder|gl_state_guard)', source, re.M):
            problems.append(f"{path}: retired GL dependency")
        if re.search(r"\b(?:sceneFBO_|sceneColorTex_|sceneDepthTex_|sceneNormalTex_|copySceneDepthForParticles|copySceneColorForVfx)\b", tokens):
            problems.append(f"{path}: parallel legacy scene storage/copy")
    targets = code(files.get(PREFIX + "gos_postprocess_targets.cpp", ""))
    for call in [r"mc2SharedSceneColorCreate\s*\(\s*MC2_SHARED_SCENE_COLOR0", r"mc2SharedSceneColorCreate\s*\(\s*MC2_SHARED_SCENE_NORMAL", r"mc2SharedSceneDepthCreate\s*\("]:
        if not re.search(call, targets):
            problems.append("native scene allocation missing: " + call)
    for field in ["sharedSceneColor_", "sharedSceneNormal_", "sharedSceneObjectId_"]:
        if not re.search(r"return\s+" + field + r"\s*!=\s*nullptr", targets):
            problems.append("readiness must query the real owner: " + field)
        if not re.search(r"mc2SharedSceneColorDestroy\s*\(\s*" + field, targets):
            problems.append("native color owner teardown missing: " + field)
    if not re.search(r"mc2SharedSceneDepthDestroy\s*\(\s*sharedSceneDepth_", targets):
        problems.append("native depth owner teardown missing")
    core = code(files.get(PREFIX + "gos_postprocess.cpp", ""))
    for call in ["runVkPostFogNative", "beginNativeComposite", "renderNativeCompositeVisual"]:
        if not re.search(r"\b" + call + r"\s*\(", core):
            problems.append("native scene/composite path missing: " + call)
    cmake = files.get(PREFIX + "CMakeLists.txt", "")
    if "gos_postprocess_targets.cpp" not in cmake or "gos_postprocess_fbo.cpp" in cmake:
        problems.append("CMake must build the native scene owner")
    if PREFIX + "gos_postprocess_fbo.cpp" in files:
        problems.append("retired parallel GL scene owner still exists")
    for path in [PREFIX + "engine_present_vk.cpp", PREFIX + "engine_present_vk.h"]:
        if re.search(r"\b(?:presentDirectFbo|directFbo_)\b", code(files.get(path, ""))):
            problems.append(path + ": retired framebuffer sentinel API")
    return problems


def main():
    problems = audit(snapshot())
    for problem in problems:
        print("[native-scene-targets] FAIL: " + problem, file=sys.stderr)
    if not problems and "--quiet" not in sys.argv:
        print("[native-scene-targets] PASS: native owners/readiness/composite; no parallel GL scene storage")
    return bool(problems)


if __name__ == "__main__":
    sys.exit(main())
