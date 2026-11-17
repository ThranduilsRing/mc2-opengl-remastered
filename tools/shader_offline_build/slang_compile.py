"""slang_compile.py - Slang (.slang) -> Vulkan SPIR-V for build_variants.py.

Targets the same SPIR-V as the GLSL Vulkan path (glslangValidator --target-env
vulkan1.2 = SPIR-V 1.5): `-profile spirv_1_5`. Override per pilot with the
`spirv` key in the pilots manifest if a shader needs a newer profile.
See docs/shaders-slang.md.
"""
from __future__ import annotations
import re
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import shader_common  # noqa: E402
import console_child  # noqa: E402

SLANG_STAGE = {"vert": "vertex", "frag": "fragment", "comp": "compute"}
DEFAULT_PROFILE = "spirv_1_5"
# 50011 = "SPIR-V version too old" (1.5 matches glslang --target-env vulkan1.2).
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
                       timeout=60,
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


def uncomment(text):
    return re.sub(r'/\*.*?\*/|//[^\n]*', lambda m: '\n' * m.group().count('\n'),
                  text, flags=re.S)


def dependencies(src: Path):
    """Conservative local dependency closure, including inactive conditional imports."""
    src = src.resolve()
    found = {}

    def visit(path):
        if path in found:
            return
        text = source_text_canon(path)
        found[path] = text
        clean = uncomment(text)
        refs = re.findall(r'\b(?:export\s+)?import\s+([\w.]+)\s*;', clean)
        names = [r.replace('.', '/') + '.slang' for r in refs]
        for include in re.findall(r'^\s*#\s*include\s+([^\n]+)', clean, re.M):
            literal = re.fullmatch(r'\s*["<]([^">]+)[">]\s*', include)
            if not literal:
                raise ValueError(f'{path.name}: only literal includes are supported')
            names.append(literal.group(1))
        for name in names:
            target = next((d / name for d in (path.parent, src.parent)
                           if (d / name).is_file()), None)
            if target is None:
                raise ValueError(f'{path.name}: unresolved local dependency {name}')
            visit(target.resolve())
    visit(src)
    return found


def source_bundle(src: Path):
    """Portable source hash input; paths and contents are both significant."""
    import os
    files = dependencies(src)
    return json.dumps(sorted((Path(os.path.relpath(p, src.resolve().parent)).as_posix(), t)
                             for p, t in files.items()), ensure_ascii=False)


RESOURCE_RE = re.compile(
    r'(?:\[\[\s*vk::binding\s*\(\s*(\d+)\s*,\s*(\d+)\s*\)\s*\]\]\s*)?'
    r'\b(ConstantBuffer|(?:RW)?StructuredBuffer|(?:RW)?ByteAddressBuffer|'
    r'(?:RW)?Texture\w*|Sampler\d\w*|SamplerState|SamplerComparisonState)'
    r'\s*(?:<([^;]+?)>)?\s+(\w+)\s*(?:\[[^]]*\])?\s*;')


def resource_bindings(text):
    for m in RESOURCE_RE.finditer(uncomment(text)):
        binding, desc_set, kind, typ, name = m.groups()
        ns = ('ubo' if kind == 'ConstantBuffer' else 'ssbo' if 'Buffer' in kind
              else 'image' if kind.startswith('RWTexture') else 'sampler' if kind.startswith('Sampler') and not kind[7:8].isdigit() else 'texture')  # SamplerND = combined image sampler
        yield {'binding': int(binding) if binding is not None else None,
               'set': int(desc_set) if desc_set is not None else None,
               'namespace': ns, 'name': name, 'type': typ,
               'line': text.count('\n', 0, m.start()) + 1}


def validate_reflection(raw, src, root=ROOT):
    """Reject implicit/duplicate descriptors, buffer type mismatches and ABI drift."""
    declarations = [r for t in dependencies(src).values() for r in resource_bindings(t)]
    errors = [f"{r['name']}: explicit vk::binding required" for r in declarations
              if r['binding'] is None]
    seen = set()
    for group in ('ubos', 'ssbos', 'textures', 'separate_images', 'separate_samplers', 'images', 'samplers'):
        for r in raw.get(group, []):
            slot = (r.get('set'), r.get('binding'))
            if None in slot or slot in seen:
                errors.append(f"{r.get('name')}: missing or duplicate descriptor {slot}")
            if slot not in {(d['set'], d['binding']) for d in declarations}:
                errors.append(f"{r.get('name')}: descriptor {slot} has no explicit source binding")
            seen.add(slot)
            ns = {'ubos': 'ubo', 'ssbos': 'ssbo', 'images': 'image',
                  'textures': 'texture', 'separate_images': 'texture',
                  'separate_samplers': 'sampler'}.get(group)
            if group == 'samplers':
                ns = 'sampler' if r.get('type') == 'sampler' else 'texture'
            if (slot[0], slot[1], ns) not in {
                    (d['set'], d['binding'], d['namespace']) for d in declarations}:
                errors.append(f'{r.get("name")}: descriptor {slot} source buffer type mismatch ({ns})')
    return errors + validate_abi(raw, root, check_bindings=True)


def validate_abi(raw, root=ROOT, check_bindings=False):
    errors = []
    sys.path.insert(0, str(ROOT / 'tools'))
    import gpu_abi_gen as abi
    types = raw.get('types', {})
    pending = []
    for group in ('ubos', 'ssbos'):
        for block in raw.get(group, []):
            type_id = block.get('type')
            if not isinstance(type_id, str) or not type_id:
                errors.append(f"gpu_abi: {group} {block.get('name')} missing buffer type")
            else:
                pending.append(type_id)
    physical = set()
    while pending:
        type_id = pending.pop()
        if type_id in physical:
            continue
        physical.add(type_id)
        if type_id not in types:
            errors.append(f'gpu_abi: missing reflected buffer type {type_id}')
            continue
        pending.extend(m['type'] for m in types[type_id].get('members', [])
                       if m['type'].startswith('_'))
    for header, _, _ in abi.STRUCTS:
        path = Path(root) / header
        if not path.exists():
            continue
        info = abi.parse_source(path.read_text(encoding='utf-8'), header)
        for type_id, reflected in types.items():
            if reflected.get('name') not in (info['glsl'], info['cpp']):
                continue
            # SPIR-V also contains undecorated function-local copies of structs.
            if (physical and type_id not in physical) or (not physical and
                    not any('offset' in m for m in reflected.get('members', []))):
                continue
            if info['block'] and check_bindings:
                for block in raw.get('ubos', []):
                    if block.get('type') == type_id and (
                            block.get('set'), block.get('binding')) != (
                                int(info['set']), int(info['binding'])):
                        errors.append(f"gpu_abi: {reflected['name']} set/binding drift")
            offsets, _, _ = abi.struct_offsets(info['fields'], info['rules'])
            expected = []
            for (name, typ), (_, offset) in zip(info['fields'], offsets):
                base, arr = abi.glsl_type_str(typ)
                expected.append((name, base, offset, [] if not arr else [typ[2]]))
            got = [(m['name'], m['type'], m.get('offset'), m.get('array', []))
                   for m in reflected.get('members', [])]
            if got != expected:
                errors.append(f"gpu_abi: {reflected['name']} member type/offset/array drift")
            for m, (_, typ) in zip(reflected.get('members', []), info['fields']):
                if isinstance(typ, tuple):
                    stride = abi.align_size(typ, info['rules'])[1] // typ[2]
                    if m.get('array_stride') != stride:
                        errors.append(f"gpu_abi: {m['name']} array stride drift")
                if (typ == 'mat4' or isinstance(typ, tuple) and typ[1] == 'mat4') and (
                        m.get('matrix_stride') != 16 or m.get('row_major', False)):
                    errors.append(f"gpu_abi: {m['name']} matrix layout drift")
    return errors


def compile_slang(exe, src: Path, stage: str, out: Path, entry="main",
                  profile=DEFAULT_PROFILE, defines=()):
    """Return (ok, diagnostics). Never leaves a partial `out` behind on failure."""
    if stage not in SLANG_STAGE:
        return False, f"unsupported stage '{stage}' (vert|frag|comp)"
    out.unlink(missing_ok=True)
    cmd = [exe, str(src), "-I", str(src.parent), "-target", "spirv", "-profile", profile,
           "-matrix-layout-column-major",
           "-stage", SLANG_STAGE[stage], "-entry", entry,
           "-warnings-disable", DISABLED_WARNINGS, "-o", str(out)]
    cmd += [f"-D{d}" for d in sorted(defines)]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=60,
                           **console_child.child_kwargs(redirected=True))
    except (OSError, subprocess.SubprocessError) as e:
        out.unlink(missing_ok=True)
        return False, str(e)
    ok = r.returncode == 0 and out.exists()
    if not ok and out.exists():
        out.unlink()
    return ok, (r.stdout + r.stderr).strip()


def optimize(spirv_opt, out: Path):
    """spirv-opt -O in place (CMake build only); same flags as the glslang rule in GameOS/gameos/CMakeLists.txt."""
    r = subprocess.run([spirv_opt, '-O', '--preserve-bindings', '--preserve-spec-constants',
                        '--target-env=vulkan1.2', str(out), '-o', str(out)],
                       capture_output=True, text=True, timeout=120,
                       **console_child.child_kwargs(redirected=True))
    return r.returncode == 0, (r.stderr or r.stdout).strip()


def bake(src, out, entry='main', defines=(), exe=None, cross=None, spirv_opt=None):
    out.unlink(missing_ok=True)
    exe = exe or find_slangc()
    cross = cross or shader_common.find_tool('spirv-cross')
    if not exe or not cross:
        return False, 'slangc and spirv-cross required (Vulkan SDK)'
    try:
        error = check_version(exe)
        if error:
            return False, error
        dependencies(src)
        # A shader needing wave ops opts in with a `// slang-profile: spirv_1_3` line.
        found = re.search(r'^// slang-profile: (spirv_\d_\d)\s*$',
                          src.read_text(encoding='utf-8', errors='replace'), re.M)
        ok, diag = compile_slang(exe, src, src.suffixes[-2][1:], out, entry,
                                 profile=found.group(1) if found else DEFAULT_PROFILE,
                                 defines=defines)
        if not ok:
            return ok, diag
        raw = json.loads(subprocess.run([cross, str(out), '--reflect'], check=True,
                         capture_output=True, text=True, timeout=60,
                         **console_child.child_kwargs(redirected=True)).stdout)
        errors = validate_reflection(raw, src)
        if not errors:
            if spirv_opt:
                ok, diag = optimize(spirv_opt, out)
                if not ok:
                    out.unlink(missing_ok=True)
                    return False, diag
            return True, diag
        diag = '\n'.join(errors)
    except (ValueError, OSError, subprocess.SubprocessError) as e:
        diag = str(e)
    out.unlink(missing_ok=True)
    return False, diag


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source', type=Path)
    ap.add_argument('-o', required=True, type=Path)
    ap.add_argument('--slangc')
    ap.add_argument('--spirv-cross')
    ap.add_argument('--spirv-opt')
    args = ap.parse_args()
    ok, diagnostic = bake(args.source, args.o, exe=args.slangc, cross=args.spirv_cross, spirv_opt=args.spirv_opt)
    if diagnostic:
        print(diagnostic)
    sys.exit(0 if ok else 1)
