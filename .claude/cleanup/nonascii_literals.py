#!/usr/bin/env python3
"""Report non-ASCII characters that sit inside string/char literals (not comments) in first-party C++ sources."""
import subprocess, os, sys, collections, re
roots = ["code","mclib","GameOS","RenderCore","RenderWorld","GameAdapters","gui","EditorBridge","GuiRuntime","editor","tools/mc2_launcher","tools/asset_viewer","HostServices"]
ext = {".cpp",".h",".hpp",".c",".inl"}
files = subprocess.run(["git","ls-files"],capture_output=True,text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout.split("\n")
out = collections.defaultdict(list); scanned = 0
for f in files:
    if "3rdparty" in f or not any(f.startswith(r+"/") for r in roots): continue
    if os.path.splitext(f)[1].lower() not in ext: continue
    b = open(f,"rb").read()
    if not any(x > 127 for x in b): continue
    try: s = b.decode("utf-8")
    except Exception: print("undecodable", f); continue
    scanned += 1
    i, n, line = 0, len(s), 1; wide = False
    st = None  # None | 'lc' | 'bc' | 'str' | 'chr'
    while i < n:
        c = s[i]
        if c == "\n":
            line += 1
            if st in ("lc","str","chr"): st = None   # unterminated literal / line comment ends
        elif st is None:
            if c == "/" and i+1 < n and s[i+1] == "/": st = "lc"; i += 1
            elif c == "/" and i+1 < n and s[i+1] == "*": st = "bc"; i += 1
            elif c == '"': st = "str"; pre = s[max(0,i-2):i]; wide = bool(re.search(r'(?:^|[^A-Za-z0-9_])(?:L|u|U|u8)$', pre) or pre.endswith(('L','U')) or pre.endswith('u8'))
            elif c == "'" and not (i > 0 and s[i-1].isalnum()): st = "chr"
        elif st == "bc":
            if c == "*" and i+1 < n and s[i+1] == "/": st = None; i += 1
        elif st in ("str","chr"):
            if c == "\\": i += 1
            elif (st == "str" and c == '"') or (st == "chr" and c == "'"): st = None
            elif ord(c) > 127: out[f].append((line, s[max(0,i-20):i+20].replace("\n"," ")))
        i += 1
print("scanned", scanned, "files with non-ASCII; files with non-ASCII inside literals:", len(out))
for f, l in sorted(out.items()):
    print(f, "lines", sorted(set(x[0] for x in l))[:8])
    print("   ", l[0][1])

w=[(f,x) for f,l in out.items() for x in l if x[1].startswith('WIDE')]
print('WIDE/char hits:', len(w))
for f,x in w[:20]: print(f,x)
