# Depth-1 outline of one function body: the statements at the function's own brace depth, with line numbers.
# usage: py -3 .claude/ph_flow.py FILE 'signature regex'
import re, sys
BS = chr(92)
path, sig = sys.argv[1], sys.argv[2]
L = open(path, encoding='utf-8', errors='replace').read().replace('\r', '').split('\n')
s = next(i for i, l in enumerate(L) if re.search(sig, l))
inblock = [False]


def clean(l):
    out = ''
    i = 0
    n = len(l)
    while i < n:
        if inblock[0]:
            j = l.find('*/', i)
            if j < 0:
                return out
            inblock[0] = False
            i = j + 2
            continue
        if l.startswith('/*', i):
            inblock[0] = True
            i += 2
            continue
        if l.startswith('//', i):
            break
        c = l[i]
        if c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n and l[j] != q:
                j += 2 if l[j] == BS else 1
            out += q + q
            i = j + 1
            continue
        out += c
        i += 1
    return out


depth = 0
started = False
end = None
rows = []
for i in range(s, len(L)):
    c = clean(L[i])
    if c.lstrip().startswith('#'):
        if started and depth == 1:
            rows.append((i + 1, 'PP ' + L[i].strip()[:100]))
        continue
    d0 = depth
    for ch in c:
        if ch == '{':
            depth += 1
            started = True
        elif ch == '}':
            depth -= 1
    if started and d0 == 1 and L[i].strip():
        rows.append((i + 1, L[i].strip()[:110]))
    if started and depth == 0:
        end = i + 1
        break
print('function', s + 1, end)
prev = None
for ln, t in rows:
    print(ln, t)
