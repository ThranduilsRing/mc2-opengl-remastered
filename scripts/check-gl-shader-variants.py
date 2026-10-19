#!/usr/bin/env python3
"""check-gl-shader-variants.py -- offline GL shader variant-matrix check.

Why: the engine compiles each GL program with a C++-built prefix (#version +
#define set) in front of the include-flattened shader text.  A shader that is
fine under one define set can be broken under another, and a bad include splice
only shows up on the variant whose #ifdef touches it (2026-10-09: an include
spliced into a comment line of static_prop.frag left a stray "#ifdef MC2_COALESCE
region below..." line, the GL prop program failed to compile and ALL GPU props
were disabled on both backends -- invisible to every other offline gate).

What it does (all offline; no GL context, no build):
  1. FRESHNESS   scripts/gl_shader_variants.json must equal a fresh re-extraction
                 from GameOS/gameos/** (scripts/extract_gl_shader_variants.py).
  2. MATRIX      for every manifest variant, assemble the source exactly as the
                 engine does (prefix + engine-style #include flattening, or raw
                 text / predicate / marker splice for the compute loaders that
                 bypass shader_builder), write each stage, and run
                     glslangValidator -l <all stages of the program>
                 i.e. compile every stage under desktop GLSL AND LINK them, so
                 vert<->frag interface mismatches fail too.  Diagnostics are
                 mapped back to real shaders/ file:line through #line ids.
  3. REGRESSION  proves the matrix rejects a malformed directive in a live
                 fragment, after the unmodified control compiled successfully.

Limit: glslang is not an NVIDIA/AMD driver; it will not see driver-specific
quirks.  Exit 0 = all variants link; 1 = any failure / stale manifest / tool
missing.

Usage:
    py -3 scripts/check-gl-shader-variants.py [--quiet] [--jobs N]
        [--only SUBSTR] [--list] [--no-selftest] [--shaders-dir DIR]
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import shader_common  # noqa: E402
import extract_gl_shader_variants as extractor  # noqa: E402

MANIFEST = extractor.MANIFEST
EXT_BY_STAGE = {v: k for k, v in shader_common.STAGE_BY_EXT.items()}
_LINE_RE = re.compile(r"^#line (\d+) // (.*)$", re.M)


def find_glslang() -> str | None:
    p = shader_common.find_tool("glslangValidator")
    if p:
        return p
    for cand in sorted(glob.glob("C:/VulkanSDK/*/Bin/glslangValidator.exe"), reverse=True):
        return cand
    return None


# ---------------------------------------------------------------------------
# Source assembly (mirrors shader_builder.cpp / the compute loaders)
# ---------------------------------------------------------------------------
class Assembler:
    def __init__(self, shaders_dir: Path):
        self.shaders_dir = shaders_dir           # the "shaders/" directory
        self.base = shaders_dir.parent           # dir that "shaders/x" is relative to

    def path(self, rel: str) -> Path:
        assert rel.startswith("shaders/"), rel
        return self.shaders_dir / rel[len("shaders/"):]

    # glslang only declares the GL_ARB_shader_draw_parameters builtins under
    # "#version 460" (where they are the core gl_* names). The ARB spellings are
    # pure aliases of them, so for programs whose prefix enables the extension we
    # compile as 460 with the ARB names rewritten -- the same shim
    # shader_common/reflect.py use for the SPIR-V pilots. Nothing else changes.
    _ARB = (("gl_BaseInstanceARB", "gl_BaseInstance"), ("gl_DrawIDARB", "gl_DrawID"),
            ("gl_BaseVertexARB", "gl_BaseVertex"))

    def assemble(self, variant: dict, rel: str) -> tuple[str, dict]:
        src, ids = self._assemble(variant, rel)
        if "GL_ARB_shader_draw_parameters" in variant["prefix"]:
            src = re.sub(r"^#version\s+\d+", "#version 460", src, count=1, flags=re.M)
            for a, b in self._ARB:
                src = src.replace(a, b)
        return src, ids

    def _assemble(self, variant: dict, rel: str) -> tuple[str, dict]:
        """Return (full source, {line-id: display path})."""
        prefix = variant["prefix"]
        mode = variant["assemble"]
        splice = variant.get("splice")
        p = self.path(rel)
        text = p.read_text(encoding="utf-8", errors="replace")
        ids: dict[int, str] = {}
        if mode == "engine":
            flat = shader_common.parse_includes_engine_style(text, str(p))
            flat = self._number_lines(flat, ids)
            return prefix + flat, ids
        # raw loaders -----------------------------------------------------
        if splice and "include_line" in splice:        # gpu_cull_compute.cpp
            sp = self.path(splice["file"]).read_text(encoding="utf-8", errors="replace")
            inc = splice["include_line"]
            k = text.find(inc)
            if k >= 0:
                text = (text[:k] + "\n// --- begin " + Path(splice["file"]).name + " ---\n" +
                        sp + "\n// --- end " + Path(splice["file"]).name + " ---\n" +
                        text[k + len(inc):])
        elif splice and "begin_marker" in splice:      # gos_terrain_lighting.cpp
            sp = (self.base / splice["file"]).read_text(encoding="utf-8", errors="replace")
            b = text.find(splice["begin_marker"])
            e = text.find(splice["end_marker"], b) if b >= 0 else -1
            if b >= 0 and e >= 0:
                text = (text[:b] + "\n// --- begin terrain_lighting_shared.hglsl (inlined) ---\n" +
                        sp + "\n// --- end terrain_lighting_shared.hglsl (inlined) ---\n" +
                        text[e + len(splice["end_marker"]):])
        return prefix + text, ids

    @staticmethod
    def _number_lines(flat: str, ids: dict) -> str:
        """`#line N // path` -> `#line N <id>` so glslang reports the id."""
        paths: dict[str, int] = {}

        def sub(m):
            path = m.group(2).strip()
            if path not in paths:
                paths[path] = len(paths) + 1
                try:
                    disp = Path(path).resolve().relative_to(ROOT).as_posix()
                except (ValueError, OSError):
                    disp = path.replace("\\", "/")
                ids[paths[path]] = disp
            return f"#line {m.group(1)} {paths[path]}"

        return _LINE_RE.sub(sub, flat)


# ---------------------------------------------------------------------------
# One variant
# ---------------------------------------------------------------------------
_ERR_RE = re.compile(r"^(ERROR|WARNING): (\d+):(\d+): (.*)$")


def map_diag(out: str, ids_by_stage: dict, stage_by_file: dict) -> list[str]:
    lines = []
    for ln in out.splitlines():
        s = ln.strip()
        if not s:
            continue
        m = _ERR_RE.match(s)
        if m and m.group(1) == "ERROR":
            fid, line = int(m.group(2)), int(m.group(3))
            # which stage? glslang prints "<file>" header lines before the errors
            lines.append(("ERROR", fid, line, m.group(4)))
        else:
            lines.append(("RAW", 0, 0, s))
    # attribute by walking glslang's "<tmpfile>" headers
    cur = None
    result = []
    for ln in out.splitlines():
        s = ln.strip()
        if s in stage_by_file:
            cur = stage_by_file[s]
            continue
        m = _ERR_RE.match(s)
        if m and m.group(1) == "ERROR":
            fid, line = int(m.group(2)), int(m.group(3))
            ids = ids_by_stage.get(cur, {})
            where = ids.get(fid)
            if where is None:
                where = f"<{cur or '?'} prefix/#version region>" if fid == 0 else f"<{cur}:str{fid}>"
            result.append(f"{where}:{line}: {m.group(4)}")
        elif s.startswith("ERROR:") or "error" in s.lower() and not s.startswith("Warning"):
            result.append(s)
    return result


def run_variant(glslang: str, asm: Assembler, variant: dict, workdir: Path) -> tuple[bool, list[str]]:
    key = variant["key"]
    d = workdir / re.sub(r"[^A-Za-z0-9_.-]", "_", key)[:80]
    d.mkdir(parents=True, exist_ok=True)
    files, ids_by_stage, stage_by_file = [], {}, {}
    try:
        for stage, rel in variant["stages"].items():
            src, ids = asm.assemble(variant, rel)
            fp = d / f"p{EXT_BY_STAGE[stage]}"
            fp.write_text(src, encoding="utf-8", newline="\n")
            files.append(str(fp))
            ids_by_stage[stage] = ids
            stage_by_file[fp.name] = stage
            stage_by_file[str(fp)] = stage
    except (OSError, ValueError) as e:
        return False, [f"assemble: {e}"]
    proc = subprocess.run([glslang, "-l"] + files, capture_output=True, cwd=str(d))
    out = (proc.stdout + proc.stderr).decode("utf-8", errors="replace")
    if proc.returncode == 0:
        shutil.rmtree(d, ignore_errors=True)
        return True, []
    diag = map_diag(out, ids_by_stage, stage_by_file)
    if not diag:
        diag = [x.strip() for x in out.splitlines() if x.strip()][:6]
    return False, diag


def run_matrix(glslang: str, shaders_dir: Path, variants: list, jobs: int,
               tmp: Path) -> list[tuple[dict, list[str]]]:
    asm = Assembler(shaders_dir)
    failures = []
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(run_variant, glslang, asm, v, tmp): v for v in variants}
        for f in cf.as_completed(futs):
            ok, diag = f.result()
            if not ok:
                failures.append((futs[f], diag))
    failures.sort(key=lambda t: t[0]["key"])
    return failures


# ---------------------------------------------------------------------------
# Regression proof
# ---------------------------------------------------------------------------
def regression_target(variants: list) -> tuple[str, list]:
    """Use a fragment the current C++ actually loads; retirement cannot orphan the probe."""
    fragment = next((v["stages"]["frag"] for v in variants if "frag" in v["stages"]), None)
    if fragment is None:
        raise ValueError("the live GL matrix has no fragment for its negative compile probe")
    return fragment, [v for v in variants if fragment in v["stages"].values()]


def selftest(glslang: str, variants: list, jobs: int) -> list[str]:
    """Control must compile; an unterminated directive must fail every selected variant."""
    try:
        fragment, targets = regression_target(variants)
    except ValueError as error:
        return [str(error)]
    with tempfile.TemporaryDirectory(prefix="glvariants_") as td:
        t = Path(td); mirror = t / "shaders"
        shutil.copytree(ROOT / "shaders", mirror,
                        ignore=shutil.ignore_patterns("vulkan", "spv", "fixtures"))
        control = run_matrix(glslang, mirror, targets, jobs, t / "work")
        if control:
            return ["control run (live fragment) already fails: " + control[0][0]["key"]]
        mutated = mirror / fragment.removeprefix("shaders/")
        with mutated.open("a", encoding="utf-8", newline="\n") as output:
            output.write("\n#ifdef MC2_UNTERMINATED_MATRIX_PROBE\n")
        bad = run_matrix(glslang, mirror, targets, jobs, t / "work2")
        failed = {variant["key"] for variant, _ in bad}
        missed = [variant["key"] for variant in targets if variant["key"] not in failed]
        if missed:
            return ["malformed live fragment NOT caught for: " + ", ".join(missed[:4])]
        if not any(Path(fragment).name in " ".join(diag) or "#ifdef" in " ".join(diag)
                   for _, diag in bad):
            return ["negative compile failed without pointing at the live fragment/directive"]
    return []


# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--jobs", type=int, default=min(16, (os.cpu_count() or 4)))
    ap.add_argument("--only", help="only variants whose key contains this substring")
    ap.add_argument("--list", action="store_true", help="list variants and exit")
    ap.add_argument("--no-selftest", action="store_true")
    ap.add_argument("--shaders-dir", help="shaders/ directory to validate (default: repo)")
    a = ap.parse_args(argv)
    t0 = time.time()

    # 1. freshness ----------------------------------------------------------
    try:
        fresh = extractor.extract()
    except extractor.ExtractError as e:
        print(f"check-gl-shader-variants: FAIL extraction gap: {e}", file=sys.stderr)
        return 1
    cur = MANIFEST.read_text(encoding="utf-8") if MANIFEST.exists() else ""
    if cur != extractor.dumps(fresh):
        print("check-gl-shader-variants: FAIL scripts/gl_shader_variants.json is STALE vs the C++.\n"
              "  A makeProgram/prefix/compute-loader changed. Regenerate and commit:\n"
              "      py -3 scripts/extract_gl_shader_variants.py\n"
              "  (diff: py -3 scripts/extract_gl_shader_variants.py --check)", file=sys.stderr)
        return 1
    variants = json.loads(cur)["variants"]
    if a.list:
        for v in variants:
            print(v["key"], "|", " ".join(v["stages"].values()))
        return 0
    if a.only:
        variants = [v for v in variants if a.only in v["key"]]

    glslang = find_glslang()
    if not glslang:
        print("check-gl-shader-variants: FAIL glslangValidator not found "
              "($VULKAN_SDK/Bin, PATH or C:/VulkanSDK/*/Bin).", file=sys.stderr)
        return 1

    # 2. matrix -------------------------------------------------------------
    shaders_dir = Path(a.shaders_dir).resolve() if a.shaders_dir else shader_common.SHADERS
    with tempfile.TemporaryDirectory(prefix="glvariants_") as td:
        failures = run_matrix(glslang, shaders_dir, variants, a.jobs, Path(td))
    for v, diag in failures:
        print(f"FAIL {v['key']}  [{v['source']}]")
        for d in diag[:6]:
            print(f"    {d}")
    t_matrix = time.time() - t0

    # coverage note: shaders on disk that no GL program references
    used = {r for v in variants for r in v["stages"].values()}
    if not a.only and not a.quiet:
        disk = {f"shaders/{p.name}" for ext in shader_common.STAGE_BY_EXT
                for p in shaders_dir.glob(f"*{ext}")}
        orphan = sorted(disk - used)
        if orphan:
            print("note: not reachable from any GL program in the manifest: " +
                  ", ".join(Path(o).name for o in orphan))

    # 3. regression proof ---------------------------------------------------
    st_problems = []
    if not a.no_selftest and not a.only and not a.shaders_dir:
        st_problems = selftest(glslang, variants, a.jobs)
        for p in st_problems:
            print(f"FAIL selftest: {p}")

    print(f"check-gl-shader-variants: {len(variants) - len(failures)}/{len(variants)} variants "
          f"compile+link OK, {len(failures)} failed, selftest "
          f"{'skipped' if (a.no_selftest or a.only or a.shaders_dir) else ('FAIL' if st_problems else 'ok')} "
          f"(matrix {t_matrix:.1f}s, total {time.time() - t0:.1f}s, jobs={a.jobs})")
    return 1 if (failures or st_problems) else 0


if __name__ == "__main__":
    sys.exit(main())
