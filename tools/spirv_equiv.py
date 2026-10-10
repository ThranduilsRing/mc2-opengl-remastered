#!/usr/bin/env python3
"""spirv_equiv.py -- prove a Vulkan shader refactor left the compiled program unchanged.

What: compiles each named shaders/vulkan/*.vert|*.frag at a git revision (default HEAD) and in the worktree with
glslangValidator -V, optimises both with `spirv-opt -O --canonicalize-ids` (inlines the helper calls, renumbers the ids), disassembles
and compares (module-scope declarations as a sorted set, function bodies verbatim). Identical optimised disassembly = the refactor (a helper function replacing a hand-typed expression) compiles
to the same program; a difference prints a unified diff and exits 1.
Why: D11 replaces triplets such as vec3(-a.x, a.z, a.y) with mc2ToRender(a); the GLSL text changes but the SPIR-V must not.
Usage: py -3 tools/spirv_equiv.py [--rev REV] shaders/vulkan/mech_tier1.vert ...
Exit: 0 all equal, 1 a difference or compile failure, 2 tool not found.
"""
import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile

SDK_GLOBS = ("C:/VulkanSDK",)


def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    sdk = os.environ.get("VULKAN_SDK")
    cands = []
    if sdk:
        cands.append(os.path.join(sdk, "Bin", name + ".exe"))
    for root in SDK_GLOBS:
        if os.path.isdir(root):
            for v in sorted(os.listdir(root), reverse=True):
                cands.append(os.path.join(root, v, "Bin", name + ".exe"))
    for c in cands:
        if os.path.exists(c):
            return c
    return None


def run(cmd, cwd=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors="replace")


def compile_one(glslang, opt, dis, root, rel, tmp, tag):
    spv = os.path.join(tmp, tag + ".spv")
    r = run([glslang, "-V", rel.replace("\\", "/"), "-o", spv], cwd=root)
    if r.returncode != 0:
        return None, "glslang failed for %s (%s):\n%s%s" % (rel, tag, r.stdout, r.stderr)
    o = os.path.join(tmp, tag + ".opt.spv")
    r = run([opt, "-O", "--canonicalize-ids", spv, "-o", o])
    if r.returncode != 0:
        return None, "spirv-opt failed for %s (%s):\n%s%s" % (rel, tag, r.stdout, r.stderr)
    r = run([dis, "--no-header", o])
    if r.returncode != 0:
        return None, "spirv-dis failed for %s (%s)" % (rel, tag)
    return canonical(r.stdout), ""


def canonical(text):
    """Global declarations (types, constants, decorations) in sorted order, then the function bodies verbatim.

    The helper call changes which declaration the compiler meets first, so the order of the module-scope section is not
    meaningful; the instruction streams inside the functions are, and must match exactly."""
    lines = text.splitlines()
    cut = next((i for i, l in enumerate(lines) if "OpFunction" in l), len(lines))
    return chr(10).join(sorted(lines[:cut]) + ["---"] + lines[cut:])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rev", default="HEAD")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    glslang, opt, dis = find_tool("glslangValidator"), find_tool("spirv-opt"), find_tool("spirv-dis")
    if not (glslang and opt and dis):
        print("spirv_equiv: glslangValidator/spirv-opt/spirv-dis not found (set VULKAN_SDK)", file=sys.stderr)
        return 2
    repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    tmp = tempfile.mkdtemp(prefix="spirv_equiv_")
    bad = 0
    try:
        old_root = os.path.join(tmp, "old")
        os.makedirs(old_root)
        tar = os.path.join(tmp, "old.tar")
        r = subprocess.run(["git", "archive", "--format=tar", "-o", tar, a.rev, "shaders"], cwd=repo, capture_output=True)
        if r.returncode != 0:
            print("spirv_equiv: git archive failed: %s" % r.stderr.decode("utf-8", "replace"), file=sys.stderr)
            return 2
        subprocess.run(["tar", "-xf", tar, "-C", old_root], check=True)
        for rel in a.files:
            old, e1 = compile_one(glslang, opt, dis, old_root, rel, tmp, "old")
            new, e2 = compile_one(glslang, opt, dis, repo, rel, tmp, "new")
            if old is None or new is None:
                print("FAIL %s\n%s%s" % (rel, e1, e2))
                bad += 1
            elif old == new:
                print("SAME %s (%d optimised instructions)" % (rel, old.count("\n")))
            else:
                bad += 1
                print("DIFF %s" % rel)
                for l in list(difflib.unified_diff(old.splitlines(), new.splitlines(), "old", "new", lineterm="", n=2))[:60]:
                    print("   " + l)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
