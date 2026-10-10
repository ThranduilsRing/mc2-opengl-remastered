# Phase-helper split of one oversized function: moves each lexical phase of the body, verbatim, into a
# helper that takes the small shared-state struct(s) holding the locals that cross phases; the original
# function keeps only the glue (calls, early-outs, scoping).
# usage: py -3 tools/phase/ph_split.py tools/phase/proofs/ph_spec_<name>.py
# Reads the text at the spec's BASE commit (default HEAD) so it is re-runnable; writes the working file.
#
# Spec fields:
#   BASE, SRC, FUNC_SIG        commit to start from, source file, regex of the function's signature line
#   STRUCTS                    {frame id: struct name}; FRAME_DOC: comment above the structs
#   MEMBERS                    [(name, type[, frame id[, lines]])] locals that live in a struct (default frame 'f');
#                              lines limits which original declaration lines are lifted (same name may be a local elsewhere)
#   HOIST                      first lines of function-local types moved to file scope unchanged
#   EDITS                      {line: (old, new)} statements the declaration-lift rule cannot rewrite
#   ORCH_BEFORE                {line: text} glue lines inserted before that original line
#   PHASES                     [(helper name, first line, last line, doc[, #ifdef guard])]
#   METHOD_CLASS               helpers are members of this class (default: file-static functions)
#   PARAMS                     [(name, type)] parameters of the original function; each helper takes those it uses
#   SPLITS / INTERNAL_INCLUDES   [(first phase, path, head comment lines)] helper pieces written to further files;
#                              the include lines every piece (and the frame header) starts with
#   PREFIX_EDITS               {line: text} comment lines before the function to rewrite (the file header)
#   a phase tuple may end in {'file': path}: its helper is written to that file (heads in FILE_HEADS)
#   a phase tuple may end in {'exits': True} or {'exits': '<value>'} (every exit is `return <value>;`): the helper returns bool (true = the original function returned) and
#                              its bare `return;` statements become `return true;`
#   PRIMARY_INCLUDE / PRIMARY_INCLUDE_AFTER   an include line added to the source file after the first line starting with the second
#   ONLY                       {frame id: [phase names]} frames only those phases may alias (name clashes)
#   FRAME_METHODS              single frame: helpers that use its members are methods of the struct (no alias lines);
#                              a phase tuple may end in {'free': True} to stay a file-static function
#   FRAME_GLOBAL               keep the frame struct out of an anonymous namespace (block-scope externs in its methods)
#   ALIAS_PER_LINE             alias declarations packed per line (keeps identical alias runs short)
#   {'exits': 'continue'}: the phase's `continue;` statements leave the enclosing loop iteration instead; the orchestrator call is `if (helper()) continue;`
#   FILE_NAMESPACE             namespace wrapped around the helper blocks written to other files (after their includes)
#   a phase tuple may end in {'dedent': prefix} to strip a different indent from that helper body
#   INDENT, DEDENT, ORCH_DOC   indentation unit, prefix stripped from helper bodies, comment above function
import importlib.util
import re
import os
import subprocess
import sys

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'scripts'))
import console_child  # noqa: E402  (windowless console children without a conhost each)

MEMBER_COMMENTS = {}
MEMBER_GUARDS = {}    # member name -> #ifdef guard around its struct declaration
MEMBER_LINES = {}     # member name -> original lines whose declaration is lifted (absent: any matching line)


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


def ident(name):
    return re.match(r'\w+', name).group(0)


def member_rx(typ, name):
    t = re.escape(typ).replace(r'\ ', r'\s*')
    return re.compile(r'^(\s*)' + t + r'\s*' + re.escape(name) + r'(?!\w)(\s*=\s*[^;,]+)?\s*;\s*(.*)$')


def transform(line, members, edits, lineno):
    """Apply the spec's declaration-lift rules to one original line; None means the line is dropped."""
    if lineno in edits:
        old, new = edits[lineno]
        assert old in line, (lineno, old)
        return line.replace(old, new)
    for name, typ, _fr in members:
        if name in MEMBER_LINES and lineno not in MEMBER_LINES[name]:
            continue
        m = member_rx(typ, name).match(line)
        if not m:
            continue
        ind, init, rest = m.group(1), m.group(2), m.group(3)
        out = ''
        if init:
            out = ident(name) + ' ' + init.strip() + ';'
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
    raw = subprocess.check_output(['git', 'show', getattr(S, 'BASE', 'HEAD') + ':' + S.SRC], **console_child.child_kwargs(redirected=True))
    crlf = raw.count(b'\r\n') * 2 > raw.count(b'\n')
    text = raw.decode('utf-8')
    if crlf:
        text = text.replace('\r\n', '\n')
    L = text.split('\n')
    fs = next(i for i, l in enumerate(L) if re.search(S.FUNC_SIG, l))           # 0-based signature line
    fe = next(i for i in range(fs, len(L)) if L[i].rstrip('\r') == '}')           # closing brace
    print('function', fs + 1, fe + 1, 'CRLF' if crlf else 'LF')
    structs = S.STRUCTS if hasattr(S, 'STRUCTS') else {'f': S.STRUCT}
    members = [(m[0], m[1], m[2] if len(m) > 2 else 'f') for m in S.MEMBERS]
    for m in S.MEMBERS:
        if len(m) > 3:
            MEMBER_LINES[m[0]] = set(m[3])
        if len(m) > 4 and m[4]:
            MEMBER_GUARDS[m[0]] = m[4]
    edits = dict(S.EDITS)
    method = getattr(S, 'METHOD_CLASS', None)
    indent = getattr(S, 'INDENT', '    ')
    dedent = getattr(S, 'DEDENT', '')
    hoist = {}
    for a in getattr(S, 'HOIST', []):
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
    # every phase must be brace-balanced (a cut inside a block would not compile)
    for (pn, a, b, *_r) in ph:
        depth = 0
        pp = 0
        for ln in range(a, b + 1):
            c = strip_code(L[ln - 1])
            if c.lstrip().startswith('#'):
                d = re.match(r'#\s*(\w+)', c.lstrip()).group(1)
                pp += 1 if d in ('if', 'ifdef', 'ifndef') else (-1 if d == 'endif' else 0)
                continue
            depth += c.count('{') - c.count('}')
        assert depth == 0, 'phase %s (lines %d-%d) is not brace-balanced (%+d)' % (pn, a, b, depth)
        assert pp == 0, 'phase %s (lines %d-%d) splits a preprocessor conditional (%+d)' % (pn, a, b, pp)

    def body_for(a, b, dedent=dedent):
        out = []
        for ln in range(a, b + 1):
            if ln in hoisted_lines:
                continue
            t = transform(L[ln - 1], members, edits, ln)
            if t is not None:
                if dedent and t.startswith(dedent):
                    t = t[len(dedent):]
                out.append(t)
        return out

    only = getattr(S, 'ONLY', {})

    def used_members(lines, phase):
        code = '\n'.join(strip_code(x) for x in lines)
        return [(n, fr) for n, _t, fr in members
                if (fr not in only or phase in only[fr]) and re.search(r'\b' + ident(n) + r'\b', code)]

    splits = list(getattr(S, 'SPLITS', []))
    if hasattr(S, 'SPLIT_AT'):
        splits.append((S.SPLIT_AT, S.SPLIT_SRC, S.SPLIT_HEAD))
    internal = list(getattr(S, 'INTERNAL_INCLUDES', ['#include "mission_internal.h"']))
    bodies = {p[1]: body_for(p[1], p[2], next((r for r in p[4:] if isinstance(r, dict)), {}).get('dedent', dedent)) for p in ph}
    frame_methods = getattr(S, 'FRAME_METHODS', False)
    per_line = getattr(S, 'ALIAS_PER_LINE', 1)
    info = []
    FILE_BLOCKS = {}
    for (name, a, b, doc, *rest) in ph:
        guard = next((r for r in rest if isinstance(r, str)), None)
        opts = next((r for r in rest if isinstance(r, dict)), {})
        body = bodies[a]
        used = used_members(body, name)
        frames = [fid for fid in structs if any(fr == fid for _n, fr in used)]
        as_method = frame_methods and bool(used) and not opts.get('free')
        exits = bool(opts.get('exits'))
        exit_val = opts['exits'] if isinstance(opts.get('exits'), str) else ''
        if exits:
            stmt_rx = r'\bcontinue\s*;' if exit_val == 'continue' else r'\breturn' + ((' ' + re.escape(exit_val)) if exit_val else '') + r'\s*;'
            body = [re.sub(stmt_rx, 'return true;', x) for x in body]
            body = body + [indent + 'return false;']
        code = '\n'.join(strip_code(x) for x in body)
        uparams = [(n, t) for n, t in getattr(S, 'PARAMS', []) if re.search(r'\b' + n + r'\b', code)]
        info.append(dict(exits=exits, exit_val=exit_val, file=opts.get('file'), name=name, a=a, doc=doc, guard=guard, body=body, used=used, frames=frames, as_method=as_method, uparams=uparams))
    H = []
    anon = not method and not getattr(S, 'FRAME_GLOBAL', False)
    if anon:
        H.append('namespace {')
        H.append('')
    for a in sorted(hoist):
        for ln in range(a, hoist[a] + 1):
            s = L[ln - 1]
            H.append(s[4:] if s.startswith('    ') else s)
        H.append('')
    H.append('\x00FRAME_BEGIN')
    H += S.FRAME_DOC.split(chr(10)) if structs else []
    for fid, sname in structs.items():
        H.append('struct ' + sname + ' {')
        for n, t, fr in members:
            if fr == fid:
                g = MEMBER_GUARDS.get(n)
                if g:
                    H.append('#ifdef ' + g)
                H.append(indent + t + ' ' + n + ';' + (('  ' + MEMBER_COMMENTS[n]) if n in MEMBER_COMMENTS else ''))
                if g:
                    H.append('#endif')
        meths = [i for i in info if i['as_method']]
        if meths:
            H.append('')
        for i in meths:
            if i['guard']:
                H.append('#ifdef ' + i['guard'])
            H.append(indent + 'void %s(%s);' % (i['name'], ', '.join((t if re.search(r'\b' + n + r'\b', t) else '%s %s' % (t, n)) for n, t in i['uparams'])))
            if i['guard']:
                H.append('#endif')
        H.append('};')
        H.append('')
    H.append('\x00FRAME_END')
    if anon:
        H.append('}  // namespace')
    calls = {}
    decls = []
    for i in info:
        name, guard, used, frames = i['name'], i['guard'], i['used'], i['frames']
        for sk, (sn, _sp, _sh) in enumerate(splits):
            if name == sn:
                H.append('\x00SPLIT%d' % sk)
        blk = []
        blk.append('')
        for d in i['doc'].split('\n'):
            blk.append('// ' + d)
        if guard:
            blk.append('#ifdef ' + guard)
        params = ', '.join([(t if re.search(r'\b' + n + r'\b', t) else '%s %s' % (t, n)) for n, t in i['uparams']] +['%s& %s' % (structs[fid], fid) for fid in frames])
        if i['as_method']:
            mparams = ', '.join((t if re.search(r'\b' + n + r'\b', t) else '%s %s' % (t, n)) for n, t in i['uparams'])
            blk.append('void %s::%s(%s) {' % (structs['f'], name, mparams))
            calls[i['a']] = 'f.%s(%s);' % (name, ', '.join(n for n, _t in i['uparams']))
        else:
            head = ('void %s::%s(%s) {' if method else 'static void %s%s(%s) {') % ((method, name, params) if method else ('', name, params))
            if not method and i['file']:
                head = head.replace('static ', '', 1)      # defined in another file: external linkage, declared in the shared header
            if i['exits']:
                head = head.replace('void ', 'bool ', 1)
            blk.append(head)
            decls.append('%s %s(%s);' % ('bool' if i['exits'] else 'void', name, params))
            for k in range(0, len(used), per_line):
                blk.append(indent + ' '.join('auto& %s = %s.%s;' % (ident(n), fr, ident(n)) for n, fr in used[k:k + per_line]))
            calls[i['a']] = '%s(%s);' % (name, ', '.join([n for n, _t in i['uparams']] + frames))
            if i['exits']:
                calls[i['a']] = 'if (' + calls[i['a']][:-1] + ') ' + ('continue;' if i['exit_val'] == 'continue' else 'return' + (' ' + i['exit_val'] if i['exit_val'] else '') + ';')
        blk += i['body']
        blk.append('}')
        if guard:
            blk.append('#endif')
        (FILE_BLOCKS.setdefault(i['file'], []) if i['file'] else H).extend(blk)
    H.append('')
    O = []
    in_ph = {}
    for (name, a, b, *_r) in ph:
        for ln in range(a, b + 1):
            in_ph[ln] = a

    def before(ln):
        v = S.ORCH_BEFORE[ln]
        return v if v[:1] in ' \t' else indent + v

    for ln in range(fs + 1, fe + 2):
        if ln in hoisted_lines:
            continue
        if ln in in_ph:
            if in_ph[ln] == ln:
                if ln in S.ORCH_BEFORE:
                    O.append(before(ln))
                lead = re.match(r'\s*', L[ln - 1]).group(0) if not L[ln - 1].startswith('#') else indent
                O.append(lead + calls[ln])
            continue
        if ln in S.ORCH_BEFORE:
            O.append(before(ln))
        t = transform(L[ln - 1], members, edits, ln)
        if t is None:
            continue
        if t.strip() == '' and O and O[-1].strip() == '':
            continue
        O.append(t)

    def is_call(x):
        return re.match(r'^\s*[\w.]+\(.*\);$', x) is not None or (bool(structs) and re.match(r'^\s*(%s) \w+;$' % '|'.join(structs.values()), x) is not None)
    O = [x for i, x in enumerate(O) if not (x.strip() == '' and 0 < i < len(O) - 1 and is_call(O[i - 1]) and is_call(O[i + 1]))]
    for ln, txt in getattr(S, 'PREFIX_EDITS', {}).items():
        assert ln - 1 < fs, 'PREFIX_EDITS line %d is not before the function' % ln
        L[ln - 1] = txt
    frame_lines = []
    if 'FRAME_HEADER' in S.__dict__:
        i0, i1 = H.index('\x00FRAME_BEGIN'), H.index('\x00FRAME_END')
        frame_lines = H[i0 + 1:i1]
        H = H[:i0] + H[i1 + 1:]
    else:
        H = [x for x in H if x not in ('\x00FRAME_BEGIN', '\x00FRAME_END')]
    pieces = []
    for sk in reversed(range(len(splits))):
        mk = '\x00SPLIT%d' % sk
        if mk in H:
            k = H.index(mk)
            pieces.append(H[k + 1:])
            H = H[:k]
    pieces.reverse()                      # cut from the back, so pieces[i] is the text after split i
    if 'FRAME_HEADER' in S.__dict__:
        guard_name = re.sub(r'\W', '_', S.FRAME_HEADER).upper()
        hdr = ['// ' + S.FRAME_HEADER_DOC, '#ifndef ' + guard_name, '#define ' + guard_name, ''] + internal + [''] + frame_lines + ['#endif  // ' + guard_name]
        open(S.FRAME_HEADER, 'wb').write(('\n'.join(hdr) + '\n').encode('utf-8'))
        inc = '#include "%s"' % S.FRAME_HEADER.split('/')[-1]
        j = next(k for k, l in enumerate(L[:fs]) if l.startswith(internal[0]))
        L = L[:j + 1] + [inc] + L[j + 1:]
        fs += 1
        fe += 1
    if hasattr(S, 'PRIMARY_INCLUDE'):
        j = next(k for k, l in enumerate(L[:fs]) if l.startswith(S.PRIMARY_INCLUDE_AFTER))
        L = L[:j + 1] + [S.PRIMARY_INCLUDE] + L[j + 1:]
        fs += 1
        fe += 1
    fns = getattr(S, 'FILE_NAMESPACE', None)
    ns_open = ['namespace ' + fns + ' {', ''] if fns else []
    ns_close = ['', '}  // namespace ' + fns] if fns else []
    for path, body in FILE_BLOCKS.items():
        extra_inc = internal + (['#include "%s"' % S.FRAME_HEADER.split('/')[-1]] if hasattr(S, 'FRAME_HEADER') else [])
        open(path, 'wb').write(('\n'.join(list(S.FILE_HEADS[path]) + extra_inc + [''] + ns_open + body + ns_close) + '\n').encode('utf-8'))
    for (sn, sp, sh), body in zip(splits, pieces):
        extra_inc = internal + (['#include "%s"' % S.FRAME_HEADER.split('/')[-1]] if hasattr(S, 'FRAME_HEADER') else [])
        open(sp, 'wb').write(('\n'.join(list(sh) + extra_inc + [''] + ns_open + body + ns_close) + '\n').encode('utf-8'))
    new = L[:fs] + H + (S.ORCH_DOC.split('\n') if getattr(S, 'ORCH_DOC', '') else [])
    new = new + O + L[fe + 1:]
    out = '\n'.join(new)
    if crlf:
        out = out.replace('\n', '\r\n')
    open(S.SRC, 'wb').write(out.encode('utf-8'))
    decl_path = os.path.splitext(sys.argv[1])[0] + '.decls.txt'   # the member declarations to add to the class header
    open(decl_path, 'w').write('\n'.join(decls) + '\n')
    print('lines', len(L), '->', len(new), ' orchestrator', len(O), ' helpers', len(ph))


main()
