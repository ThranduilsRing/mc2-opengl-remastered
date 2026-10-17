"""slang_compile.py - Slang (.slang) -> Vulkan SPIR-V for build_variants.py.

Targets the same SPIR-V as the GLSL Vulkan path (glslangValidator -V default =
Vulkan 1.0 / SPIR-V 1.0): `-profile spirv_1_0`. Override per pilot with the
`spirv` key in the pilots manifest if a shader needs a newer profile.
See docs/shaders-slang.md.
"""
from __future__ import annotations
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import shader_common  # noqa: E402
import console_child  # noqa: E402

SLANG_STAGE = {"vert": "vertex", "frag": "fragment", "comp": "compute"}
DEFAULT_PROFILE = "spirv_1_0"
# 50011 = "SPIR-V version too old" (1.0 is deliberate: matches the GLSL path).
DISABLED_WARNINGS = "50011"


def is_slang(rel) -> bool:
    return str(rel).endswith(".slang")


def min_version() -> str:
    for line in (ROOT / "tools" / "slang" / "SLANG_VERSION").read_text().splitlines():
        if line.startswith("MIN_VERSION="):
            return line.split("=", 1)[1].strip()
    raise RuntimeError("tools/slang/SLANG_VERSION: MIN_VERSION missing")


def _vtuple(v: str):
    return tuple(int(x) for x in re.findall(r"\d+", v)[:3])


def find_slangc():
    return shader_common.find_tool("slangc")


def slangc_version(exe) -> str:
    r = subprocess.run([exe, "-v"], capture_output=True, text=True,
                       **console_child.child_kwargs(redirected=True))
    out = (r.stdout + r.stderr).strip().splitlines()
    return out[0].strip() if out else "unknown"


def check_version(exe):
    """Return an error string if slangc is older than MIN_VERSION, else None."""
    v = slangc_version(exe)
    if _vtuple(v) < _vtuple(min_version()):
        return f"slangc {v} older than pinned minimum {min_version()}"
    return None


def source_text_canon(path: Path) -> str:
    return path.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")


def compile_slang(exe, src: Path, stage: str, out: Path, entry="main",
                  profile=DEFAULT_PROFILE):
    """Return (ok, diagnostics). Never leaves a partial `out` behind on failure."""
    if stage not in SLANG_STAGE:
        return False, f"unsupported stage '{stage}' (vert|frag|comp)"
    cmd = [exe, str(src), "-target", "spirv", "-profile", profile,
           "-stage", SLANG_STAGE[stage], "-entry", entry,
           "-warnings-disable", DISABLED_WARNINGS, "-o", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True,
                       **console_child.child_kwargs(redirected=True))
    ok = r.returncode == 0 and out.exists()
    if not ok and out.exists():
        out.unlink()
    return ok, (r.stdout + r.stderr).strip()
