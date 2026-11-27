"""Footprint fades must preserve a named mean; exceptions are scoped below."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Each entry names an existing, reviewed exception, rather than exempting a file.
ALLOW = {
    "mv_detailFade": "normal detail, mean-zero slopes",
    "wpnFade": "returns a weight; consumers must preserve their mean",
    "alphaLift": "alpha mip coverage compensation, not a colour fade",
    "propCoverage": "alpha mip coverage compensation",
    "waveLOD": "water look held for owner; separate atmosphere lane",
    "keep": "water slope detail, mean-zero; water look held for owner",
    "resolved": "water foam look held for owner",
    "swashW": "water swash look held for owner",
    "edgeAA": "shore coverage antialiasing",
    "shore": "shore coverage antialiasing",
    "foam": "shore coverage antialiasing",
    "wash": "shore coverage antialiasing",
    "contact": "shore coverage antialiasing",
    "edge": "shore coverage antialiasing",
    "reef": "shore coverage antialiasing",
    "w": "water slope detail, mean-zero",
    "r": "water wave slope resolution, mean-zero",
}


def sources():
    yield from (ROOT / "shaders/vulkan").rglob("*.frag")
    yield from (ROOT / "shaders/vulkan").rglob("*.glsl")
    yield from (ROOT / "shaders/include").glob("*.hglsl")


def violations(name, text):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    statements = text.split(";")
    # Follow derivative/LOD aliases so a separate declaration cannot hide a fade.
    tainted = set(re.findall(r"\b\w*[Ff]ootprint\w*\b", text))
    for _ in range(8):
        for stmt in statements:
            match = re.search(r"\b(\w+)\s*=(?!=)(.*)", stmt, re.S)
            if not match:
                continue
            calls = set(re.findall(r"\b(\w+)\s*\(", match[2]))
            if calls - {"fwidth", "textureQueryLod", "length", "max", "min", "abs", "clamp", "log2", "dFdx", "dFdy", "wpnFootprint"}:
                continue
            if (re.search(r"\b(fwidth|textureQueryLod|wpnFootprint)\s*\(", match[2])
                    or set(re.findall(r"\b\w+\b", match[2])) & tainted):
                tainted.add(match[1])
    errors = []
    for stmt in statements:
        match = re.search(r"\b(\w+)\s*=(?!=)(.*)", stmt, re.S)
        if not match:
            continue
        var, expr = match.groups()
        fade_args = []
        for call in re.finditer(r"\b(smoothstep|wpnFade|mv_detailFade|textureQueryLod)\s*\(", expr):
            depth, end = 1, call.end()
            while depth and end < len(expr):
                depth += (expr[end] == "(") - (expr[end] == ")")
                end += 1
            fade_args.append(expr[call.end():end])
        if not any(set(re.findall(r"\b\w+\b", args)) & (tainted | {"fwidth", "textureQueryLod"}) for args in fade_args):
            continue
        # Normal-return helpers have no local assignment for their smoothstep.
        exception_file = {
            "mv_detailFade": "terrain_detail_normal.glsl", "wpnFade": "world_patch_noise.glsl",
            "alphaLift": "vfx_vegetation.frag", "propCoverage": "color_prop_tier1.frag",
        }.get(var, "water_geom.frag")
        if var in ALLOW and Path(name).name == exception_file:
            continue
        consumers = [s for s in statements if re.search(rf"\b{var}\b", s) and s != stmt]
        multiplied = "*" in expr or any("*" in s for s in consumers)
        mean_mix = any(re.search(r"mix\s*\([^;]*(?:[Mm]ean|\b0\.0335\b)", s) for s in [stmt] + consumers)
        if multiplied and not mean_mix:
            errors.append(f"{name}: {var} fades a multiplied term without a named mean")
    # Shading code must not key its grain to the moving viewport.
    if "gl_FragCoord" in text and (name.endswith(".hglsl") or "/include/" in name):
        errors.append(f"{name}: screen-keyed shading include")
    if name.endswith("vfx_vegetation.frag") and "gl_FragCoord" in text:
        errors.append(f"{name}: screen-keyed vegetation coverage")
    if name.endswith("terrain_detail_normal.glsl"):
        for line in text.splitlines():
            if "mv_srcAo" in line and re.search(r"\b(?:fw|df)\w+\b", line):
                errors.append(f"{name}: footprint-weighted source AO")
    return errors


def test_footprint_fades_preserve_mean():
    errors = []
    for path in sources():
        errors += violations(path.relative_to(ROOT).as_posix(), path.read_text(encoding="utf-8"))
    assert not errors, "\n".join(errors)


def test_scanner_follows_aliases_and_rejects_zero_fade():
    assert violations("example.frag", "float fw = fwidth(x); float fade = 1-smoothstep(1,2,fw); c *= fade;")
    assert not violations("example.frag", "float fw=fwidth(x); float fade=smoothstep(1,2,fw); c=mix(c, colourMean, fade);")
