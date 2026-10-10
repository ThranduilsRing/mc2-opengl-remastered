#!/usr/bin/env python3
# scripts/validate_shaders.py
"""MC2 shader validator -- Tier 1.1 of docs/testing-strategy.md.

Walks shaders/*.{vert,frag,tesc,tese,comp,geom}, prepends the canonical
"#version 430" header (matching makeProgram() discipline -- shader files
themselves never carry a #version directive), and runs:

    glslangValidator --auto-map-locations -V -I<shaders/include> -S <stage> <tmpfile>
    spirv-val <emitted.spv>

Both must exit 0 for the shader to pass. Failure prints the offending
shader + tool diagnostic and accumulates; exit code is 0 if all pass,
1 if any failed.

Validator-prep prefix (path (a), per testing-strategy):
  * If the shader source contains a `#include` directive, the temp file
    additionally gets `#extension GL_GOOGLE_include_directive : require`
    after the #version line. The engine's makeProgram() tolerates the
    missing extension; glslangValidator does not. This injection keeps
    the validator-side gate matched to what the engine actually compiles.
  * `--auto-map-locations` is passed in both Vulkan and GL modes so
    glslangValidator auto-assigns the layout(location=) qualifiers that
    Vulkan/SPIR-V requires but the engine does not (engine uses
    glGetUniformLocation at runtime; OpenGL auto-assigns input/output
    varyings). Vulkan-strict tightening (explicit layout qualifiers on
    every uniform / varying) is deferred to when Vulkan-prep is on the
    active roadmap.

Skipped shaders:
  * `shaders/gos_terrain_lighting.comp` is programmatically stitched at
    runtime by tl_build_terrain_lighting_program (its include of
    terrain_lighting_shared.hglsl is intentionally commented out in the
    file on disk). Standalone validation cannot reach this shader
    without replicating the engine's include stitching, so it is
    explicitly skipped and counted separately.

Why prepend #version: shaders in this tree are loaded via
GameOS/gameos/gameos_graphics.cpp `makeProgram()`, which concatenates a
"#version 430\n" prefix before compile. Validating the file as-written
would fail every shader for the wrong reason.

Why -I shaders/include: the include/ subdir holds shared .hglsl snippets
referenced via #include directives. glslangValidator resolves them with
-I<dir> (note: NO space between -I and the path).

Tool discovery: prefers $VULKAN_SDK/Bin/{glslangValidator,spirv-val} on
Windows. Falls back to PATH. If neither resolves, exits with a clear
"install Vulkan SDK" error -- never silently skips.

Pre-commit wiring (optional, manual): add a one-liner to
.git/hooks/pre-commit:

    exec py -3 scripts/validate_shaders.py || exit 1

Modes:
    --vulkan   default; emits SPIR-V + runs spirv-val (Vulkan-prep gate)
    --gl       GL-semantics only, no SPIR-V emission
    --shader X validate only X (repeatable)

Exit code: 0 = all passed; 1 = any failed or environment broken.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# ensure scripts/ dir is importable when invoked as `py -3 scripts/validate_shaders.py`
sys.path.insert(0, str(Path(__file__).parent))
import shader_common
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

ROOT = shader_common.ROOT
SHADERS = shader_common.SHADERS


def _resolve_tools() -> tuple[str, str] | None:
    """Return (glslang, spirv_val) absolute paths, or None if missing."""
    glslang = shader_common.find_tool("glslangValidator")
    spirv_val = shader_common.find_tool("spirv-val")
    if not glslang or not spirv_val:
        return None
    return glslang, spirv_val


def _decode(data: bytes | str) -> str:
    if isinstance(data, str):
        return data
    return data.decode("utf-8", errors="replace")


def _annotate(shader: Path, tmp: Path, stdout: str, stderr: str,
              phase: str = "glslang") -> str:
    """Format a diagnostic, rewriting tmp basename back to the real shader path."""
    body = (stdout + stderr).strip()
    body = body.replace(tmp.name, shader.name)
    body = body.replace(str(tmp), str(shader))
    rel = shader.relative_to(ROOT)
    return f"--- {rel} ({phase}) ---\n{body}"


def _first_error_line(diag: str) -> str:
    """Extract the first ERROR: line for terse summary."""
    for line in diag.splitlines():
        s = line.strip()
        if s.startswith("ERROR:") or "error" in s.lower():
            return s
    # fallback: first non-empty non-header line
    for line in diag.splitlines():
        s = line.strip()
        if s and not s.startswith("---"):
            return s
    return diag.strip()[:200]


def validate_one(shader: Path, vulkan: bool,
                 glslang: str, spirv_val: str) -> tuple[bool, str]:
    """Return (passed, diagnostic). diagnostic is empty on success."""
    stage = shader_common.STAGE_BY_EXT[shader.suffix]
    try:
        src = shader_common.build_shader_source(shader)
    except OSError as e:
        return False, f"--- {shader.relative_to(ROOT)} (read) ---\ncannot read shader: {e}"
    except ValueError as e:
        return False, f"--- {shader.relative_to(ROOT)} (include) ---\n{e}"

    # Write to a temp file in the shader dir so relative #include paths
    # (if any are not in -I) and error messages stay sensible. The dir
    # also makes -I<shaders/include> resolution natural.
    fd, tmp_name = tempfile.mkstemp(
        suffix=shader.suffix, prefix="_validate_", dir=str(SHADERS)
    )
    tmp = Path(tmp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(src)

        cmd = [
            glslang,
            "-S", stage,
            # NOTE: no -I flags. The Python pre-processor above
            # (parse_includes_engine_style) already flattens every
            # `#include` directive using engine semantics. glslang's
            # -I-based resolver uses search paths which the engine does
            # not, so feeding glslang the flattened source avoids the
            # divergence entirely.
            # Auto-assign layout(location=) on input/output varyings and
            # uniforms the engine does not declare explicitly. Engine
            # uses glGetUniformLocation at runtime; GL auto-assigns
            # varyings. Vulkan/SPIR-V demands them.
            "--auto-map-locations",
            # Auto-assign layout(binding=) on samplers/images. Engine
            # binds samplers via glUniform1i at runtime; Vulkan/SPIR-V
            # demands explicit bindings.
            "--auto-map-bindings",
        ]
        if vulkan:
            # -R = relaxed Vulkan rules: allow default uniforms (i.e.
            # `uniform mat4 mvp;` outside a block), atomic_uints, etc.
            # This matches what the engine compiles via makeProgram();
            # full Vulkan-strict (UBO blocks) is deferred to Vulkan-prep.
            cmd += ["-V", "-R"]
        else:
            # -G emits SPIR-V under OpenGL semantics (which permits the
            # same default-uniform style the engine relies on).
            cmd += ["-G"]
        cmd += [str(tmp)]

        # glslangValidator with -V or -G writes <input>.spv in CWD by
        # default; pass -o to keep it next to the temp file and easy to
        # clean. Both modes emit SPIR-V here (Vulkan via -V, GL via -G).
        spv_path = tmp.with_suffix(tmp.suffix + ".spv")
        cmd += ["-o", str(spv_path)]

        proc = subprocess.run(cmd, capture_output=True, **console_child.child_kwargs(redirected=True))
        stdout = _decode(proc.stdout)
        stderr = _decode(proc.stderr)
        if proc.returncode != 0:
            return False, _annotate(shader, tmp, stdout, stderr, phase="glslang")

        # Only spirv-val in Vulkan mode -- spirv-val targets the Vulkan
        # SPIR-V environment; GL-semantics SPIR-V can fail rules that
        # are GL-legal (and the engine doesn't consume the .spv anyway).
        if vulkan and spv_path.exists():
            v = subprocess.run([spirv_val, str(spv_path)], capture_output=True)
            v_stdout = _decode(v.stdout)
            v_stderr = _decode(v.stderr)
            if v.returncode != 0:
                return False, _annotate(shader, tmp, v_stdout, v_stderr,
                                        phase="spirv-val")

        return True, ""
    finally:
        try:
            tmp.unlink()
        except OSError:
            pass
        try:
            spv_path.unlink()
        except (OSError, NameError, UnboundLocalError):
            pass


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Validate MC2 shaders with glslangValidator + spirv-val."
    )
    ap.add_argument("--shader", action="append",
                    help="Validate only this shader (repeatable). Default: all.")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--vulkan", action="store_true",
                      help="Emit SPIR-V + run spirv-val (default; Vulkan-prep).")
    mode.add_argument("--gl", action="store_true",
                      help="GL-semantics validation only (no SPIR-V).")
    args = ap.parse_args()

    tools = _resolve_tools()
    if tools is None:
        sdk = os.environ.get("VULKAN_SDK", "<unset>")
        print(
            "validate_shaders: cannot find glslangValidator and/or spirv-val.\n"
            f"  $VULKAN_SDK = {sdk}\n"
            "  Install the Vulkan SDK from https://vulkan.lunarg.com/ and ensure\n"
            "  $VULKAN_SDK/Bin/glslangValidator (+ spirv-val) exist, or place\n"
            "  them on PATH.",
            file=sys.stderr,
        )
        return 1
    glslang, spirv_val = tools

    if args.shader:
        targets = [Path(s).resolve() for s in args.shader]
        # Validate the user gave us recognised stages.
        for t in targets:
            if t.suffix not in shader_common.STAGE_BY_EXT:
                print(f"validate_shaders: unknown shader stage for {t}",
                      file=sys.stderr)
                return 1
    else:
        targets = shader_common.discover_shaders()

    if not targets:
        print("validate_shaders: no shaders found", file=sys.stderr)
        return 1

    vulkan = not args.gl   # default = Vulkan-prep mode
    passed = 0
    failed = 0
    skipped = 0
    failures: list[str] = []
    for sh in targets:
        rel = sh.relative_to(ROOT)
        # Skip shaders the engine stitches programmatically at runtime;
        # standalone validation cannot reach them. Counted separately.
        skip_reason = shader_common.SKIP_SHADERS.get(sh.name)
        if skip_reason is not None:
            skipped += 1
            print(f"[SKIP] {rel}: {skip_reason}")
            continue
        ok, diag = validate_one(sh, vulkan=vulkan,
                                glslang=glslang, spirv_val=spirv_val)
        if ok:
            passed += 1
            print(f"[PASS] {rel}")
        else:
            failed += 1
            first = _first_error_line(diag)
            print(f"[FAIL] {rel}: {first}")
            failures.append(diag)

    total = passed + failed + skipped
    mode_label = "Vulkan/SPIR-V" if vulkan else "GL-only"
    print(f"\nvalidate_shaders: {passed} passed, {failed} failed, "
          f"{skipped} skipped (out of {total}) [{mode_label}]")

    if failures:
        print("\n" + "=" * 72)
        print("Full diagnostics:\n")
        for diag in failures:
            print(diag)
            print()
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
