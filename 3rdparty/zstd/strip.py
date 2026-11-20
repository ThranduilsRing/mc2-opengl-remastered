"""Reproduces 3rdparty/zstd/zstd.c from an unpacked upstream zstd-1.5.7 tree.

    py -3 -I strip.py <zstd-1.5.7 dir> <out zstd.c>

1. combine.py (upstream build/single_file_libs) on zstd-in.c with the decompress/, dictBuilder/, zstdmt_compress,
   threading and pool includes removed and ZSTD_MULTITHREAD off (compressor only; fse_decompress.c stays, the
   compressor needs it).
2. Every comment becomes one space (string and character literals are skipped); blank lines and trailing blanks go.
3. The upstream BSD/GPLv2 licence header (lib/zstd.h) is prepended verbatim.
4. One patch: after the upstream "#undef ZSTD_isError" the function stays renamed (see zstd_prefix.h).
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

DROP = ('dictBuilder/', 'decompress/zstd', 'decompress/huf', 'zstdmt_compress', 'common/threading', 'common/pool')

def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1]); i = j + 1
        elif src.startswith('/*', i):
            j = src.index('*/', i + 2); out.append(' '); i = j + 2
        elif src.startswith('//', i):
            j = src.find('\n', i); j = n if j < 0 else j
            out.append(' '); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)

def main(root, dest):
    root = Path(root)
    single = root / 'build/single_file_libs'
    lines = (single / 'zstd-in.c').read_text(encoding='utf-8').splitlines()
    lines = [l for l in lines if not l.startswith('#define ZSTD_MULTITHREAD') and not any(d in l for d in DROP)]
    lines = [l.replace('#define ZSTD_TRACE 0', '#define ZSTD_TRACE 0\n#define ZSTD_NO_MT 1') for l in lines]
    with tempfile.TemporaryDirectory() as tmp:
        inp, combined = Path(tmp) / 'zstd-c-in.c', Path(tmp) / 'zstd_c.c'
        inp.write_text('\n'.join(lines) + '\n', encoding='utf-8')
        subprocess.run([sys.executable, '-I', str(single / 'combine.py'), '-r', str(root / 'lib'), '-x',
                        'legacy/zstd_legacy.h', '-o', str(combined), str(inp)], check=True, stdout=subprocess.DEVNULL)
        body = strip_comments(combined.read_text(encoding='utf-8', errors='surrogateescape'))
    kept, prev = [], ''
    for line in body.split('\n'):
        line = line.rstrip(' \t')
        # a blank line after a backslash continuation ends the macro: keep it
        if line or prev.endswith('\\'):
            kept.append(line)
        prev = line
    body = '\n'.join(kept)
    assert body.count('#undef ZSTD_isError') == 1
    body = body.replace('#undef ZSTD_isError', '#undef ZSTD_isError\n#define ZSTD_isError mc2z_ZSTD_isError')
    header = re.match(r'\s*(/\*.*?\*/)', (root / 'lib/zstd.h').read_text(encoding='utf-8'), re.S).group(1)
    Path(dest).write_text(header + '\n' + body.lstrip('\n'), encoding='utf-8', newline='\n')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
