#!/usr/bin/env python3
"""nttp_probe.py -- compile-cost probe for gate::get<"MC2_X">() (typed gate registry, G1a).

WHAT: measures front end (c1xx), back end (c2), .obj and per-TU .pdb size of
  * synthetic TUs with 0 / 102 / 1400 read sites in four shapes (lambda-static initialiser,
    namespace-scope initialiser, use inside a function template, use inside a macro),
  * a copy of gos_postprocess.cpp (103 real raw reads today) with and without 102 appended sites,
    and two controls that isolate the name-as-NTTP cost: a plain load(<idx>) call and a
    size_t-NTTP index form (the "pre-resolved index" fallback).
WHY: the design budget for call sites that spell the gate name (README: typed-gate-registry-design
  section 5.1) needs numbers, not a guess.
USAGE: py -3 .claude/cleanup/probe/nttp_probe.py <outdir> [--reps 6]   (needs the Ninja tree for flags)
Throwaway tool: results live in .claude/cleanup/g1a-status.md.
"""
import argparse, contextlib, io, json, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
from configure_ninja import VCVARS  # noqa: E402


def gate_names():
    src = open(os.path.join(ROOT, "Gates", "gate_index.gen.h"), encoding="utf-8").read()
    return [(n, k) for n, k in re.findall(r'\{"(MC2_\w+)", Kind::(\w+)', src)]


NAMES = gate_names()
USABLE = [n for n, k in NAMES if k != "String"]


def site(n, i, call):
    c = call(n)
    s = i % 4
    if s == 0:
        return f"int lam_{i}() {{ static const bool v = []{{ return {c}; }}(); return v; }}\n"
    if s == 1:
        return f"static const bool ns_{i} = {c};\n"
    if s == 2:
        return (f"template <class T> int tpl_{i}(T x) {{ return {c} ? x : x + 1; }}\n"
                f"int tpl_use_{i}() {{ return tpl_{i}(1); }}\n")
    return f"#define MACRO_{i}() ({c} ? 1 : 0)\nint mac_{i}() {{ return MACRO_{i}(); }}\n"


def body(count, call):
    return "".join(site(USABLE[i % len(USABLE)], i, call) for i in range(count))


def by_name(n):
    return f'(int)gate::get<"{n}">()'


def by_load(n):
    return f"(int)gate::detail::load({[x for x, _ in NAMES].index(n)})"


def by_tidx(n):
    return f"getIdx<{[x for x, _ in NAMES].index(n)}>()"


def flags():
    cc = json.load(open(os.path.join(ROOT, "build64-ninja", "compile_commands.json")))
    cmd = next(e["command"] for e in cc if e["file"].replace("\\", "/").endswith("gos_postprocess.cpp"))
    f = re.search(r"/nologo.*?/Fo", cmd).group(0)[:-3].replace("/MP", "")
    return f + " -I" + os.path.join(ROOT, "GameOS", "gameos")


def compile_tu(fl, src, tag):
    full = (f'call "{VCVARS}" >nul && cl.exe {fl} /d1reportTime /Bt+ /Fo{tag}.obj /Fd{tag}.pdb /FS -c {src}')
    r = subprocess.run(full, shell=True, capture_output=True, text=True)
    out = r.stdout + r.stderr
    fe = re.findall(r"time\(.*?c1xx\.dll\)=([\d.]+)s", out)
    be = re.findall(r"time\(.*?c2\.dll\)=([\d.]+)s", out)
    if r.returncode or not fe:
        raise SystemExit(f"{tag}: compile failed\n{out[-1500:]}")
    return float(fe[0]), float(be[0] if be else 0), os.path.getsize(tag + ".obj"), os.path.getsize(tag + ".pdb")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--reps", type=int, default=6)
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    os.chdir(a.outdir)
    fl = flags()
    hdr = '#include "Gates/gate.h"\nusing namespace mc2;\n'
    tus = {}
    for n in (0, 102, 1400):
        tus[f"synth{n}"] = hdr + body(n, by_name) + "\nint probe_anchor(){return 0;}\n"
    base = open(os.path.join(ROOT, "GameOS", "gameos", "gos_postprocess.cpp"), encoding="utf-8", errors="replace").read()
    inc = '\n#include "Gates/gate.h"\nnamespace probe_ns { using namespace mc2;\n'
    tus["pp_base"] = base
    tus["pp_hdr_only"] = base + '\n#include "Gates/gate.h"\n'
    tus["pp_names102"] = base + inc + body(102, by_name) + "}\n"
    tus["pp_load102"] = base + inc + body(102, by_load) + "}\n"
    tus["pp_tidx102"] = (base + inc + "template <std::size_t I> inline int getIdx(){ return (int)gate::detail::load(I);}\n"
                         + body(102, by_tidx) + "}\n")
    for t, text in tus.items():
        open(t + ".cpp", "w", encoding="utf-8").write(text)
    res = {t: [] for t in tus}
    for rep in range(a.reps):
        for t in tus:
            res[t].append(compile_tu(fl, t + ".cpp", f"{t}_{rep}"))
    print(f"{'TU':12s} {'fe min':>7s} {'fe med':>7s} {'be min':>7s} {'be med':>7s} {'obj':>9s} {'pdb':>9s}")
    for t, v in res.items():
        fe = sorted(x[0] for x in v); be = sorted(x[1] for x in v); m = len(v) // 2
        print(f"{t:12s} {fe[0]:7.3f} {fe[m]:7.3f} {be[0]:7.3f} {be[m]:7.3f} {v[0][2]:9d} {v[0][3]:9d}")


if __name__ == "__main__":
    main()
