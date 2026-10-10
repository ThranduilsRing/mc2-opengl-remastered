# Phase-helper split of one oversized function: moves each lexical phase of the body, verbatim, into a
# file-local helper that takes a small shared-state struct; the original function keeps only the glue.
# usage: py -3 .claude/ph_split.py .claude/ph_spec_<name>.py   (reads the committed text from git HEAD,
#        so it is re-runnable; writes the working file)
import importlib.util
import re
import subprocess
import sys


def load(path):
    sp = importlib.util.spec_from_file_location('spec', path)
    m = importlib.util.module_from_spec(sp)
    sp.loader.exec_module(m)
    return m


def strip_code(s):
    """Line with // comments and string/char literals blanked, for identifier scans."""
    s = re.sub(r'"(?:[^"\\]|\\.)*"', '""', s)
    s = re.sub(r"'(?:[^'\\]|\\.)*'", "''", s)
    return re.sub(r'//.*', '', s)


MEMBER_COMMENTS = {}


def member_rx(typ, name):
    t = re.escape(typ).replace(r'\ ', r'\s*')
    return re.compile(r'^(\s*)' + t + r'\s*' + name + r'\b(\s*=\s*[^;,]+)?\s*;\s*(.*)$')


def transform(line, members, edits, lineno):
    """Apply the spec's declaration-lift rules to one original line; None means the line is dropped."""
    if lineno in edits:
        old, new = edits[lineno]
        assert old in line, (lineno, old)
        return line.replace(old, new)
    for name, typ in members:
        m = member_rx(typ, name).match(line)
        if not m:
            continue
        ind, init, rest = m.group(1), m.group(2), m.group(3)
        out = ''
        if init:
            out = name + ' ' + init.strip() + ';'
        if rest:
            if rest.startswith('//'):
                if out:
                    out += '  ' + rest
                else:
                    MEMBER_COMMENTS[name] = rest
            else:
                out = (out + ' ' + rest) if out else rest
        if not out:
            return None
        return ind + out
    return line


def main():
    S = load(sys.argv[1])
    raw = subprocess.check_output(['git', 'show', getattr(S, 'BASE', 'HEAD') + ':' + S.SRC])
    L = raw.decode('utf-8').split('\n')
    fs = next(i for i, l in enumerate(L) if re.search(S.FUNC_SIG, l))           # 0-based signature line
    fe = next(i for i in range(fs, len(L)) if L[i].rstrip('\r') == '}')           # closing brace
    print('function', fs + 1, fe + 1)
    members = [(n, t) for n, t in S.MEMBERS]
    edits = dict(S.EDITS)
    # hoisted local types: from the `struct X {` line to the matching `};`
    hoist = {}
    for a in S.HOIST:
        b = next(i for i in range(a - 1, fe) if L[i].strip() == '};')
        hoist[a] = b + 1
    hoisted_lines = set()
    for a, b in hoist.items():
        hoisted_lines.update(range(a, b + 1))
    ph = list(S.PHASES)
    for (_n, a, b, *_r) in ph:
        assert fs + 1 < a <= b < fe + 1, (a, b)
    ph.sort(key=lambda p: p[1])
    for x, y in zip(ph, ph[1:]):
        assert x[2] < y[1], (x[0], y[0])
    names = [n for n, _t in members]

    def body_for(a, b):
        out = []
        for ln in range(a, b + 1):
            if ln in hoisted_lines:
                continue
            t = transform(L[ln - 1], members, edits, ln)
            if t is not None:
                out.append(t)
        return out

    def used_members(lines):
        code = '\n'.join(strip_code(x) for x in lines)
        return [n for n in names if re.search(r'\b' + re.escape(n) + r'\b', code)]

    bodies = {a: body_for(a, b) for (_n, a, b, *_r) in ph}
    H = []
    H.append('namespace {')
    H.append('')
    for a in sorted(hoist):
        for ln in range(a, hoist[a] + 1):
            s = L[ln - 1]
            H.append(s[4:] if s.startswith('    ') else s)
        H.append('')
    H.append(S.FRAME_DOC)
    H.append('struct ' + S.STRUCT + ' {')
    for n, t in members:
        H.append('    ' + t + ' ' + n + ';' + (('  ' + MEMBER_COMMENTS[n]) if n in MEMBER_COMMENTS else ''))
    H.append('};')
    H.append('')
    H.append('}  // namespace')
    calls = {}
    for (name, a, b, doc, *rest) in ph:
        guard = rest[0] if rest else None
        body = bodies[a]
        used = used_members(body)
        H.append('')
        for d in doc.split('\n'):
            H.append('// ' + d)
        if guard:
            H.append('#ifdef ' + guard)
        if used:
            H.append('static void %s(%s& %s) {' % (name, S.STRUCT, S.FRAME_VAR))
            for n in used:
                H.append('    auto& %s = %s.%s;' % (n, S.FRAME_VAR, n))
            calls[a] = '%s(%s);' % (name, S.FRAME_VAR)
        else:
            H.append('static void %s() {' % name)
            calls[a] = '%s();' % name
        H += body
        H.append('}')
        if guard:
            H.append('#endif')
    H.append('')
    # orchestrator
    O = []
    in_ph = {}
    for (name, a, b, *_r) in ph:
        for ln in range(a, b + 1):
            in_ph[ln] = a
    for ln in range(fs + 1, fe + 2):
        if ln in hoisted_lines:
            continue
        if ln in in_ph:
            if in_ph[ln] == ln:
                if ln in S.ORCH_BEFORE:
                    O.append('    ' + S.ORCH_BEFORE[ln])
                O.append('    ' + calls[ln])
            continue
        if ln in S.ORCH_BEFORE and ln not in in_ph:
            O.append('    ' + S.ORCH_BEFORE[ln])
        t = transform(L[ln - 1], members, edits, ln)
        if t is None:
            continue
        if t.strip() == '' and O and O[-1].strip() == '':
            continue
        O.append(t)
    # a blank line between two bare phase calls separates nothing
    def is_call(x):
        return re.match(r'^    \w+\(.*\);$', x) is not None or x.startswith('    ' + S.STRUCT + ' ')
    O = [x for i, x in enumerate(O) if not (x.strip() == '' and 0 < i < len(O) - 1 and is_call(O[i - 1]) and is_call(O[i + 1]))]
    # blank lines between consecutive calls for readability are left to the source blanks; drop blanks that
    # now separate nothing (leading/trailing)
    new = L[:fs] + H + (S.ORCH_DOC.split(chr(10)) if S.ORCH_DOC else [])
    new = new + O + L[fe + 1:]
    open(S.SRC, 'wb').write('\n'.join(new).encode('utf-8'))
    print('lines', len(L), '->', len(new), ' orchestrator', len(O), ' helpers', len(ph))


main()
