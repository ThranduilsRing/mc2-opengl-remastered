# Which locals declared in one line range are read again in a later range of the same function?
# usage: py -3 .claude/ph_xuse.py FILE a-b,c-d,...   (1-based inclusive ranges, in execution order)
import re
import sys
L = open(sys.argv[1], encoding='utf-8', errors='replace').read().replace('\r', '').split('\n')
rngs = [tuple(int(x) for x in r.split('-')) for r in sys.argv[2].split(',')]
decl = re.compile(r'^\s*(?:static\s+)?(?:const\s+|constexpr\s+)?(?:unsigned\s+)?[A-Za-z_][\w:<>\*&,\s]*?[\s\*&]([A-Za-z_]\w*)\s*(?:=[^=]|;|\[|\()')


def code(line):
    line = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
    return re.sub(r'//.*', '', line)


for i, (a, b) in enumerate(rngs):
    names = {}
    for ln in range(a, b + 1):
        c = code(L[ln - 1])
        m = decl.match(c)
        if m and not c.lstrip().startswith(('return', 'else', 'goto', 'delete', 'case')):
            names.setdefault(m.group(1), ln)
    out = []
    for n, ln in names.items():
        later = [j for j, (c, d) in enumerate(rngs) if j > i and any(re.search(r'\b' + re.escape(n) + r'\b', code(L[k - 1])) for k in range(c, d + 1))]
        if later:
            out.append('%s(L%d)->phases %s' % (n, ln, later))
    print('range %d-%d: %s' % (a, b, '; '.join(out) if out else '-'))
