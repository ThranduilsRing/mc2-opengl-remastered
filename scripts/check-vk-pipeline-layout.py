#!/usr/bin/env python3
"""Offline Vulkan pipeline-layout lint (vertex input / descriptors / push constants).

Catches CPU-vs-shader interface mismatches that only show up as invisible or
exploded geometry at runtime, e.g. the shadow island pipeline baked with vertex
stride 12 while the shared static-prop VBO stride is 40 (fixed in c26b1eb2a via
mc2_static_prop_vertex_stride()).

Pipeline:
  1. Compile shaders/vulkan/*.{vert,frag} with glslc (SDK) into a temp dir and
     reflect them with spirv-cross --reflect (inputs, ubos/ssbos/images, push).
  2. Scrape GameOS/gameos/vulkan_*.cpp for, per pipeline:
        VkVertexInputBindingDescription / VkVertexInputAttributeDescription,
        VkDescriptorSetLayoutBinding, VkPushConstantRange,
     and the "<name>.vert.spv"/"<name>.frag.spv" loaded for it.
     Stride/size expressions are resolved to numbers via constants,
     static_assert(sizeof(T)==N), simple POD structs and extern "C" accessors.
  3. Compare. Findings are `file:line: ERROR|WARN: message`.

Rules:
  VK-STRIDE-FALLBACK   stride is `x ? x : fallback` (runtime value with a fallback;
                       pipelines are baked before geometry upload) -> ERROR
  VK-STRIDE-ZERO       stride resolves to 0
  VK-STRIDE-VBO        stride != the shared VBO contract for that shader
  VK-ATTR-OOB          attribute offset+size exceeds stride
  VK-INPUT-MISSING     shader vertex input location has no C++ attribute
  VK-INPUT-TYPE        attribute format class (float/int/uint) != shader type
  VK-INPUT-WIDTH       attribute has fewer components than shader input (WARN)
  VK-ATTR-UNUSED       attribute location not consumed by shader (WARN)
  VK-DESC-MISSING / VK-DESC-TYPE / VK-DESC-STAGE
  VK-PUSH-MISSING / VK-PUSH-SIZE / VK-PUSH-STAGE
  VK-UNRESOLVED        expression could not be resolved (WARN)

Suppress a line with a trailing `// vk-lint: ok` comment on the stride/format line.

Exit 0 = no errors, 1 = errors, 2 = tooling missing.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import os as _os, sys as _sys  # noqa: E401
_sys.path.append(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "scripts"))
import console_child  # noqa: E402  (windowless console children without a conhost each; scripts/console_child.py)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK_DEFAULT = r"C:\VulkanSDK\1.4.350.0"

# Shaders whose vertex input is fed by a shared VBO with a fixed stride owned by
# an accessor in the batcher. pipeline stride MUST equal that accessor's value.
VBO_CONTRACT = {
    "shadow_prop.vert": "mc2_static_prop_vertex_stride",
    "color_prop_tier1.vert": "mc2_static_prop_vertex_stride",
    "mech_tier1.vert": "mc2_mech_vertex_stride",
}

# Baked SPIR-V names that have no source file of their own: GameOS/gameos/CMakeLists.txt
# (MC2_VK_SHADER_SRC_ALIAS_*) bakes them from another shader, so they reflect identically.
SPV_ALIASES = {
    "terrain_tier1_skirt.frag": "terrain_tier1.frag",
}

# ---------------------------------------------------------------------------
# VkFormat tables: name -> (class, components, bytes)
# ---------------------------------------------------------------------------
_FMT = {}


def _add(names, cls, comps, bytes_):
    for n in names:
        _FMT["VK_FORMAT_" + n] = (cls, comps, bytes_)


for bits, by in ((8, 1), (16, 2), (32, 4), (64, 8)):
    for comps, sfx in ((1, "R"), (2, "RG"), (3, "RGB"), (4, "RGBA")):
        tag = sfx.replace("", "").join([""]) or sfx
        base = "".join(f"{c}{bits}" for c in sfx)
        if bits == 8:
            _add([base + "_UNORM", base + "_SNORM", base + "_USCALED", base + "_SSCALED"], "float", comps, by * comps)
            _add([base + "_UINT"], "uint", comps, by * comps)
            _add([base + "_SINT"], "int", comps, by * comps)
        elif bits == 16:
            _add([base + "_UNORM", base + "_SNORM", base + "_USCALED", base + "_SSCALED", base + "_SFLOAT"], "float", comps, by * comps)
            _add([base + "_UINT"], "uint", comps, by * comps)
            _add([base + "_SINT"], "int", comps, by * comps)
        elif bits == 32:
            _add([base + "_SFLOAT"], "float", comps, by * comps)
            _add([base + "_UINT"], "uint", comps, by * comps)
            _add([base + "_SINT"], "int", comps, by * comps)
        else:
            _add([base + "_SFLOAT"], "float", comps, by * comps)
_add(["B8G8R8A8_UNORM", "B8G8R8A8_SNORM"], "float", 4, 4)
_add(["B8G8R8A8_UINT"], "uint", 4, 4)
_add(["A2B10G10R10_UNORM_PACK32", "A2B10G10R10_SNORM_PACK32", "A2R10G10B10_UNORM_PACK32"], "float", 4, 4)
_add(["A2B10G10R10_UINT_PACK32"], "uint", 4, 4)
_add(["A8B8G8R8_UNORM_PACK32"], "float", 4, 4)
_add(["B10G11R11_UFLOAT_PACK32"], "float", 3, 4)

SCALAR_BYTES = {
    "float": 4, "double": 8, "int": 4, "unsigned": 4, "uint": 4, "uint32_t": 4, "int32_t": 4,
    "int16_t": 2, "uint16_t": 2, "short": 2, "int8_t": 1, "uint8_t": 1, "char": 1,
    "uint64_t": 8, "int64_t": 8, "size_t": 8, "bool": 1, "unsigned char": 1,
    "unsigned short": 2, "unsigned int": 4, "BYTE": 1, "WORD": 2, "DWORD": 4, "uint32": 4,
}

DESC_COMPAT = {
    "ubo": {"VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER", "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC"},
    "ssbo": {"VK_DESCRIPTOR_TYPE_STORAGE_BUFFER", "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC"},
    "sampled_image": {"VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER"},
    "separate_image": {"VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE"},
    "separate_sampler": {"VK_DESCRIPTOR_TYPE_SAMPLER"},
    "storage_image": {"VK_DESCRIPTOR_TYPE_STORAGE_IMAGE"},
    "subpass_input": {"VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT"},
}
REFLECT_KEYS = {
    "ubos": "ubo", "ssbos": "ssbo", "images": "storage_image", "textures": "sampled_image",
    "separate_images": "separate_image", "separate_samplers": "separate_sampler",
    "storage_images": "storage_image", "subpass_inputs": "subpass_input",
}
STAGE_BIT = {"vert": "VK_SHADER_STAGE_VERTEX_BIT", "frag": "VK_SHADER_STAGE_FRAGMENT_BIT"}


class Finding:
    def __init__(self, sev, rule, path, line, msg):
        self.sev, self.rule, self.path, self.line, self.msg = sev, rule, path, line, msg

    def __str__(self):
        try:
            rel = os.path.relpath(self.path, ROOT).replace("\\", "/")
        except ValueError:
            rel = self.path
        return f"{rel}:{self.line}: {self.sev}: [{self.rule}] {self.msg}"


# ---------------------------------------------------------------------------
# Source helpers
# ---------------------------------------------------------------------------
def strip_comments(text):
    """Blank out // and /* */ comments (keeping newlines + string literals)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1]); i = j + 1
        elif c == "'" and i + 2 < n:
            j = text.find("'", i + 1 + (1 if text[i + 1] == "\\" else 0) + 1)
            j = j if j != -1 else i
            out.append(text[i:j + 1]); i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j == -1 else j
            out.append(" " * (j - i)); i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j == -1 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def lineno(text, pos):
    return text.count("\n", 0, pos) + 1


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


class Symbols:
    """Constants, sizeof tables and accessor functions scraped from the tree."""

    def __init__(self):
        self.consts = {}     # name -> [(file, expr)]
        self.sizeof = {}     # type -> int
        self.structs = {}    # name -> body text
        self.funcs = {}      # name -> [(file, expr)]

    def scan(self, path, clean):
        for m in re.finditer(r"(?:constexpr|const)\s+(?:static\s+)?[\w:<> ]+?\s+(\w+)\s*=\s*([^;{}]+);", clean):
            self.consts.setdefault(m.group(1), []).append((path, m.group(2).strip()))
        for m in re.finditer(r"enum\s*\{([^}]*)\}", clean):
            for e in re.finditer(r"(\w+)\s*=\s*([^,}]+)", m.group(1)):
                self.consts.setdefault(e.group(1), []).append((path, e.group(2).strip()))
        for m in re.finditer(r"static_assert\s*\(\s*sizeof\s*\(\s*([\w:]+)\s*\)\s*==\s*(\d+)", clean):
            self.sizeof[m.group(1).split("::")[-1]] = int(m.group(2))
        for m in re.finditer(r"struct\s+(\w+)\s*\{([^{}]*)\}\s*;", clean):
            self.structs.setdefault(m.group(1), m.group(2))
        for m in re.finditer(r"\b(\w+)\s*\(\s*(?:void)?\s*\)\s*(?:const\s*)?\{\s*return\s+([^;]+);\s*\}", clean):
            self.funcs.setdefault(m.group(1), []).append((path, m.group(2).strip()))

    def struct_size(self, name, depth=0):
        if name in self.sizeof:
            return self.sizeof[name]
        body = self.structs.get(name)
        if body is None or depth > 4:
            return None
        off, maxal = 0, 1
        for stmt in body.split(";"):
            stmt = stmt.strip()
            if not stmt or "(" in stmt or stmt.startswith(("static", "using", "typedef")):
                continue
            mm = re.match(r"([\w: ]+?)\s+(\w[\w\s,\[\]]*)$", stmt)
            if not mm:
                return None
            ty = mm.group(1).strip().replace("const ", "")
            sz = SCALAR_BYTES.get(ty)
            if sz is None:
                sz = self.struct_size(ty, depth + 1)
            if sz is None:
                return None
            for decl in mm.group(2).split(","):
                am = re.match(r"\s*\w+\s*(?:\[(\d+)\])?\s*$", decl)
                if not am:
                    return None
                cnt = int(am.group(1)) if am.group(1) else 1
                al = min(sz, 8)
                off = (off + al - 1) // al * al + sz * cnt
                maxal = max(maxal, al)
        return (off + maxal - 1) // maxal * maxal

    @staticmethod
    def unit_stem(path):
        """gos_static_prop_batcher_shadow.cpp and gos_static_prop_batcher_internal.h are one translation unit's source:
        a split piece and the shared header of its stem count as the same file for constant lookup."""
        base = os.path.splitext(os.path.basename(path))[0]
        d = os.path.dirname(path)
        if base.endswith("_internal"):
            return os.path.join(d, base[:-len("_internal")])
        cut = base.rsplit("_", 1)[0]
        if cut != base and os.path.exists(os.path.join(d, cut + "_internal.h")):
            return os.path.join(d, cut)
        return os.path.join(d, base)

    def lookup(self, table, name, cur_file):
        ents = table.get(name)
        if not ents:
            return None
        same = [e for e in ents if e[0] == cur_file]
        if not same:
            cs = self.unit_stem(cur_file)
            same = [e for e in ents if self.unit_stem(e[0]) == cs]
        if same:
            return same[-1]
        vals = {e[1] for e in ents}
        return ents[0] if len(vals) == 1 else None

    def eval(self, expr, cur_file, depth=0):
        """Return int or None."""
        if depth > 6:
            return None
        e = expr.strip()
        e = re.sub(r"static_cast<[^>]+>\s*\(", "(", e)
        e = re.sub(r"\(\s*(?:u?int\d*_t|size_t|unsigned|int|uint32_t|VkDeviceSize|uint32)\s*\)", "", e)
        e = re.sub(r"\bsizeof\s*\(\s*([\w: ]+?)\s*\)", lambda m: self._sizeof_repl(m.group(1), cur_file), e)
        if "None" in e:
            return None
        e = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", e)

        def ident(m):
            name = m.group(0).split("::")[-1]
            if re.fullmatch(r"0[xX][0-9a-fA-F]+|\d+", name):
                return name
            f = self.lookup(self.funcs, name + "()", cur_file)
            ent = self.lookup(self.consts, name, cur_file)
            if ent is None:
                return "None"
            v = self.eval(ent[1], ent[0], depth + 1)
            return "None" if v is None else str(v)

        # accessor calls: name() -> value
        def call(m):
            ent = self.lookup(self.funcs, m.group(1), cur_file)
            if ent is None:
                return "None"
            v = self.eval(ent[1], ent[0], depth + 1)
            return "None" if v is None else str(v)

        e = re.sub(r"\b(\w+)\s*\(\s*\)", call, e)
        e = re.sub(r"\b[A-Za-z_][\w:]*\b", ident, e)
        if "None" in e or not re.fullmatch(r"[\d\s+\-*/()%<>|&xXa-fA-F]*", e):
            return None
        try:
            # SAFE: e is charset-restricted above to digits/operators/parens; no names, no builtins.
            v = eval(e, {"__builtins__": {}}, {})
            return int(v)
        except Exception:
            return None

    def _sizeof_repl(self, ty, cur_file):
        ty = ty.strip().replace("const ", "")
        if ty in SCALAR_BYTES:
            return str(SCALAR_BYTES[ty])
        sz = self.struct_size(ty.split("::")[-1])
        return "None" if sz is None else str(sz)


# ---------------------------------------------------------------------------
# Shader compile + reflect
# ---------------------------------------------------------------------------
def find_tool(sdk, name):
    for d in (os.path.join(sdk, "Bin"), os.path.join(sdk, "bin")):
        p = os.path.join(d, name + (".exe" if os.name == "nt" else ""))
        if os.path.exists(p):
            return p
    return shutil.which(name)


def type_size(types, tname, base_scalar):
    """size in bytes for a spirv-cross reflect type name; None if unknown."""
    m = re.fullmatch(r"([iu]?)(?:vec(\d)|(float|int|uint))", tname)
    if m:
        n = int(m.group(2)) if m.group(2) else 1
        return 4 * n
    if tname in ("mat4",):
        return 64
    if tname in ("mat3",):
        return 48
    return None


def reflect_shader(glslc, cross, src, outdir, incdir):
    base = os.path.basename(src)
    spv = os.path.join(outdir, base + ".spv")
    r = subprocess.run([glslc, "-I", incdir, src, "-o", spv], capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    if r.returncode != 0:
        return base, None, "glslc failed: " + r.stderr.strip().splitlines()[0] if r.stderr.strip() else "glslc failed"
    r = subprocess.run([cross, spv, "--reflect"], capture_output=True, text=True)
    if r.returncode != 0:
        return base, None, "spirv-cross failed"
    d = json.loads(r.stdout)
    info = {"inputs": {}, "bindings": {}, "push": None, "stage": base.rsplit(".", 1)[-1]}
    for it in d.get("inputs", []):
        info["inputs"][it["location"]] = (it["type"], it["name"])
    for key, kind in REFLECT_KEYS.items():
        for it in d.get(key, []):
            if "binding" in it:
                info["bindings"][(it.get("set", 0), it["binding"])] = (kind, it["name"])
    for pc in d.get("push_constants", []):
        t = d["types"].get(pc["type"])
        size, ok = 0, True
        for mem in (t or {}).get("members", []):
            sz = type_size(d["types"], mem["type"], None)
            if sz is None:
                ok = False
                break
            if "array" in mem:
                for a in mem["array"]:
                    sz *= a
            size = max(size, mem.get("offset", 0) + sz)
        info["push"] = size if ok else -1
    return base, info, None


def compile_all(shader_dir, sdk, tmp):
    glslc, cross = find_tool(sdk, "glslc"), find_tool(sdk, "spirv-cross")
    if not glslc or not cross:
        return None, "glslc/spirv-cross not found (set --sdk or VULKAN_SDK)"
    incdir = os.path.join(shader_dir, "include")
    files = sorted(glob.glob(os.path.join(shader_dir, "*.vert")) + glob.glob(os.path.join(shader_dir, "*.frag")))
    out, errs = {}, {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        for base, info, err in ex.map(lambda f: reflect_shader(glslc, cross, f, tmp, incdir), files):
            if info is not None:
                out[base] = info
            else:
                errs[base] = err
    for alias, real in SPV_ALIASES.items():
        if real in out:
            out[alias] = out[real]
    return (out, errs), None


# ---------------------------------------------------------------------------
# C++ scraping
# ---------------------------------------------------------------------------
STMT_RE = re.compile(r"(\w+)\s*(?:\[\s*([\w+\-*]+)\s*\])?\s*\.\s*(location|binding|format|offset|stride|inputRate)\s*=\s*([^;]+);")


def raw_line(raw_lines, ln):
    return raw_lines[ln - 1] if 0 < ln <= len(raw_lines) else ""


def parse_pipelines(path, text, sym):
    clean = strip_comments(text)
    raw_lines = text.split("\n")
    vib_decls = [m for m in re.finditer(r"VkVertexInputBindingDescription\s+(\w+)\s*(?:\[\w+\])?\s*\{\s*\}", clean)]
    via_decls = [m for m in re.finditer(r"VkVertexInputAttributeDescription\s+(\w+)\s*(?:\[\w+\])?\s*\{\s*\}", clean)]
    dsl_decls = [m for m in re.finditer(r"VkDescriptorSetLayoutBinding\s+(\w+)\s*(?:\[\w+\])?\s*\{\s*\}", clean)]
    dsl_groups = _dsl_groups(clean, dsl_decls)
    pcr_decls = [m for m in re.finditer(r"VkPushConstantRange\s+(\w+)\s*(?:\{\s*\}|\{([^;]*)\})\s*;", clean)]
    vert_names = [(m.start(), m.group(1)) for m in re.finditer(r'"([\w]+\.vert)\.spv"', clean)]
    frag_names = [(m.start(), m.group(1)) for m in re.finditer(r'"([\w]+\.frag)\.spv"', clean)]
    vert_names = [(p, n + "" if n.endswith(".vert") else n) for p, n in vert_names]
    frag_names = [(p, n) for p, n in frag_names]

    pipelines = []
    for gi, vm in enumerate(vib_decls):
        end = vib_decls[gi + 1].start() if gi + 1 < len(vib_decls) else len(clean)
        vname = vm.group(1)
        region = clean[vm.start():end]
        # --- binding stride/binding/inputRate ---
        stride_expr, stride_pos, binding_idx = None, None, 0
        for sm in STMT_RE.finditer(region):
            if sm.group(1) != vname:
                continue
            if sm.group(3) == "stride":
                stride_expr, stride_pos = sm.group(4).strip(), vm.start() + sm.start()
            elif sm.group(3) == "binding":
                try:
                    binding_idx = int(sm.group(4).strip())
                except ValueError:
                    pass
        # --- attributes ---
        attrs = {}
        for am in via_decls:
            if not (vm.start() < am.start() < end):
                continue
            aname = am.group(1)
            for sm in STMT_RE.finditer(region):
                if sm.group(1) != aname:
                    continue
                key = (aname, sm.group(2) or "")
                a = attrs.setdefault(key, {"pos": vm.start() + sm.start()})
                a[sm.group(3)] = sm.group(4).strip()
                a.setdefault("pos", vm.start() + sm.start())
        # --- shaders: those on the last line carrying a *.vert.spv before the binding decl ---
        prev_start = vib_decls[gi - 1].start() if gi else 0
        v_cands = [(p, n) for p, n in vert_names if prev_start <= p < vm.start()]
        f_cands = [(p, n) for p, n in frag_names if prev_start <= p < vm.start()]
        if gi == 0:
            v_cands = [(p, n) for p, n in vert_names if p < vm.start()]
            f_cands = [(p, n) for p, n in frag_names if p < vm.start()]

        def last_line(c):
            if not c:
                return []
            ln = lineno(clean, c[-1][0])
            return [n for p, n in c if lineno(clean, p) == ln]

        pipelines.append({
            "vib_pos": vm.start(), "stride_expr": stride_expr,
            "stride_pos": stride_pos if stride_pos is not None else vm.start(),
            "binding": binding_idx, "attrs": list(attrs.values()),
            "vert": last_line(v_cands), "frag": last_line(f_cands),
            "dsl": _nearest_dsl(clean, dsl_groups, vm.start()),
            "pcr": _nearest_pcr(clean, pcr_decls, vm.start(), sym, path),
        })
    return clean, raw_lines, pipelines


def _enclosing_conditional(clean, decl_pos, pos):
    """True if `pos` sits inside a brace block opened after decl_pos that follows if/else/for."""
    stack = []
    for i in range(decl_pos, pos):
        c = clean[i]
        if c == "{":
            head = clean[max(decl_pos, i - 80):i].rstrip()
            stack.append(bool(re.search(r"(\bif\s*\([^;{}]*\)|\belse|\bfor\s*\([^;{}]*\)|\bwhile\s*\([^;{}]*\))\s*$", head)))
        elif c == "}":
            if stack:
                stack.pop()
    return any(stack)


def _dsl_groups(clean, dsl_decls):
    groups = []
    for d in dsl_decls:
        m = re.search(r"VkDescriptorSetLayoutCreateInfo", clean[d.start():])
        end = d.start() + (m.start() if m else 4000)
        region = clean[d.start():end]
        entries = []
        for bm in re.finditer(r"\.\s*binding\s*=\s*([^;]+);", region):
            val = bm.group(1).strip()
            pos = d.start() + bm.start()
            opt = _enclosing_conditional(clean, d.start(), pos)
            if re.fullmatch(r"\d+", val):
                bvals = [int(val)]
            else:
                bvals = None
                fm = None
                for fm in re.finditer(r"for\s*\(\s*(?:int|uint32_t|unsigned)\s+(\w+)\s*=\s*(\d+)\s*;\s*\w+\s*(<=|<)\s*(\d+)", region[:bm.start()]):
                    pass
                if fm and fm.group(1) in val and bm.start() - fm.end() < 600:
                    hi = int(fm.group(4)) + (1 if fm.group(3) == "<=" else 0)
                    bvals = list(range(int(fm.group(2)), hi))
            entries.append({"bindings": bvals, "pos": pos, "type": None, "stage": None, "optional": opt})
        for i, e in enumerate(entries):
            lo = e["pos"] - d.start()
            hi = entries[i + 1]["pos"] - d.start() if i + 1 < len(entries) else len(region)
            seg = region[lo:hi]
            tm = re.search(r"descriptorType\s*=\s*(\w+)", seg)
            sm = re.search(r"stageFlags\s*=\s*([^;]+);", seg)
            e["type"] = tm.group(1) if tm else None
            e["stage"] = sm.group(1) if sm else None
        cm = re.search(r"vkCreateDescriptorSetLayout\s*\([^;]*?&\s*([\w>.\-]+?)\s*\)", clean[d.start() + (m.start() if m else 0):d.start() + (m.start() if m else 0) + 600])
        ident = re.split(r"->|\.", cm.group(1))[-1] if cm else None
        groups.append({"pos": d.start(), "entries": entries, "ident": ident,
                       "wild": any(e["bindings"] is None for e in entries)})
    return groups


def _nearest_dsl(clean, groups, before):
    """Return list of DSL groups indexed by set number for the pipeline layout created before `before`."""
    cands = [g for g in groups if g["pos"] < before]
    if not cands:
        return []
    plci = None
    for pm in re.finditer(r"VkPipelineLayoutCreateInfo\s+\w+", clean[:before]):
        plci = pm
    if plci is not None:
        reg = clean[plci.start():before]
        sm = re.search(r"pSetLayouts\s*=\s*([^;]+);", reg)
        if sm:
            ref = sm.group(1).strip()
            names = None
            if ref.startswith("&"):
                names = [re.split(r"->|\.", ref[1:].strip())[-1]]
            else:
                am = re.search(r"VkDescriptorSetLayout\s+" + re.escape(ref) + r"\s*\[[^\]]*\]\s*=\s*\{([^}]*)\}", clean[:before])
                if am:
                    names = [re.split(r"->|\.", x.strip())[-1] for x in am.group(1).split(",") if x.strip()]
            if names:
                out = []
                for n in names:
                    g = [g for g in cands if g["ident"] == n]
                    out.append(g[-1] if g else None)
                if any(out):
                    return out
    return [cands[-1]]


def _nearest_pcr(clean, pcr_decls, before, sym, path):
    cands = [d for d in pcr_decls if d.start() < before]
    if not cands:
        return None
    d = cands[-1]
    init = d.group(2)
    stage = size = None
    if init and init.strip():
        parts = [p.strip() for p in re.split(r",(?![^()]*\))", init)]
        if len(parts) >= 3:
            stage, size = parts[0], sym.eval(parts[2], path)
    else:
        nxt = clean[d.end():d.end() + 600]
        name = d.group(1)
        sm = re.search(name + r"\.stageFlags\s*=\s*([^;]+);", nxt)
        zm = re.search(name + r"\.size\s*=\s*([^;]+);", nxt)
        stage = sm.group(1) if sm else None
        size = sym.eval(zm.group(1), path) if zm else None
    return {"pos": d.start(), "stage": stage, "size": size}


def stage_has(expr, bit):
    if expr is None:
        return None
    if "ALL" in expr:
        return True
    toks = re.findall(r"VK_SHADER_STAGE_\w+", expr)
    if not toks:
        return None
    return bit in toks


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
def shader_in_class(t):
    if t.startswith("u"):
        return "uint", int(re.sub(r"\D", "", t) or 1)
    if t.startswith("i"):
        return "int", int(re.sub(r"\D", "", t) or 1)
    return "float", int(re.sub(r"\D", "", t) or 1)


def check_file(path, text, sym, shaders, findings):
    clean, raw_lines, pipes = parse_pipelines(path, text, sym)

    def add(sev, rule, pos, msg):
        ln = lineno(clean, pos)
        if "vk-lint: ok" in raw_line(raw_lines, ln):
            return
        findings.append(Finding(sev, rule, path, ln, msg))

    for p in pipes:
        vnames = p["vert"]
        label = "/".join(vnames) if vnames else "<shader not loaded in this file>"
        # ---- stride ----
        sexpr = p["stride_expr"]
        stride = None
        if sexpr is None:
            add("WARN", "VK-UNRESOLVED", p["vib_pos"], f"[{label}] vertex binding has no .stride assignment")
        else:
            if re.search(r"\?[^:]+:", sexpr):
                add("ERROR", "VK-STRIDE-FALLBACK", p["stride_pos"],
                    f"[{label}] stride '{sexpr}' is a runtime value with a fallback; pipelines are baked "
                    f"before geometry upload so the fallback silently wins (pre-c26b1eb2a shadow stride bug). "
                    f"Use the batcher's accessor (e.g. mc2_static_prop_vertex_stride()).")
                m = re.search(r":\s*(.+)$", sexpr)
                stride = sym.eval(m.group(1), path) if m else None
            else:
                stride = sym.eval(sexpr, path)
                if stride is None:
                    add("WARN", "VK-UNRESOLVED", p["stride_pos"], f"[{label}] cannot resolve stride '{sexpr}'")
            if stride == 0:
                add("ERROR", "VK-STRIDE-ZERO", p["stride_pos"], f"[{label}] stride resolves to 0 (every vertex reads offset 0)")
        # ---- VBO contract ----
        for vn in vnames:
            acc = VBO_CONTRACT.get(vn)
            if acc and stride is not None:
                want = sym.eval(acc + "()", path)
                if want is not None and want != stride:
                    add("ERROR", "VK-STRIDE-VBO", p["stride_pos"],
                        f"[{vn}] pipeline stride {stride} != shared VBO stride {want} ({acc}())")
        # ---- attributes ----
        attrs = []
        for a in p["attrs"]:
            try:
                loc = int(a.get("location", "").strip())
            except ValueError:
                add("WARN", "VK-UNRESOLVED", a["pos"], "attribute location not a literal")
                continue
            fmt = a.get("format", "").strip()
            off = sym.eval(a["offset"], path) if "offset" in a else 0
            f = _FMT.get(fmt)
            if f is None:
                add("WARN", "VK-UNRESOLVED", a["pos"], f"location {loc}: unknown VkFormat '{fmt}'")
            if off is None:
                add("WARN", "VK-UNRESOLVED", a["pos"], f"location {loc}: cannot resolve offset '{a.get('offset')}'")
            if "binding" in a:
                try:
                    if int(a["binding"].strip()) != p["binding"]:
                        continue  # attribute from another binding of this group
                except ValueError:
                    pass
            attrs.append((loc, f, off, a))
            if f and off is not None and stride and off + f[2] > stride:
                add("ERROR", "VK-ATTR-OOB", a["pos"],
                    f"[{label}] location {loc}: offset {off} + {f[2]}B ({fmt}) exceeds stride {stride}")
        # ---- shader comparison ----
        for vn in vnames:
            sh = shaders.get(vn)
            if sh is None:
                add("WARN", "VK-UNRESOLVED", p["vib_pos"], f"shader {vn} has no reflection (not compiled?)")
                continue
            by_loc = {l: (f, o, a) for l, f, o, a in attrs}
            for loc, (stype, sname) in sorted(sh["inputs"].items()):
                if loc not in by_loc:
                    add("ERROR", "VK-INPUT-MISSING", p["vib_pos"],
                        f"[{vn}] input location {loc} ({stype} {sname}) has no C++ vertex attribute")
                    continue
                f, o, a = by_loc[loc]
                if not f:
                    continue
                scls, scomp = shader_in_class(stype)
                if f[0] != scls:
                    add("ERROR", "VK-INPUT-TYPE", a["pos"],
                        f"[{vn}] location {loc} {sname}: shader {stype} ({scls}) vs format {a['format']} ({f[0]})")
                elif f[1] < scomp:
                    add("WARN", "VK-INPUT-WIDTH", a["pos"],
                        f"[{vn}] location {loc} {sname}: shader {stype} has {scomp} comps, format {a['format']} only {f[1]}")
            for loc, f, o, a in attrs:
                if loc not in sh["inputs"]:
                    add("WARN", "VK-ATTR-UNUSED", a["pos"], f"[{vn}] attribute location {loc} not consumed by shader")
        # ---- descriptors + push constants ----
        for kind, names in (("vert", p["vert"]), ("frag", p["frag"])):
            for sn in names:
                sh = shaders.get(sn)
                if sh is None:
                    continue
                dsl, pcr = p["dsl"], p["pcr"]
                bit = STAGE_BIT[kind]
                for (sset, b), (dk, dname) in sorted(sh["bindings"].items()):
                    grp = dsl[sset] if sset < len(dsl) else None
                    if grp is None or not grp["entries"]:
                        if dsl:
                            add("WARN", "VK-UNRESOLVED", p["vib_pos"], f"[{sn}] set{sset} binding {b} ({dname}): layout for set {sset} not found in source")
                        continue
                    e = None
                    for en in grp["entries"]:
                        if en["bindings"] and b in en["bindings"]:
                            e = en
                    if e is None:
                        if not grp["wild"]:
                            add("ERROR", "VK-DESC-MISSING", grp["pos"],
                                f"[{sn}] set{sset} binding {b} ({dname}, {dk}) missing from descriptor set layout")
                        continue
                    if e["type"] and e["type"] not in DESC_COMPAT[dk]:
                        add("ERROR", "VK-DESC-TYPE", e["pos"],
                            f"[{sn}] set{sset} binding {b} ({dname}): shader {dk} vs layout {e['type']}")
                    if stage_has(e["stage"], bit) is False:
                        add("ERROR", "VK-DESC-STAGE", e["pos"],
                            f"[{sn}] set{sset} binding {b} ({dname}): layout stageFlags '{e['stage']}' lacks {bit}")
                if sh["push"] is not None:
                    if pcr is None:
                        add("ERROR", "VK-PUSH-MISSING", p["vib_pos"], f"[{sn}] shader uses push constants but no VkPushConstantRange precedes pipeline")
                    else:
                        if sh["push"] > 0 and pcr["size"] is not None and pcr["size"] < sh["push"]:
                            add("ERROR", "VK-PUSH-SIZE", pcr["pos"],
                                f"[{sn}] push block {sh['push']}B > range size {pcr['size']}B")
                        if stage_has(pcr["stage"], bit) is False:
                            add("ERROR", "VK-PUSH-STAGE", pcr["pos"], f"[{sn}] push range stageFlags '{pcr['stage']}' lacks {bit}")


def build_symbols(root, extra_dirs):
    sym = Symbols()
    pats = ["GameOS/gameos/*.h", "GameOS/gameos/*.cpp", "RenderCore/*.h"]
    for pat in pats:
        for f in glob.glob(os.path.join(root, pat)):
            f = os.path.abspath(f)
            try:
                sym.scan(f, strip_comments(read(f)))
            except OSError:
                pass
    return sym


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--sdk", default=os.environ.get("VULKAN_SDK") or SDK_DEFAULT)
    ap.add_argument("--shader-dir", default=None)
    ap.add_argument("--cpp", action="append", help="C++ file(s) to lint (default GameOS/gameos/vulkan_*.cpp)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--warn-as-error", action="store_true")
    a = ap.parse_args(argv)

    shader_dir = a.shader_dir or os.path.join(a.root, "shaders", "vulkan")
    with tempfile.TemporaryDirectory(prefix="vkpl_") as tmp:
        res, err = compile_all(shader_dir, a.sdk, tmp)
    if res is None:
        print(f"check-vk-pipeline-layout: {err}", file=sys.stderr)
        return 2
    shaders, shader_errs = res
    sym = build_symbols(a.root, [])
    cpps = a.cpp or sorted(glob.glob(os.path.join(a.root, "GameOS", "gameos", "vulkan_*.cpp")))
    findings = []
    for n, e in sorted(shader_errs.items()):
        findings.append(Finding("ERROR", "VK-SHADER-COMPILE", os.path.join(shader_dir, n), 1, e))
    for c in cpps:
        c = os.path.abspath(c)
        txt = read(c)
        if "VkVertexInputBindingDescription" not in txt and "VkDescriptorSetLayoutBinding" not in txt:
            continue
        check_file(c, txt, sym, shaders, findings)
    errs = [f for f in findings if f.sev == "ERROR" or a.warn_as_error]
    for f in findings:
        if f.sev == "ERROR" or not a.quiet:
            print(f)
    if errs:
        print(f"check-vk-pipeline-layout: FAIL ({len(errs)} error(s), {len(findings) - len(errs)} warning(s))")
        return 1
    print(f"check-vk-pipeline-layout: OK ({len(cpps)} file(s), {len(shaders)} shader(s), {len(findings)} warning(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
