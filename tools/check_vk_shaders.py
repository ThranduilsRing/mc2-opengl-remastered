#!/usr/bin/env python3
"""check_vk_shaders.py — VK-OFFLINE-PROOFS-1 SHADER COMPILE GATE.

Wraps the SAME SPIR-V compiler the build already uses (VULKAN-SHADER-TOOLCHAIN-1
in GameOS/gameos/CMakeLists.txt: glslangValidator -V, or glslc as fallback) over
every shaders/vulkan/*.{vert,frag,comp} file and stage-named Slang file, errors only.
Slang uses the shared bake helper (version, descriptors and GPU ABI checked). No CMake
configure/build needed.

Locates the compiler via (in order): --glslang / --glslc CLI flag, VULKAN_SDK
env var (Bin/bin subdir), PATH.

Usage:
    py -3 tools/check_vk_shaders.py
    py -3 tools/check_vk_shaders.py --shader-dir shaders/vulkan

Exit 0 = all shaders compiled clean. Exit 1 = at least one compile error
(printed verbatim from the compiler, file grouped). Exit 2 = no compiler found.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent / 'shader_offline_build'))
import slang_compile


def find_compiler(explicit_glslang, explicit_glslc):
    if explicit_glslang:
        return ("glslang", explicit_glslang)
    if explicit_glslc:
        return ("glslc", explicit_glslc)
    sdk = os.environ.get("VULKAN_SDK")
    candidates = []
    if sdk:
        for sub in ("Bin", "bin"):
            candidates.append(os.path.join(sdk, sub, "glslangValidator.exe"))
            candidates.append(os.path.join(sdk, sub, "glslangValidator"))
    for c in candidates:
        if os.path.isfile(c):
            return ("glslang", c)
    p = shutil.which("glslangValidator")
    if p:
        return ("glslang", p)
    p = shutil.which("glslc")
    if p:
        return ("glslc", p)
    return (None, None)


def compile_one(kind, exe, src_path):
    with tempfile.TemporaryDirectory() as td:
        spv = os.path.join(td, "out.spv")
        if src_path.endswith('.slang'):
            return slang_compile.bake(Path(src_path), Path(spv))
        if kind == "glslang":
            cmd = [exe, "-V", src_path, "-o", spv]
        else:
            cmd = [exe, src_path, "-o", spv]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60, **console_child.child_kwargs(redirected=True))
        except FileNotFoundError:
            return False, f"compiler not found: {exe}"
        ok = proc.returncode == 0 and os.path.isfile(spv)
        out = (proc.stdout or "") + (proc.stderr or "")
        return ok, out.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shader-dir", default="shaders/vulkan")
    ap.add_argument("--glslang", help="explicit path to glslangValidator")
    ap.add_argument("--glslc", help="explicit path to glslc")
    args = ap.parse_args()

    kind, exe = find_compiler(args.glslang, args.glslc)
    shader_dir = args.shader_dir
    if not os.path.isdir(shader_dir):
        print(f"check_vk_shaders: shader dir not found: {shader_dir}", file=sys.stderr)
        sys.exit(2)

    files = sorted(
        f for f in os.listdir(shader_dir)
        if f.endswith(('.vert', '.frag', '.comp', '.vert.slang', '.frag.slang', '.comp.slang'))
    )
    if not files:
        print(f"check_vk_shaders: no stage shaders in {shader_dir}", file=sys.stderr)
        sys.exit(2)

    if not exe and any(not f.endswith('.slang') for f in files):
        print('check_vk_shaders: glslangValidator or glslc required', file=sys.stderr)
        sys.exit(2)

    any_error = False
    checked = 0
    for fname in files:
        src = os.path.join(shader_dir, fname)
        ok, out = compile_one(kind, exe, src)
        checked += 1
        if not ok:
            any_error = True
            print(f"FAIL {fname}")
            for line in out.splitlines():
                print(f"    {line}")

    if any_error:
        sys.exit(1)
    print(f"check_vk_shaders: {checked} shader(s) compiled clean ({kind}: {exe})")
    sys.exit(0)


if __name__ == "__main__":
    main()
