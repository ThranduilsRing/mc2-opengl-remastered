#!/usr/bin/env python3
"""What: proves a verb chain split into handler-group functions is the same chain: same branches, same bodies, same order.
Why: processUiCommandChannel's prefix matches are order-sensitive ("order move " before "order ", "list buildings" before "list"),
so a split must keep the branch sequence; comparing comment- and whitespace-stripped text catches any reorder, drop or edit.
Usage: py -3 tools/split/verb_chain_equiv.py --old-rev REV --old-file PATH --dispatcher PATH FILE...
  The old chain is the text between 'if (line == "toggle_pause")' and the 'if (ack) {' epilogue of the old file; the new chain is
  the bodies of the uiCmd<Group> functions found in FILE... concatenated in the order the dispatcher calls them."""
import argparse, os, re, subprocess, sys


def strip_comments(t):
    out, i, n = [], 0, len(t)
    while i < n:
        c = t[i]
        if c in "\"'":
            q, j = c, i + 1
            while j < n and t[j] != q:
                j += 2 if t[j] == "\\" else 1
            out.append(t[i:j + 1]); i = j + 1
        elif t.startswith("//", i):
            while i < n and t[i] != "\n":
                i += 1
        elif t.startswith("/*", i):
            i = t.find("*/", i) + 2
        else:
            out.append(c); i += 1
    return "".join(out)


def norm(t):
    return re.sub(r"\s+", "", strip_comments(t)).replace("elseif(", "if(")


def func_body(text, name):
    m = re.search(r"bool MissionInterfaceManager::%s\(UiCmdCtx& c\)\s*\{" % name, text)
    if not m:
        raise SystemExit("missing function " + name)
    depth, i = 1, m.end()
    while depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


ap = argparse.ArgumentParser()
ap.add_argument("--old-rev", required=True)
ap.add_argument("--old-file", required=True)
ap.add_argument("--dispatcher", required=True)
ap.add_argument("files", nargs="+")
a = ap.parse_args()
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts"))
import console_child  # noqa: E402

old = subprocess.run(["git", "show", "%s:%s" % (a.old_rev, a.old_file)], capture_output=True, text=True, encoding="utf-8",
                     **console_child.child_kwargs(redirected=True)).stdout
s = old.index('if (line == "toggle_pause")')
e = old.index("if (ack) {", s)
old_chain = norm(old[s:e])

disp = open(a.dispatcher, encoding="utf-8").read()
order = re.search(r"\(void\)\(((?:uiCmd\w+\(c\)(?:\s*\|\|\s*)?)+)\)", disp)
if not order:
    raise SystemExit("dispatcher call chain not found")
names = re.findall(r"(uiCmd\w+)\(c\)", order.group(1))
src = "\n".join(open(f, encoding="utf-8").read() for f in a.files)
alias = re.compile(r"(const std::string& line = c\.line;|const char\*& result = c\.result;|std::string& note = c\.note;|std::string& extra = c\.extra;)")
new_chain = ""
for n in names:
    body = alias.sub("", func_body(src, n))
    body = re.sub(r"\belse\s+return false;\s*return true;\s*$", "", body.strip())
    new_chain += norm(body)
print("dispatch order:", " -> ".join(names))
if old_chain == new_chain:
    print("verb chain identical (%d normalised chars, %d groups)" % (len(old_chain), len(names)))
    sys.exit(0)
for i, (x, y) in enumerate(zip(old_chain, new_chain)):
    if x != y:
        print("DIFF at char %d:\n old: %s\n new: %s" % (i, old_chain[max(0, i - 80):i + 80], new_chain[max(0, i - 80):i + 80]))
        break
else:
    print("length differs: old %d new %d" % (len(old_chain), len(new_chain)))
sys.exit(1)
