"""scripts/shader_common.py — shared shader source construction.

Shared by tools/check_vk_shaders.py and tools/shader_reflect/reflect.py
(validate_shaders.py, the original owner, was retired 2026-10-12, cull wave 1) so both gates see
the exact same source string the engine compiles.

Do NOT import from this module outside scripts/ and tools/shader_reflect/.
"""
from __future__ import annotations

import os
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "shaders"
INCLUDE = SHADERS / "include"

VERSION_PREFIX = "#version 430\n"
INCLUDE_EXTENSION = "#extension GL_GOOGLE_include_directive : require\n"
_INCLUDE_RE = re.compile(r"(?m)^\s*#\s*include\b")
ENGINE_PATH_SEPARATOR = "/"

STAGE_BY_EXT: dict[str, str] = {
    ".vert": "vert",
    ".frag": "frag",
    ".tesc": "tesc",
    ".tese": "tese",
    ".geom": "geom",
    ".comp": "comp",
}

SKIP_SHADERS: dict[str, str] = {
    "gos_terrain_lighting.comp": (
        "programmatically stitched at runtime via "
        "tl_build_terrain_lighting_program; standalone validation cannot "
        "reach this shader without replicating the engine's include stitching"
    ),
}

SHADER_PREFIX_OVERRIDE: dict[str, str] = {
    "gos_terrain_water_fast_mdi.vert": "#version 460\n",
}

SHADER_TOKEN_REWRITES: dict[str, list[tuple[str, str]]] = {
    "gos_terrain_water_fast_mdi.vert": [
        ("gl_DrawIDARB", "gl_DrawID"),
    ],
}


def _engine_get_path(fname: str) -> str:
    idx = max(fname.rfind("/"), fname.rfind("\\"))
    if idx < 0:
        return ""
    return fname[:idx]


def _engine_find_next_include_directive(p: str, start: int) -> int:
    INCLUDE = "#include"
    i = start
    n = len(p)
    while i < n:
        c = p[i]
        nxt = p[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and p[i] != "\n":
                i += 1
        elif c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (p[i] == "*" and p[i + 1] == "/"):
                i += 1
            if i < n:
                i += 2
        elif c == '"' or c == "'":
            q = c
            i += 1
            while i < n and p[i] != q:
                if p[i] == "\\" and i + 1 < n:
                    i += 2
                else:
                    i += 1
            if i < n:
                i += 1
        elif c == "#" and p.startswith(INCLUDE, i):
            return i
        else:
            i += 1
    return -1


def _engine_parse_include(s: str, start: int):
    n = len(s)
    begin = s.find("<", start)
    end = s.find(">", start)
    eol = s.find("\n", start)
    if begin < 0 or end < 0:
        return None
    if eol >= 0 and end > eol:
        return None
    if end - begin <= 1:
        return None
    begin += 1
    while begin < end and s[begin] == " ":
        begin += 1
    while end > begin and s[end - 1] == " ":
        end -= 1
    name = s[begin:end]
    ieol = eol + 1 if eol >= 0 else n
    return name, ieol


def _engine_get_num_lines(text: str) -> int:
    if not text:
        return 0
    return 1 + text.count("\n")


def parse_includes_engine_style(source_text: str, source_path: str,
                                _visited: set | None = None) -> str:
    """Port of GameOS/gameos/utils/shader_builder.cpp:262 parse_includes."""
    if _visited is None:
        _visited = set()
    norm_self = os.path.normcase(os.path.normpath(source_path))
    if norm_self in _visited:
        raise ValueError(f"include cycle detected at {source_path}")
    _visited = _visited | {norm_self}

    base_path = _engine_get_path(source_path)
    out_parts: list[str] = []
    current_line = 1
    start = 0
    n = len(source_text)
    INCLUDE_LEN = len("#include")

    while True:
        tok = _engine_find_next_include_directive(source_text, start)
        if tok < 0:
            break
        code = source_text[start:tok]
        out_parts.append(f"#line {current_line} // {source_path}\n")
        out_parts.append(code)
        current_line += _engine_get_num_lines(code)
        parsed = _engine_parse_include(source_text, tok + INCLUDE_LEN)
        if parsed is None:
            raise ValueError(
                f"malformed #include in {source_path} near offset {tok}"
            )
        inc_name, ieol = parsed
        if base_path:
            include_path = base_path + ENGINE_PATH_SEPARATOR + inc_name
        else:
            include_path = inc_name
        try:
            included_src = Path(include_path).read_text(
                encoding="utf-8", errors="replace"
            )
        except OSError as e:
            raise ValueError(
                f"cannot open include {include_path} (from {source_path}): {e}"
            )
        inlined = parse_includes_engine_style(
            included_src, include_path, _visited=_visited
        )
        out_parts.append(inlined)
        if ieol >= n:
            start = n
            break
        start = ieol

    if start < n:
        out_parts.append(f"#line {current_line} // {source_path}\n")
        out_parts.append(source_text[start:])

    return "".join(out_parts)


def find_tool(name: str) -> str | None:
    """Locate a Vulkan SDK tool. Prefer $VULKAN_SDK/Bin, then PATH."""
    sdk = os.environ.get("VULKAN_SDK")
    if sdk:
        candidate = Path(sdk) / "Bin" / (
            name + (".exe" if os.name == "nt" else "")
        )
        if candidate.exists():
            return str(candidate)
    return shutil.which(name)


def discover_shaders() -> list[Path]:
    """Return all shader files under shaders/, sorted by extension then name.
    Recursive: includes shaders/fixtures/ and any future subdirectories.
    Convention: any file under shaders/**/*.frag|vert|... is part of shader
    validation unless explicitly listed in SKIP_SHADERS.
    """
    out: list[Path] = []
    for ext in STAGE_BY_EXT:
        out.extend(sorted(SHADERS.glob(f"**/*{ext}")))
    return out


def build_shader_source(
    shader_path: Path,
    extra_defines: list[str] | None = None,
    version_override: str | None = None,
    extra_token_rewrites: list[tuple[str, str]] | None = None,
) -> str:
    """Assemble full shader source as the engine would compile it.

    Reads shader_path, expands #include directives using engine semantics
    (parse_includes_engine_style), prepends the version prefix (or per-shader
    override), injects extra_defines, and applies per-shader token rewrites.

    extra_defines: list of "NAME=VALUE" or "NAME" strings. Each becomes a
    #define line injected between the version prefix and the body, matching
    the engine's runtime prefix-string composition for variant programs.

    version_override: if provided, replaces both SHADER_PREFIX_OVERRIDE and the
    default VERSION_PREFIX for this invocation. Used by reflect.py to use
    #version 460 for coalesce variants that rely on gl_BaseInstanceARB /
    gl_DrawIDARB — glslangValidator requires version 460 (core) rather than
    430 + GL_ARB_shader_draw_parameters for these builtins.

    extra_token_rewrites: additional (old, new) string replacements applied
    after the per-shader SHADER_TOKEN_REWRITES. Used by reflect.py to rewrite
    ARB-suffixed builtins (e.g. gl_BaseInstanceARB → gl_BaseInstance) to their
    GL 4.6 core names when compiling under #version 460.

    Raises OSError if the file cannot be read.
    Raises ValueError if an #include cannot be resolved or forms a cycle.
    """
    if extra_defines is None:
        extra_defines = []
    if extra_token_rewrites is None:
        extra_token_rewrites = []

    src_text = shader_path.read_text(encoding="utf-8", errors="replace")
    flat_text = parse_includes_engine_style(src_text, str(shader_path))

    if version_override is not None:
        prefix = version_override
    else:
        prefix = SHADER_PREFIX_OVERRIDE.get(shader_path.name, VERSION_PREFIX)
    parts: list[str] = [prefix]

    if _INCLUDE_RE.search(flat_text):
        parts.append(INCLUDE_EXTENSION)

    for d in extra_defines:
        if "=" in d:
            name, val = d.split("=", 1)
            parts.append(f"#define {name} {val}\n")
        else:
            parts.append(f"#define {d}\n")

    parts.append(flat_text)
    src = "".join(parts)

    for old_tok, new_tok in SHADER_TOKEN_REWRITES.get(shader_path.name, ()):
        src = src.replace(old_tok, new_tok)
    for old_tok, new_tok in extra_token_rewrites:
        src = src.replace(old_tok, new_tok)

    return src
