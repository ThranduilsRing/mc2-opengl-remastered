#!/usr/bin/env python3
"""axis_migrate_glsl.py -- rewrite hand-typed `vec3(-a.x, a.z, a.y)` triples in GLSL to `mc2ToRender(a)`.

What: replaces every triplet-ctor swap whose three components share one base expression with a call to the
shaders/include/axis.hglsl helper, adds the include when the file lacks it, and appends `.xyz` when the base is a
vec4 (declared `vec4 NAME` in the file, or a `.light_dir[...]` array element).
Why: D11 -- one typed spelling of the (x, y, z) -> (-x, z, y) conversion; tools/axis_swizzle_ratchet.py counts what is left.
Usage: py -3 tools/axis_migrate_glsl.py [--apply] FILE...   (dry run by default; prints each rewrite)
The include line style follows the file: GL shaders `#include <include/axis.hglsl>`, shaders/vulkan `#include "../include/axis.hglsl"`,
shaders/include `#include <axis.hglsl>`.
"""
import re
import sys

TRIPLE = re.compile(r"vec3\(\s*-\s*([A-Za-z_][\w\.\[\]]*?)\s*\.x\s*,\s*\1\s*\.z\s*,\s*\1\s*\.y\s*\)")


def include_line(path):
    p = path.replace("\\", "/")
    if "/shaders/vulkan/" in "/" + p:
        return '#include "../include/axis.hglsl"'
    if "/shaders/include/" in "/" + p:
        return "#include <axis.hglsl>"
    return "#include <include/axis.hglsl>"


def is_vec4(text, base):
    root = re.split(r"[\.\[]", base)[0]
    if re.search(r"\bvec4\s+%s\b" % re.escape(root), text):
        return True
    return ".light_dir[" in base


def migrate(path, apply):
    raw = open(path, "rb").read()
    text = raw.decode("utf-8", "surrogateescape")
    nl = "\r\n" if "\r\n" in text else "\n"
    n = 0

    def sub(m):
        nonlocal n
        n += 1
        base = m.group(1)
        arg = base + ".xyz" if is_vec4(text, base) else base
        return "mc2ToRender(%s)" % arg

    out = TRIPLE.sub(sub, text)
    if n and "axis.hglsl" not in out:
        inc = include_line(path)
        lines = out.split("\n")
        last = -1
        for i, l in enumerate(lines[:120]):
            if l.startswith("#include"):
                last = i
        if last < 0:
            for i, l in enumerate(lines[:40]):
                if l.startswith("#version"):
                    last = i
                    break
        lines.insert(last + 1, inc + ("\r" if nl == "\r\n" else ""))
        out = "\n".join(lines)
    print("%s: %d site(s)%s" % (path, n, "" if apply else " (dry run)"))
    if apply and n:
        open(path, "wb").write(out.encode("utf-8", "surrogateescape"))
    return n


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--apply"]
    total = sum(migrate(f, "--apply" in sys.argv) for f in args)
    print("total", total)
