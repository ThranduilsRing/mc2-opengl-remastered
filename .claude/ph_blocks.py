# Brace-matched blocks of one function: for every `{` opening at the given depth inside the function, print its
# line, the matching `}` line and the first line of the comment run directly above it.
# usage: py -3 .claude/ph_blocks.py FILE 'signature regex' DEPTH [first_line last_line]
import re
import sys

path, sig, depth = sys.argv[1], sys.argv[2], int(sys.argv[3])
lo = int(sys.argv[4]) if len(sys.argv) > 4 else 0
hi = int(sys.argv[5]) if len(sys.argv) > 5 else 10 ** 9
L = open(path, encoding='utf-8', errors='replace').read().replace('\r', '').split('\n')
s = next(i for i, l in enumerate(L) if re.search(sig, l))


def code(line):
    line = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
    line = re.sub(r"'(?:[^'\\]|\\.)*'", "''", line)
    return re.sub(r'//.*', '', line)


d = 0
started = False
stack = []
out = []
for i in range(s, len(L)):
    c = code(L[i])
    if c.lstrip().startswith('#'):
        continue
    for ch in c:
        if ch == '{':
            d += 1
            started = True
            stack.append((i + 1, d))
        elif ch == '}':
            ln, dd = stack.pop()
            d -= 1
            if dd == depth and lo <= ln <= hi:
                j = ln - 1
                k = j
                while k - 1 >= 0 and L[k - 1].strip().startswith('//'):
                    k -= 1
                out.append((ln, i + 1, k + 1 if k < j else ln, L[ln - 1].strip()[:60]))
    if d == 0 and out is not None and stack == [] and i > s and any(True for _ in [0]) and started:
        break
for ln, end, cm, txt in out:
    print('open L%d close L%d comment-from L%d  %s' % (ln, end, cm, txt))
