"""Omnitech ABL builtins: every registration's spec string must match what its exec function pops, and its return type what it pushes.

The ABL compiler type-checks and consumes exactly len(spec) arguments per call; an exec function that pops a different count (or
pushes a result for a function registered with no return type) unbalances the interpreter stack. Static check over code/ablmc2*.cpp.
"""
import re
from pathlib import Path

CODE = Path(__file__).resolve().parents[2] / "code"
POP = re.compile(r"\bABLi_pop\w+\s*\(")
PUSH = re.compile(r"\bABLi_push(?:Integer|Real|Boolean)\s*\(")
REG = re.compile(r'ABLi_addFunction\("(\w+)",\s*\w+,\s*(?:"([^"]*)"|nullptr),\s*(?:"(\w)"|nullptr),\s*(\w+)\)')


def _body(src, fn):
    m = re.search(r"\bvoid\s+" + fn + r"\s*\(\s*void\s*\)\s*\{", src)
    if not m:
        return None
    depth, i = 1, m.end()
    while depth and i < len(src):
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        i += 1
    body = re.sub(r"//[^\n]*", "", src[m.end():i])
    return re.sub(r"/\*.*?\*/", "", body, flags=re.S)


def _omnitech_regs():
    text = (CODE / "ablmc2.cpp").read_text(encoding="utf-8", errors="ignore")
    start = text.index('"setwillrequesthelp"')
    return REG.findall(text[start:text.index("logAblFunctionTableCapacity();", start)])


def test_pops_and_pushes_match_registration():
    srcs = {p: p.read_text(encoding="utf-8", errors="ignore") for p in CODE.glob("ablmc2*.cpp")}
    regs = _omnitech_regs()
    assert len(regs) > 40
    bad = []
    for name, spec, ret, fn in regs:
        body = next((b for s in srcs.values() if (b := _body(s, fn)) is not None), None)
        if body is None:
            bad.append(f"{name}: exec {fn} not found")
            continue
        if len(POP.findall(body)) != len(spec or ""):
            bad.append(f"{name}: spec '{spec}' but {len(POP.findall(body))} pops")
        if len(PUSH.findall(body)) < (1 if ret else 0) or (not ret and PUSH.search(body)):
            bad.append(f"{name}: return '{ret}' but pushes {len(PUSH.findall(body))}")
    assert not bad, "\n".join(bad)


def test_out_params_are_checked_reference_specs():
    regs = {r[0]: r for r in _omnitech_regs()}
    # real out-params use 'v' (compiler requires a bare real variable); 'r' would let a literal reach ABLi_popRealPtr
    assert regs["gettargetrelativeposition"][1] == "ivv"
    assert regs["hasorderfromplayer"][1] == ""
    assert regs["scanareacapture"][1:3] == ("*", "i")
