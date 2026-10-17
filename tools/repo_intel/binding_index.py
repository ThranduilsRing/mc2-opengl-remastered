#!/usr/bin/env python3
"""
REPO-INTEL-1c: binding_index.py
Shader / GL binding point index.
Sources:
  1. docs/render-binding-registry.md   — documented registry (authoritative audit doc)
  2. shaders/ layout(binding=N)        — live grep of GLSL source
  3. C++ glBindBufferBase calls        — live grep of engine source

Use via repo_query.py:
    python tools/repo_intel/repo_query.py binding 5
    python tools/repo_intel/repo_query.py binding --conflicts
    python tools/repo_intel/repo_query.py binding --namespace ssbo
    python tools/repo_intel/repo_query.py binding         (summary)
"""

import os
import re
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_SHADER_EXTS    = {'.vert', '.frag', '.comp', '.geom', '.tesc', '.tese', '.hglsl', '.slang'}
_CPP_EXTS       = {'.cpp', '.h', '.hpp', '.c'}
_EXCLUDE_DIRS   = {'build64', '3rdparty', '.git', 'releases', 'dist', '.vscode'}

# GL target → namespace label
_GL_TARGET_NS = {
    'GL_SHADER_STORAGE_BUFFER': 'ssbo',
    'GL_UNIFORM_BUFFER':        'ubo',
    'GL_ATOMIC_COUNTER_BUFFER': 'atomic',
    'GL_TRANSFORM_FEEDBACK_BUFFER': 'xfb',
}

_SHADER_BINDING_RE = re.compile(
    r'layout\s*\([^)]*\bbinding\s*=\s*(\d+)[^)]*\)\s*(.*)',
    re.IGNORECASE,
)
_GL_BIND_BASE_RE = re.compile(
    r'glBindBufferBase\s*\(\s*(GL_\w+)\s*,\s*(\d+(?:u)?)',
    re.IGNORECASE,
)
_GL_BIND_RANGE_RE = re.compile(
    r'glBindBufferRange\s*\(\s*(GL_\w+)\s*,\s*(\d+(?:u)?)',
    re.IGNORECASE,
)

# Sections in render-binding-registry.md mapped to namespace tags
_SECTION_NS = {
    'Runtime UBO':           'ubo',
    'Runtime SSBO':          'ssbo',
    'Texture sampler units': 'texture',
    'Image':                 'image',
    'Fixture-only':          'fixture',
}


# ---------------------------------------------------------------------------
# Registry parser (render-binding-registry.md)
# ---------------------------------------------------------------------------

def _parse_registry(repo_root: Path) -> dict:
    """
    Parse render-binding-registry.md table rows.
    Returns {(namespace, binding_int): [entry_dict, ...]}.
    Multiple entries for the same slot are legitimate multi-binder cases.
    """
    path = repo_root / 'docs' / 'render-binding-registry.md'
    if not path.exists():
        return {}

    text    = path.read_text(encoding='utf-8', errors='replace')
    lines   = text.splitlines()
    result  = {}
    cur_ns  = None
    in_data = False  # True after we've seen the header separator row

    for line in lines:
        # Section heading detection
        stripped = line.strip()
        if stripped.startswith('##'):
            cur_ns  = None
            in_data = False
            for section_keyword, ns in _SECTION_NS.items():
                if section_keyword.lower() in stripped.lower():
                    cur_ns = ns
                    break
            continue

        if cur_ns is None or not stripped.startswith('|'):
            continue

        cells = [c.strip() for c in line.split('|')[1:-1]]
        if not cells:
            continue

        # Separator row (e.g. |---:|---|...)
        if all(re.match(r'^[-:\s]*$', c) for c in cells):
            in_data = True
            continue

        if not in_data:
            # Header row — skip
            continue

        # Data row — first cell is binding number
        binding_raw = cells[0]
        try:
            binding_n = int(re.sub(r'[^\d]', '', binding_raw.split()[0]))
        except (ValueError, IndexError):
            continue

        entry = {
            'binding':          binding_n,
            'namespace':        cur_ns,
            'name':             cells[1] if len(cells) > 1 else '',
            'owner':            cells[2] if len(cells) > 2 else '',
            'type':             cells[3] if len(cells) > 3 else '',
            'cpp_site':         cells[4] if len(cells) > 4 else '',
            'shader_consumers': cells[5] if len(cells) > 5 else '',
            'notes':            cells[6] if len(cells) > 6 else '',
            'source':           'docs/render-binding-registry.md',
        }

        key = (cur_ns, binding_n)
        result.setdefault(key, []).append(entry)

    return result


# ---------------------------------------------------------------------------
# Live grep: shader layout(binding=N)
# ---------------------------------------------------------------------------

def _grep_shader_bindings(repo_root: Path) -> dict:
    """
    Walk shaders/ for layout(binding=N) declarations.
    Returns {(namespace_guess, binding_int): [{file, line, snippet}]}.
    namespace_guess: 'ssbo' if buffer keyword present, 'ubo' if uniform block,
                     'image' if image2D/sampler, 'unknown' otherwise.
    """
    shaders_dir = repo_root / 'shaders'
    if not shaders_dir.exists():
        return {}

    result = {}
    for fpath in shaders_dir.rglob('*'):
        if fpath.suffix.lower() not in _SHADER_EXTS:
            continue
        # Skip fixtures directory (test-only probes)
        if 'fixtures' in fpath.parts:
            continue
        try:
            text = fpath.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue

        rel = str(fpath.relative_to(repo_root)).replace('\\', '/')
        if fpath.suffix == '.slang':
            import sys
            sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'shader_offline_build'))
            from slang_compile import resource_bindings
            for resource in resource_bindings(text):
                if resource['binding'] is None:
                    continue
                ns = resource['namespace']
                if resource['set'] != 0:
                    ns = f"vk_set{resource['set']}_{ns}"
                key = (ns, resource['binding'])
                result.setdefault(key, []).append({
                    'file': rel, 'line': resource['line'], 'set': resource['set'],
                    'snippet': f"{resource['name']} (Slang)",
                })
            continue
        for lineno, raw_line in enumerate(text.splitlines(), 1):
            comment_pos = raw_line.find('//')
            for m in _SHADER_BINDING_RE.finditer(raw_line):
                if comment_pos >= 0 and m.start() >= comment_pos:
                    continue  # match is inside a line comment
                binding_n = int(m.group(1))
                rest      = m.group(2).lower()
                snippet   = raw_line.strip()[:160]

                if 'buffer' in rest:
                    ns = 'ssbo'
                elif 'uniform' in rest and 'sampler' not in rest:
                    ns = 'ubo'
                elif any(kw in rest for kw in ('image2d', 'image3d', 'imagecube', 'uimage', 'iimage')):
                    ns = 'image'
                else:
                    ns = 'unknown'

                key = (ns, binding_n)
                result.setdefault(key, []).append({'file': rel, 'line': lineno, 'snippet': snippet})

    return result


# ---------------------------------------------------------------------------
# Live grep: C++ glBindBufferBase / glBindBufferRange
# ---------------------------------------------------------------------------

def _grep_cpp_bindings(repo_root: Path) -> dict:
    """
    Walk C++ source for glBindBufferBase/glBindBufferRange calls.
    Returns {(namespace, binding_int): [{file, line, snippet}]}.
    """
    result  = {}
    pattern = re.compile(
        r'glBindBuffer(?:Base|Range)\s*\(\s*(GL_\w+)\s*,\s*(\d+)',
        re.IGNORECASE,
    )

    for dirpath, dirnames, filenames in os.walk(repo_root):
        dirnames[:] = [d for d in dirnames
                       if d not in _EXCLUDE_DIRS and not d.startswith('build')]
        for fname in filenames:
            if Path(fname).suffix not in _CPP_EXTS:
                continue
            fpath = Path(dirpath) / fname
            try:
                text = fpath.read_text(encoding='utf-8', errors='replace')
            except OSError:
                continue
            if 'glBindBuffer' not in text:
                continue

            rel = str(fpath.relative_to(repo_root)).replace('\\', '/')
            for lineno, line in enumerate(text.splitlines(), 1):
                for m in pattern.finditer(line):
                    gl_target = m.group(1).upper()
                    binding_n = int(re.sub(r'[^\d]', '', m.group(2)))
                    ns        = _GL_TARGET_NS.get(gl_target, gl_target.lower())
                    key       = (ns, binding_n)
                    result.setdefault(key, []).append({
                        'file':    rel,
                        'line':    lineno,
                        'snippet': line.strip()[:160],
                    })

    return result


# ---------------------------------------------------------------------------
# Index builder
# ---------------------------------------------------------------------------

def build_index(repo_root: Path) -> dict:
    """
    Build unified binding index.
    Returns {
        'registry':        {(ns, int): [entry]},
        'shader_uses':     {(ns, int): [{file,line,snippet}]},
        'cpp_uses':        {(ns, int): [{file,line,snippet}]},
    }
    """
    return {
        'registry':    _parse_registry(repo_root),
        'shader_uses': _grep_shader_bindings(repo_root),
        'cpp_uses':    _grep_cpp_bindings(repo_root),
    }


# ---------------------------------------------------------------------------
# Query interface
# ---------------------------------------------------------------------------

def _key_for_ns_binding(ns, binding_n):
    """Return the canonical dict key."""
    return (ns, binding_n)


def _slots_in_ns(index, ns):
    """All binding numbers mentioned in ns across registry + shader_uses + cpp_uses."""
    slots = set()
    for (k_ns, k_n) in index['registry']:
        if k_ns == ns:
            slots.add(k_n)
    for (k_ns, k_n) in index['shader_uses']:
        if k_ns == ns:
            slots.add(k_n)
    for (k_ns, k_n) in index['cpp_uses']:
        if k_ns == ns:
            slots.add(k_n)
    return sorted(slots)


def _build_slot_summary(index, ns, n):
    """Build a summary dict for a single (ns, n) slot."""
    key  = (ns, n)
    reg  = index['registry'].get(key, [])
    sh   = index['shader_uses'].get(key, [])
    cpp  = index['cpp_uses'].get(key, [])

    documented = bool(reg)
    multi_binder = len(reg) > 1

    # Check for unregistered shader uses
    registered_shader_files = set()
    for r in reg:
        for ref in re.findall(r'shaders/[^\s,`()]+', r.get('shader_consumers', '')):
            registered_shader_files.add(ref.split(':')[0])

    unregistered_shader_uses = [
        s for s in sh
        if s['file'] not in registered_shader_files
    ]

    return {
        'namespace':               ns,
        'binding':                 n,
        'documented':              documented,
        'registry_entries':        reg,
        'shader_uses':             sh,
        'cpp_uses':                cpp,
        'multi_binder':            multi_binder,
        'unregistered_shader_uses': unregistered_shader_uses,
        'conflict_candidate':      not documented and (bool(sh) or bool(cpp)),
    }


def query_binding(
    repo_root: Path,
    binding:    int          = None,
    namespace:  str          = None,
    conflicts:  bool         = False,
    show_all:   bool         = False,
) -> dict:
    """
    Query binding index.
    Modes (priority order):
      binding=N [namespace=ns]  → look up specific slot
      conflicts=True            → slots with shader uses not in registry
      namespace=ns              → summary of all slots in that namespace
      (default)                 → high-level counts summary
    """
    index = build_index(repo_root)

    # Aggregate slot summaries per namespace
    all_ns = {'ubo', 'ssbo', 'texture', 'image'}
    all_ns.update(k_ns for k_ns, _ in index['registry'])
    all_ns.update(k_ns for k_ns, _ in index['shader_uses'])
    all_ns.update(k_ns for k_ns, _ in index['cpp_uses'])

    summary = {}
    for ns in sorted(all_ns):
        slots = _slots_in_ns(index, ns)
        slot_summaries = [_build_slot_summary(index, ns, n) for n in slots]
        summary[ns] = {
            'total_slots':          len(slots),
            'documented':           sum(1 for s in slot_summaries if s['documented']),
            'multi_binder':         sum(1 for s in slot_summaries if s['multi_binder']),
            'conflict_candidates':  sum(1 for s in slot_summaries if s['conflict_candidate']),
        }

    # Mode: specific binding lookup
    if binding is not None:
        namespaces_to_check = [namespace] if namespace else sorted(all_ns)
        hits = []
        for ns in namespaces_to_check:
            s = _build_slot_summary(index, ns, binding)
            if s['registry_entries'] or s['shader_uses'] or s['cpp_uses']:
                hits.append(s)
        if not hits:
            return {
                'query': f'binding={binding}',
                'error': f'No entries found for binding {binding}'
                         + (f' in namespace {namespace}' if namespace else ''),
                'summary': summary,
            }
        return {
            'query':  f'binding={binding}',
            'hits':   hits,
            'summary': summary,
        }

    # Mode: conflicts — shader uses not in registry
    if conflicts:
        conflict_list = []
        for ns in sorted(all_ns):
            for n in _slots_in_ns(index, ns):
                s = _build_slot_summary(index, ns, n)
                if s['conflict_candidate'] or s['unregistered_shader_uses']:
                    conflict_list.append(s)
        return {
            'query':     'conflicts',
            'conflicts': conflict_list,
            'summary':   summary,
        }

    # Mode: namespace filter
    if namespace:
        slots     = _slots_in_ns(index, namespace)
        slot_data = [_build_slot_summary(index, namespace, n) for n in slots]
        return {
            'query':   f'namespace={namespace}',
            'slots':   slot_data,
            'summary': summary,
        }

    # Default: summary only
    return {'summary': summary}
