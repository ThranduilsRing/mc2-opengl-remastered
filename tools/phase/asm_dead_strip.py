# Removes inline-asm preprocessor branches that the build invariants make unreachable, keeping the live branch's text
# untouched, and proves the preprocessed translation unit did not change.
#
# Invariants (CMakeLists.txt add_definitions): LINUX_BUILD defined, USE_ASSEMBLER_CODE=0.
# A conditional group is collapsed when (a) every branch condition is decidable from those two macros and (b) a
# branch that can never be taken contains inline asm. The collapsed group is replaced by the text of its live branch
# (or by nothing); nested groups in the live branch are processed the same way. Everything else is copied verbatim.
#
# usage: py -3 tools/phase/asm_dead_strip.py FILE [--tu SOURCE.cpp] [--apply] [--define NAME=VAL ...] [--all-dead]
#   --define NAME=VAL  one more macro the build fixes (e.g. USE_GOS_HEAP=1, which heap.h defines under LINUX_BUILD)
#   --all-dead         collapse every group with a branch that can never be taken, not only those holding inline asm
#   without --apply: dry run (prints the groups it would collapse and the line counts)
#   with    --apply: rewrite FILE, then compare `cl /EP` output of the TU before and after (non-blank lines, trailing
#            whitespace ignored). --tu names the TU to preprocess when FILE is a header (default: FILE itself).
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'scripts'))
import console_child  # noqa: E402  (windowless console children without a conhost each)
from configure_ninja import NINJA, under_vcvars, DEFAULT_TREE  # noqa: E402

KNOWN = {'LINUX_BUILD': 1, 'USE_ASSEMBLER_CODE': 0}
ALL_DEAD = False
ASM = re.compile(r'(?<![\w])(__asm|_asm|asm\s+volatile|__asm__)\b')


def tri(expr):
    """True / False / None for a preprocessor expression over the known macros (None: depends on something else)."""
    e = re.sub(r'defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)',
               lambda m: ('1' if (m.group(1) or m.group(2)) in KNOWN else 'UNK'), expr)
    e = re.sub(r'\b([A-Za-z_]\w*)\b',
               lambda m: m.group(1) if m.group(1) == 'UNK' else (str(KNOWN[m.group(1)]) if m.group(1) in KNOWN else 'UNK'), e)
    e = e.replace('&&', ' and ').replace('||', ' or ').replace('!', ' not ').replace('not =', '!=')
    outs = set()
    for v in (0, 1):
        try:
            # offline tool: folds a numeric preprocessor expression read from repo sources
            outs.add(bool(eval(e.replace('UNK', str(v)), {'__builtins__': {}}, {})))
        except Exception:
            return None
    return outs.pop() if len(outs) == 1 else None


def directive(line):
    s = line.strip()
    if not s.startswith('#'):
        return None, ''
    m = re.match(r'#\s*(\w+)\s*(.*)', s)
    return (m.group(1), m.group(2)) if m else (None, '')


def cond_value(kind, rest):
    if kind == 'if':
        return tri(rest)
    name = rest.split()[0] if rest.split() else ''
    if name in KNOWN:
        known_defined = True
        return known_defined if kind == 'ifdef' else (not known_defined)
    return None


def parse(lines, i, stop_kinds):
    """-> (items, next index, terminator kind). An item is ('line', idx) or ('cond', branches, endif_idx);
    a branch is (directive_idx, kind, rest, items)."""
    items = []
    while i < len(lines):
        kind, rest = directive(lines[i])
        if kind in ('if', 'ifdef', 'ifndef'):
            branches = []
            d_idx, d_kind, d_rest = i, kind, rest
            i += 1
            while True:
                sub, i, term = parse(lines, i, ('elif', 'else', 'endif'))
                branches.append((d_idx, d_kind, d_rest, sub))
                if term == 'endif':
                    items.append(('cond', branches, i - 1))
                    break
                d_idx, d_kind, d_rest = i - 1, term, directive(lines[i - 1])[1]
            continue
        if kind in stop_kinds:
            return items, i + 1, kind
        items.append(('line', i))
        i += 1
    return items, i, None


def flat(items):
    for it in items:
        if it[0] == 'line':
            yield it[1]
        else:
            for _d, _k, _r, sub in it[1]:
                yield from flat(sub)


def effective(branches):
    """Per-branch live status: True (always taken), False (never), None (undecidable)."""
    out, prior_unknown, prior_true = [], False, False
    for _d, kind, rest, _sub in branches:
        if prior_true:
            out.append(False)
            continue
        v = True if kind == 'else' else cond_value(kind if kind != 'elif' else 'if', rest)
        if v is None:
            out.append(None)
            prior_unknown = True
        elif v:
            out.append(None if prior_unknown else True)
            prior_true = prior_true or not prior_unknown
        else:
            out.append(False)
    return out


def emit(lines, items, report, out):
    for it in items:
        if it[0] == 'line':
            out.append(lines[it[1]])
            continue
        branches, endif = it[1], it[2]
        eff = effective(branches)
        has_dead_asm = any(e is False and (ALL_DEAD or any(ASM.search(re.sub(r'//.*', '', lines[j])) for j in flat(sub)))
                           for e, (_d, _k, _r, sub) in zip(eff, branches))
        if has_dead_asm and None not in eff:
            first, last = branches[0][0], endif
            report.append((first + 1, last + 1, ' / '.join(lines[b[0]].strip() for b in branches)))
            for e, (_d, _k, _r, sub) in zip(eff, branches):
                if e:
                    emit(lines, sub, report, out)
            continue
        for d, kind, rest, sub in branches:
            out.append(lines[d])
            emit(lines, sub, report, out)
        out.append(lines[endif])


def strip_file(text):
    nl = '\r\n' if text.count('\r\n') * 2 > text.count('\n') else '\n'
    lines = text.replace('\r\n', '\n').split('\n')
    items, _i, _t = parse(lines, 0, ())
    out, report = [], []
    emit(lines, items, report, out)
    return nl.join(out), report, nl


def find_target(rel):
    """(ninja object target, built) for the TU; a source no target builds borrows a sibling's compile flags."""
    out = subprocess.check_output([NINJA, '-C', DEFAULT_TREE, '-t', 'targets', 'all'], text=True,
                                  **console_child.child_kwargs(redirected=True))
    rel = rel.replace('\\', '/')
    names = [l.split(':')[0].replace('\\', '/') for l in out.splitlines() if l.split(':')[0].endswith('.obj')]
    key = '/' + rel.split('/', 1)[-1] + '.obj'
    exact = [n for n in names if n.endswith(key)] or [n for n in names if n.endswith('/' + rel.split('/')[-1] + '.obj')]
    if exact:
        return exact[0], True
    sib = [n for n in names if n.startswith('out/' + os.path.dirname(rel) + '/')]
    if not sib:
        raise SystemExit('no ninja object target (or sibling) for ' + rel)
    return sib[0], False


def preprocess(tu):
    """Normalised /EP output of the TU as the project compiles it (non-blank lines, trailing whitespace dropped)."""
    target, built = find_target(tu)
    out = subprocess.check_output([NINJA, '-C', DEFAULT_TREE, '-t', 'commands', target], text=True,
                                  **console_child.child_kwargs(redirected=True))
    cmd = [l for l in out.splitlines() if ' -c ' in l][-1]
    if not built:
        cmd = re.sub(r'-c\s+\S+$', lambda m: '-c ' + os.path.abspath(tu), cmd)
    cmd = re.sub(r' -c ', ' /EP /nologo -c ', cmd, 1)
    cmd = re.sub(r'/Fo\S+|-Fo\S+|/Fd\S+|-Fd\S+|/showIncludes|-showIncludes', '', cmd)
    with tempfile.NamedTemporaryFile(suffix='.i', delete=False) as t:
        dst = t.name
    rc = under_vcvars(cmd + ' > "' + dst + '" 2>nul')
    if rc:
        raise SystemExit('preprocess failed for ' + tu)
    with open(dst, encoding='utf-8', errors='replace') as f:
        lines = [l.rstrip() for l in f.read().replace('\r', '').split('\n')]
    os.unlink(dst)
    return [l for l in lines if l.strip()]


def main():
    global ALL_DEAD
    args = sys.argv[1:]
    apply = '--apply' in args
    ALL_DEAD = '--all-dead' in args
    defines = [args[i + 1] for i, a in enumerate(args) if a == '--define']
    for d in defines:
        name, _eq, val = d.partition('=')
        KNOWN[name] = int(val or '1')
    tu = None
    if '--tu' in args:
        tu = args[args.index('--tu') + 1]
    path = [a for a in args if not a.startswith('--') and a != tu and a not in defines][0]
    text = open(path, 'rb').read().decode('utf-8', errors='surrogateescape')
    new, report, nl = strip_file(text)
    old_lines = text.count('\n') + 1
    for a, b, d in report:
        print('collapse L%d-L%d (%d lines): %s' % (a, b, b - a + 1, d))
    print('%s: %d -> %d lines, %d group(s)' % (path, old_lines, new.count('\n') + 1, len(report)))
    if not apply or not report:
        return
    tu = tu or path
    before = preprocess(tu)
    open(path, 'wb').write(new.encode('utf-8', errors='surrogateescape'))
    after = preprocess(tu)
    if before != after:
        for i, (x, y) in enumerate(zip(before, after)):
            if x != y:
                print('PREPROCESSED OUTPUT DIFFERS at line %d:\n  before: %s\n  after : %s' % (i, x[:160], y[:160]))
                break
        else:
            print('PREPROCESSED OUTPUT DIFFERS: length %d vs %d' % (len(before), len(after)))
        sys.exit(1)
    print('PASS: cl /EP of %s identical before and after (%d non-blank lines)' % (tu, len(before)))


main()
