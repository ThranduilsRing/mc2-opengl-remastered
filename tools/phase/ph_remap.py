# Re-targets a phase spec at a newer version of its source file when the only change is inserted lines
# (e.g. restored comments): every line number in the spec is mapped through a line diff, and a block of inserted
# lines directly above a phase's first line is taken into that phase. BASE is set to the new revision.
# usage: py -3 tools/phase/ph_remap.py SPEC.py NEW_REV
#   writes SPEC.py in place (deleted / replaced lines are fine as long as nothing in the spec points at them); refuses when the new file changes a line the spec points at.
import difflib
import importlib.util
import re
import subprocess
import sys

import os
sys.path.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'scripts'))
import console_child  # noqa: E402  (windowless console children without a conhost each)

spec_path, new_rev = sys.argv[1], sys.argv[2]
text = open(spec_path, encoding='utf-8', newline='').read()
crlf = '\r\n' in text
text = text.replace('\r\n', '\n')
sp = importlib.util.spec_from_file_location('spec', spec_path)
S = importlib.util.module_from_spec(sp)
sp.loader.exec_module(S)


def blob(rev):
    out = subprocess.check_output(['git', 'show', rev + ':' + S.SRC], **console_child.child_kwargs(redirected=True))
    return out.decode('utf-8').replace('\r\n', '\n').split('\n')


old, new = blob(S.BASE), blob(new_rev)
m = difflib.SequenceMatcher(None, old, new, autojunk=False)
mapping = {}
inserted_above = {}            # old line (1-based) -> number of inserted lines directly above its new position
for tag, i1, i2, j1, j2 in m.get_opcodes():
    if tag == 'equal':
        for k in range(i2 - i1):
            mapping[i1 + k + 1] = j1 + k + 1
    elif tag == 'insert':
        inserted_above[i1 + 1] = j2 - j1
    elif tag in ('delete', 'replace'):
        pass                    # fine while nothing in the spec points at a deleted line (checked below)
    else:
        sys.exit('unexpected diff opcode %s' % tag)


def mp(n):
    if n and n not in mapping:
        sys.exit('spec line %d was deleted or changed in the new revision' % n)
    return mapping[n] if n else n


def start(n):
    return mp(n) - inserted_above.get(n, 0)


lines_map = {}
edits = {mp(k): v for k, v in S.EDITS.items()}
before = {mp(k): v for k, v in S.ORCH_BEFORE.items()}
members = []
for t in S.MEMBERS:
    t = list(t)
    if len(t) > 3:
        t[3] = [mp(x) for x in t[3]]
    members.append(tuple(t))
phases = []
for p in S.PHASES:
    p = list(p)
    p[1], p[2] = start(p[1]), mp(p[2])
    phases.append(tuple(p))
hoist = [mp(h) for h in getattr(S, 'HOIST', [])]
prefix = {mp(k): v for k, v in getattr(S, 'PREFIX_EDITS', {}).items()}
print('mapped %d old lines; %d inserted block(s), %d lines' % (len(mapping), len(inserted_above), sum(inserted_above.values())))


def rewrite(name, value):
    global text
    a = re.search(r'^%s = ' % name, text, re.M)
    if not a:
        return
    # the assignment ends at the next top-level statement
    b = re.search(r'^(?=\S)', text[a.end():], re.M)
    end = a.end() + (b.start() if b else len(text) - a.end())
    # first non-continuation line after the start: find the next line that starts a new statement
    rest = text[a.end():]
    nxt = re.search(r'\n(?=[A-Za-z_#])', rest)
    end = a.end() + (nxt.start() + 1 if nxt else len(rest))
    text = text[:a.start()] + '%s = %s\n' % (name, value) + text[end:]


import pprint
text = re.sub(r"^BASE = '.*'$", "BASE = '%s'" % new_rev, text, count=1, flags=re.M)
for name, val in (('EDITS', edits), ('ORCH_BEFORE', before), ('MEMBERS', members), ('PHASES', phases)):
    if hasattr(S, name):
        rewrite(name, pprint.pformat(val, width=140, sort_dicts=False))
if hoist:
    rewrite('HOIST', repr(hoist))
if prefix:
    rewrite('PREFIX_EDITS', pprint.pformat(prefix, width=140, sort_dicts=False))
open(spec_path, 'w', encoding='utf-8', newline='').write(text.replace('\n', '\r\n') if crlf else text)
