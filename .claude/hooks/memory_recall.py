"""UserPromptSubmit hook: surface memory files relevant to the prompt.

Scores memory files by keyword overlap between the prompt and each file's
name + frontmatter description; prints the top hits so they land in context.
"""
import json
import os
import re
import sys

MEM_DIR = os.path.expanduser("~/.claude/projects/A--Games-mc2-opengl-src/memory").replace("\\", "/")
TOP_N = 5
STOP = set("""the a an and or of to in on for with is are was be it this that we you i
our my me do does did not no yes can should would could just like make from at by as
into out up so if then than but also what how why when where which who all any some
""".split())


def words(text):
    return {w for w in re.findall(r"[a-z0-9_]{3,}", text.lower()) if w not in STOP}


def describe(path):
    name, desc = os.path.basename(path), ""
    try:
        with open(path, encoding="utf-8", errors="ignore") as f:
            head = f.read(1200)
        m = re.search(r"^description:\s*(.+)$", head, re.M)
        if m:
            desc = m.group(1).strip().strip('"')
    except OSError:
        pass
    return name, desc


def main():
    try:
        d = json.load(sys.stdin)
        prompt = (d.get("prompt") or "") if isinstance(d, dict) else ""
    except Exception:
        return
    if not isinstance(prompt, str):
        return
    q = words(prompt)
    if len(q) < 2 or not os.path.isdir(MEM_DIR):
        return
    hits = []
    for fn in os.listdir(MEM_DIR):
        if not fn.endswith(".md") or fn == "MEMORY.md":
            continue
        path = os.path.join(MEM_DIR, fn)
        name, desc = describe(path)
        name_words = words(name.replace("-", " ").replace("_", " "))
        score = 2 * len(q & name_words) + len(q & words(desc))
        if fn.startswith("INDEX-"):
            score -= 1
        if score >= 2:
            hits.append((score, fn, desc))
    if not hits:
        return
    hits.sort(key=lambda h: -h[0])
    print("Possibly relevant memories (read before acting):")
    for _, fn, desc in hits[:TOP_N]:
        print(f"- {MEM_DIR}/{fn} - {desc[:140].encode('ascii', 'replace').decode()}")


if __name__ == "__main__":
    main()
