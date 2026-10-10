# Inline-expansion proof for a ph_split.py phase split: expand every helper call in the new orchestrator back
# into the helper body and require the result to equal the original function body statement for statement
# (modulo the spec's listed declaration lifts / edits and the scaffolding), and require everything outside
# the function and the added helper block to be byte-identical.
# usage: py -3 .claude/ph_verify.py .claude/ph_spec_<name>.py
import collections
import importlib.util
import re
import os
import subprocess
import sys

sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'scripts'))
import console_child  # noqa: E402  (windowless console children without a conhost each)

sp = importlib.util.spec_from_file_location('spec', sys.argv[1])
S = importlib.util.module_from_spec(sp)
sp.loader.exec_module(S)
structs = S.STRUCTS if hasattr(S, 'STRUCTS') else {'f': S.STRUCT}
members = [(m[0], m[1], m[2] if len(m) > 2 else 'f') for m in S.MEMBERS]
method = getattr(S, 'METHOD_CLASS', None)
hoist_lines = getattr(S, 'HOIST', [])


def ident(name):
    return re.match(r'\w+', name).group(0)


def lines_of(raw):
    return raw.decode('utf-8').replace('\r\n', '\n').split('\n')


old = lines_of(subprocess.check_output(['git', 'show', getattr(S, 'BASE', 'HEAD') + ':' + S.SRC], **console_child.child_kwargs(redirected=True)))
new = lines_of(open(S.SRC, 'rb').read())


def fn_range(L, sig):
    s = next(i for i, l in enumerate(L) if re.search(sig, l))
    e = next(i for i in range(s, len(L)) if L[i].rstrip('\r') == '}')
    return s, e


os_, oe = fn_range(old, S.FUNC_SIG)
ns_, ne = fn_range(new, S.FUNC_SIG)
fail = []

# 1. outside the function (and the helper block inserted before it): prefix and suffix identical
if old[:os_] != new[:os_]:
    fail.append('text before the function changed')
if old[oe + 1:] != new[ne + 1:]:
    fail.append('text after the function changed')

# 2. helpers: name -> body with alias lines dropped
one_alias = r'auto& \w+ = (?:%s)\.\w+;' % '|'.join(structs)
alias_rx = re.compile(r'^\s*' + one_alias + r'(?:\s+' + one_alias + r')*$')
helpers = {}
for (name, a, b, doc, *_r) in S.PHASES:
    if method:
        sig_rx = re.compile(r'^void ' + method + '::' + name + r'\(.*\) \{$')
    else:
        sig_rx = re.compile(r'^(?:static void ' + name + r'\(.*\)|void \w+::' + name + r'\(\)) \{$')
    i = next((k for k, l in enumerate(new) if sig_rx.match(l)), None)
    if i is None:
        fail.append('helper missing: ' + name)
        continue
    j = next(k for k in range(i, len(new)) if new[k] == '}')
    body = new[i + 1:j]
    while body and alias_rx.match(body[0]):
        body.pop(0)
    for x in body:
        if alias_rx.match(x):
            fail.append('alias line inside body of ' + name)
    helpers[name] = body

# 3. expand the orchestrator
orch = new[ns_ + 1:ne]
fr = '|'.join(structs)
call_rx = re.compile(r'^\s*(?:f\.)?(\w+)\([\w, ]*\);$')
decl_rx = re.compile(r'^\s*(%s) \w+;$' % '|'.join(structs.values()))
expanded = []
frame_decls = 0
used_calls = []
for x in orch:
    m = call_rx.match(x)
    if m and m.group(1) in helpers:
        used_calls.append(m.group(1))
        expanded += helpers[m.group(1)]
    elif decl_rx.match(x):
        frame_decls += 1
    else:
        expanded.append(x)
want_decls = sum(1 for v in S.ORCH_BEFORE.values() for x in v.split('\n') if decl_rx.match(x))
if frame_decls != want_decls:
    fail.append('frame declarations in the orchestrator: %d, spec lists %d' % (frame_decls, want_decls))
if sorted(used_calls) != sorted(h[0] for h in S.PHASES):
    fail.append('call set differs from the phase list')
want_order = [h[0] for h in sorted(S.PHASES, key=lambda p: p[1])]
if used_calls != want_order:
    fail.append('helpers are not called in original order')

# 4. expected body from the original: hoisted types removed, listed declaration lifts / edits applied
hoisted = set()
hoist_text = []
for a in hoist_lines:
    b = next(i for i in range(a - 1, oe) if old[i].strip() == '};') + 1
    hoisted.update(range(a, b + 1))
    hoist_text.append([old[k - 1].strip() for k in range(a, b + 1)])

lifts = []
expected = []
for ln in range(os_ + 2, oe + 1):
    if ln in hoisted:
        continue
    t = old[ln - 1]
    if ln in S.EDITS:
        o, n = S.EDITS[ln]
        if o not in t:
            fail.append('edit anchor missing at line %d' % ln)
        t = t.replace(o, n)
        lifts.append((ln, 'edit', old[ln - 1].strip()[:70], t.strip()[:70]))
    else:
        for name, typ, _fr in members:
            tp = re.escape(typ).replace(r'\ ', r'\s*')
            m = re.match(r'^(\s*)' + tp + r'\s*' + re.escape(name) + r'(?!\w)(\s*=\s*[^;,]+)?\s*;\s*(.*)$', t)
            if m:
                init, rest = m.group(2), m.group(3)
                out = (ident(name) + ' ' + init.strip() + ';') if init else ''
                if rest and not rest.startswith('//'):
                    out = (out + ' ' + rest) if out else rest
                elif rest and out:
                    out += '  ' + rest
                lifts.append((ln, 'lift', old[ln - 1].strip()[:70], out.strip()[:70] or '(declaration only)'))
                t = (m.group(1) + out) if out else None
                break
    if t is not None:
        expected.append(t)


def norm(seq):
    return [x.strip() for x in seq if x.strip()]


a, b = norm(expected), norm(expanded)
if a != b:
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            fail.append('first divergence at statement %d:\n   orig: %s\n   new : %s' % (i, x[:120], y[:120]))
            break
    else:
        fail.append('length differs: orig %d vs expanded %d' % (len(a), len(b)))

# 5. hoisted types present, in order, at file scope
newn = norm(new)
for ht in hoist_text:
    ht = [x.strip() for x in ht if x.strip()]
    if not any(newn[i:i + len(ht)] == ht for i in range(len(newn))):
        fail.append('hoisted type block not found verbatim: ' + ht[0])

# 6. scaffolding census over the old function vs the new helper block + function
co = collections.Counter(norm(old[os_:oe + 1]))
cn = collections.Counter(norm(new[os_:ne + 1]))
only_new = cn - co
only_old = co - cn
print('inline expansion: %d statements, helpers %d, orchestrator lines %d' % (len(b), len(helpers), len(orch)))
print('declaration lifts / edits applied (%d):' % len(lifts))
for ln, k, o, n in lifts:
    print('  L%-5d %-4s %s  =>  %s' % (ln, k, o, n))
print('scaffolding only in new (%d distinct):' % len(only_new))
for x in sorted(only_new):
    print('   +', x[:110], ('x%d' % only_new[x]) if only_new[x] > 1 else '')
print('statements only in old (%d distinct, all lifted declarations / hoisted-type lines):' % len(only_old))
for x in sorted(only_old):
    print('   -', x[:110], ('x%d' % only_old[x]) if only_old[x] > 1 else '')
if fail:
    print('FAIL')
    for f in fail:
        print('  ', f)
    sys.exit(1)
print('PASS: inline expansion of the new function equals the original body')
