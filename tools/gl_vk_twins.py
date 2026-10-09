#!/usr/bin/env python3
"""gl_vk_twins.py -- the GL <-> VK shader twin table and shader-side feature scan.

WHY: every GL shader that has a Vulkan re-implementation is a place where a GL-only feature
can be silently dropped. The coverage tool needs (1) which VK files re-implement which GL
files and (2) which uniforms / compile-time toggles each GL shader consumes.

Twins are derived from, in order of provenance:
  same-name  shaders/<stem>.<stage> exists on both sides
  suffix     VK stem minus _tier1 / _native equals a GL stem
  map        tools/gl_vk_twin_map.json (hand-curated, each entry cites the VK header)
  registry   tools/glsl_port_diff.py REGISTRY ports
  split      scripts/check-split-path-contracts.py PAIRS that straddle the two trees
A VK shader that fits none of these must be listed in the map's no_gl_twin table, so a new
VK shader cannot land unclassified.
"""
from __future__ import annotations

import importlib.util
import json
import os
import re

from gl_vk_source import ROOT, blank_comments

GL_EXTS = (".vert", ".frag", ".tesc", ".tese", ".geom", ".comp")
TWIN_MAP = "tools/gl_vk_twin_map.json"
SUFFIXES = ("_tier1", "_native")

_INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*"([^"]+)"', re.M)


def split_stem(path):
    base = os.path.basename(path)
    stem, ext = os.path.splitext(base)
    return stem, ext


def gl_shaders(src):
    """Top-level GL shader files (no include/, fixtures/, spv/, vulkan/ subtrees)."""
    return [p for p in src.glob("shaders/", GL_EXTS) if p.count("/") == 1]


def vk_shaders(src):
    return [p for p in src.glob("shaders/vulkan/", GL_EXTS) if p.count("/") == 2]


def _load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def external_pairs(root=ROOT):
    """(gl_path, vk_path, via) pairs already known to glsl_port_diff / split-path checks."""
    out = []
    pd = _load_module(os.path.join(root, "tools", "glsl_port_diff.py"), "_glsl_port_diff_reg")
    for port in pd.REGISTRY:
        if port.gl.startswith("shaders/") and port.vk.startswith("shaders/vulkan/"):
            out.append((port.gl, port.vk, "registry"))
    sp = _load_module(os.path.join(root, "scripts", "check-split-path-contracts.py"), "_split_path_reg")
    for members in sp.PAIRS.values():
        vks = [m for m in members if m.startswith("shaders/vulkan/")]
        gls = [m for m in members if m.startswith("shaders/") and not m.startswith("shaders/vulkan/")]
        for g in gls:
            for v in vks:
                out.append((g, v, "split"))
    return out


class Twins:
    """Result of twin derivation."""

    def __init__(self):
        self.gl_files = []
        self.vk_files = []
        self.family = {}        # vk stem -> {gl stem: set(provenance)}
        self.no_twin_vk = {}    # vk file -> reason
        self.unclassified_vk = []
        self.file_pairs = []    # (gl file, vk file, provenance-string)
        self.gl_unpaired = []   # GL files with no VK family at all
        self.vk_gl_stems = {}   # vk stem -> sorted gl stems

    def vk_family_for_gl(self, gl_path):
        """VK stems whose family contains this GL file's stem."""
        stem, _ = split_stem(gl_path)
        return sorted(v for v, g in self.family.items() if stem in g)


def derive(src, ext_pairs=None, twin_map=None):
    tw = Twins()
    tw.gl_files = sorted(gl_shaders(src))
    tw.vk_files = sorted(vk_shaders(src))
    gl_stems = {split_stem(p)[0] for p in tw.gl_files}
    vk_stems = {split_stem(p)[0] for p in tw.vk_files}
    if twin_map is None:
        with open(os.path.join(src.root, TWIN_MAP), "r", encoding="utf-8") as f:
            twin_map = json.load(f)
    if ext_pairs is None:
        ext_pairs = external_pairs(src.root)

    def add(vs, gs, via):
        tw.family.setdefault(vs, {}).setdefault(gs, set()).add(via)

    for vs in sorted(vk_stems):
        if vs in gl_stems:
            add(vs, vs, "same-name")
        for suf in SUFFIXES:
            if vs.endswith(suf) and vs[:-len(suf)] in gl_stems:
                add(vs, vs[:-len(suf)], "suffix")
    for vs, ent in sorted(twin_map.get("families", {}).items()):
        if vs not in vk_stems:
            raise ValueError("%s: family '%s' names no shaders/vulkan/%s.*" % (TWIN_MAP, vs, vs))
        for gs in ent["gl"]:
            if gs not in gl_stems:
                raise ValueError("%s: family '%s' names GL stem '%s' with no shaders/%s.*"
                                 % (TWIN_MAP, vs, gs, gs))
            add(vs, gs, "map")
    for g, v, via in ext_pairs:
        gs, vs = split_stem(g)[0], split_stem(v)[0]
        if vs in vk_stems and gs in gl_stems:
            add(vs, gs, via)

    no_gl = twin_map.get("no_gl_twin", {})
    for v in tw.vk_files:
        vs = split_stem(v)[0]
        if vs in tw.family:
            continue
        if vs in no_gl:
            tw.no_twin_vk[v] = no_gl[vs]
        else:
            tw.unclassified_vk.append(v)
    for vs in no_gl:
        if vs in tw.family:
            raise ValueError("%s: '%s' is listed no_gl_twin but also has a GL family" % (TWIN_MAP, vs))

    vk_by_key = {(split_stem(p)[0], split_stem(p)[1]): p for p in tw.vk_files}
    for g in tw.gl_files:
        gs, ge = split_stem(g)
        fams = tw.vk_family_for_gl(g)
        if not fams:
            tw.gl_unpaired.append(g)
            continue
        for vs in fams:
            vf = vk_by_key.get((vs, ge))
            via = "+".join(sorted(tw.family[vs][gs]))
            tw.file_pairs.append((g, vf or "(family %s: no %s stage)" % (vs, ge), via))
    tw.file_pairs.sort()
    tw.vk_gl_stems = {vs: sorted(g) for vs, g in tw.family.items()}
    return tw


# --- shader-side feature scan ------------------------------------------------

def expand_includes(src, path, seen=None, files=None, keep_comments=False):
    """Comment-blanked text of `path` with #include "x" bodies appended (order-insensitive:
    only token presence matters to the coverage scan). Returns (text, [files]).
    keep_comments leaves the comments in (the packed-slot scan reads them)."""
    seen = set() if seen is None else seen
    files = [] if files is None else files
    if path in seen:
        return "", files
    seen.add(path)
    raw = src.text(path)
    if raw is None:
        return "", files
    files.append(path)
    blanked = blank_comments(raw)
    parts = [raw if keep_comments else blanked]
    for inc in _INCLUDE_RE.findall(blanked):
        cands = [os.path.normpath(os.path.join(os.path.dirname(path), inc)).replace("\\", "/"),
                 "shaders/include/" + inc.split("/")[-1],
                 "shaders/" + inc]
        for c in cands:
            if src.exists(c):
                sub, _ = expand_includes(src, c, seen, files, keep_comments)
                parts.append(sub)
                break
    return "\n".join(parts), files


_UNIFORM_RE = re.compile(
    r"^[ \t]*(?:layout[ \t]*\([^)]*\)[ \t]*)?uniform[ \t]+(?:(?:highp|mediump|lowp|flat)[ \t]+)?"
    r"([A-Za-z_]\w*)[ \t]+([^;{]+);", re.M)
_DECL_NAME_RE = re.compile(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^,]*)?(?:,|$)")
_COND_RE = re.compile(r"^[ \t]*#[ \t]*(?:if|ifdef|ifndef|elif)\b([^\n]*)", re.M)
_DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", re.M)
_MACRO_TOKEN_RE = re.compile(r"\b([A-Z][A-Z0-9_]{3,})\b")
_NOT_MACROS = {"GL_ES", "GL_EXT"}


def uniforms_of(text):
    """Non-sampler default-block uniform names declared in (comment-blanked) shader text."""
    out = set()
    for typ, rest in _UNIFORM_RE.findall(text):
        if typ.endswith(("sampler2D", "sampler3D", "samplerCube", "sampler2DArray", "sampler2DShadow",
                         "sampler2DArrayShadow", "sampler2DRect", "samplerBuffer")) or "sampler" in typ \
                or "image" in typ:
            continue
        for m in _DECL_NAME_RE.finditer(rest.strip() + ","):
            out.add(m.group(1))
    return out


def toggles_of(text):
    """Macro names the shader branches on that the shader does not itself #define."""
    defined = set(_DEFINE_RE.findall(text))
    out = set()
    for cond in _COND_RE.findall(text):
        for tok in _MACRO_TOKEN_RE.findall(cond):
            if tok not in defined and not tok.startswith("GL_") and tok not in _NOT_MACROS:
                out.add(tok)
    return out
