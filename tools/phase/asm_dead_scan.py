# Lists every inline-asm site (__asm / _asm / asm volatile) and whether the preprocessor region holding it is dead
# under the build invariants (LINUX_BUILD defined, USE_ASSEMBLER_CODE=0). Regions guarded by anything else are "unknown".
# usage: py -3 tools/phase/asm_dead_scan.py FILE...
import re
import sys
# eval() below only folds a numeric preprocessor expression read from repo sources; this is an offline audit tool.

KNOWN = {'LINUX_BUILD': 1, 'USE_ASSEMBLER_CODE': 0}
ASM = re.compile(r'(?<![\w])(__asm|_asm|asm\s+volatile|__asm__)\b')


def ev(expr):
    """True/False/None for a #if / #elif expression over the known macros."""
    e = re.sub(r'defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)',
               lambda m: '1' if (m.group(1) or m.group(2)) in KNOWN else '?', expr)
    e = re.sub(r'\b([A-Za-z_]\w*)\b', lambda m: str(KNOWN[m.group(1)]) if m.group(1) in KNOWN else '?', e)
    if '?' in e:
        return None
    try:
        return bool(eval(e.replace('&&', ' and ').replace('||', ' or ').replace('!', ' not ')))
    except Exception:
        return None


for path in sys.argv[1:]:
    L = open(path, encoding='utf-8', errors='replace').read().replace('\r', '').split('\n')
    stack = []          # entries: [state, taken_before, header_line]; state True/False/None(unknown)
    sites = []
    in_block_comment = False
    for n, line in enumerate(L, 1):
        s = line.strip()
        if in_block_comment:
            if '*/' in s:
                in_block_comment = False
            continue
        if s.startswith('/*') and '*/' not in s:
            in_block_comment = True
            continue
        if s.startswith('#'):
            d = re.match(r'#\s*(\w+)\s*(.*)', s)
            k, rest = d.group(1), d.group(2)
            if k in ('ifdef', 'ifndef', 'if'):
                if k == 'if':
                    v = ev(rest)
                else:
                    name = rest.split()[0] if rest.split() else ''
                    v = (name in KNOWN) if k == 'ifdef' else (name not in KNOWN)
                    if name not in KNOWN:
                        v = None
                stack.append([v, v is True, n])
            elif k == 'elif' and stack:
                v = ev(rest)
                top = stack[-1]
                if top[1]:
                    top[0] = False
                else:
                    top[0] = v
                    top[1] = top[1] or v is True
            elif k == 'else' and stack:
                top = stack[-1]
                top[0] = None if top[0] is None else (not top[1])
                top[1] = True
            elif k == 'endif' and stack:
                stack.pop()
            continue
        code = re.sub(r'//.*', '', line)
        if ASM.search(code):
            states = [e[0] for e in stack]
            if any(x is False for x in states):
                status = 'DEAD'
            elif any(x is None for x in states):
                status = 'unknown-guard'
            else:
                status = 'LIVE'
            guards = ','.join(str(e[2]) for e in stack)
            sites.append((n, status, guards))
    if sites:
        by = {}
        for n, st, g in sites:
            by.setdefault((st, g), []).append(n)
        for (st, g), ns in by.items():
            print('%-34s %-14s guards@L[%s] asm sites: %d (L%d..L%d)' % (path, st, g, len(ns), ns[0], ns[-1]))
