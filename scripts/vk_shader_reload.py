#!/usr/bin/env python3
"""Compile and ABI-check a native Vulkan shader, then request a frame-boundary reload.

Example: py -3 scripts/vk_shader_reload.py --install E:/mc2-installs/mc2-win64-render
         --shader terrain_lod.frag [--source path/to/edited.frag]
The game logs success or failure_keeps_old. No renderer/simulation update is run by this command.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

import console_child

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/shader_reflect"))
import reflect  # noqa: E402 -- reuse the repository's SPIRV-Cross reflection


def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{value:016x}"


def abi(raw):
    """Resolve transient SPIR-V type IDs, preserving complete nested layouts and stage interfaces.

    The golden-check normalizer intentionally covers a subset of the ABI. Reload must also compare
    sets, descriptor counts, push constants, image formats, array extents, entry points and workgroups.
    """
    types = raw.get("types", {})

    def canonical(item):
        if isinstance(item, list):
            return [canonical(v) for v in item]
        if not isinstance(item, dict):
            return item
        return {k: canonical(types[v]) if k == "type" and isinstance(v, str) and v in types
                else canonical(v) for k, v in item.items() if k != "name"}

    result = {}
    for key, value in raw.items():
        if key == "types":
            continue
        value = canonical(value)
        if isinstance(value, list):
            value = sorted(value, key=lambda v: json.dumps(v, sort_keys=True))
        result[key] = value
    return result


def publish(install, lines):
    path = install / "shader_reload.request"
    temp = path.with_suffix(".tmp")
    temp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    os.replace(temp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--install", type=Path, required=True)
    ap.add_argument("--shader", required=True, help="shader filename, e.g. terrain_lod.frag")
    ap.add_argument("--source", type=Path, help="edited GLSL/Slang source; default is the worktree shader")
    args = ap.parse_args()
    install = args.install.resolve()
    if Path(args.shader).name != args.shader or "\n" in args.shader:
        ap.error("--shader must be one filename")
    live = install / "shaders/vulkan" / (args.shader + ".spv")
    source = (args.source or ROOT / "shaders/vulkan" / args.shader).resolve()
    candidate = live.with_suffix(".reload.spv")
    try:
        tools = reflect._find_tools()
        if not tools:
            raise RuntimeError("glslangValidator and spirv-cross required")
        compiler, cross = tools
        if source.suffix == ".slang":
            sys.path.insert(0, str(ROOT / "tools/shader_offline_build"))
            import slang_compile
            ok, diagnostics = slang_compile.bake(source, candidate)
            if not ok:
                raise RuntimeError(diagnostics)
        else:
            proc = subprocess.run([compiler, "-V", str(source), "-I" + str(ROOT / "shaders/vulkan"),
                                   "-o", str(candidate)], capture_output=True, text=True,
                                  timeout=60, **console_child.child_kwargs(redirected=True))
            if proc.returncode:
                raise RuntimeError(proc.stdout + proc.stderr)
        contracts = []
        for path in (live, candidate):
            ok, diagnostics, raw = reflect.reflect_spv(path, cross)
            if not ok:
                raise RuntimeError(diagnostics)
            contracts.append(abi(raw))
        if contracts[0] != contracts[1]:
            raise RuntimeError("ABI mismatch: descriptors, push constants, block layout, stage IO or workgroup changed")
        publish(install, ["validated-v1", live.relative_to(install).as_posix(),
                          candidate.relative_to(install).as_posix(), fnv(live.read_bytes()), fnv(candidate.read_bytes())])
        print(f"[VK_SHADER_RELOAD_COMMAND] compile=ok reflection=compatible requested={args.shader}", flush=True)
        return 0
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        reason = str(exc).replace("\r", " ").replace("\n", " ")[:400]
        publish(install, ["rejected-v1", reason])
        print(f"[VK_SHADER_RELOAD_COMMAND] failure_keeps_old {reason}", flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
