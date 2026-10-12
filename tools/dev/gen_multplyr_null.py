#!/usr/bin/env python3
"""gen_multplyr_null - regenerate the no-op MultiPlayer member bodies in code/multplyr_null.cpp.

WHAT: reads the member declarations of `class MultiPlayer` in code/multplyr.h and writes one empty body per named member between
the GENERATED-STUBS markers of code/multplyr_null.cpp. The names come from the linker: build with MC2_MULTIPLAYER=OFF, and every
`unresolved external symbol ... MultiPlayer::name(` line names a member the game still calls.
WHY: with the session layer compiled out the lobby screens and the `if (MPlayer)` sites still reference these members; a
hand-written stub drifts from the header, a generated one cannot.

  py -3 tools/dev/gen_multplyr_null.py --names setup,update,isHost
  py -3 tools/dev/gen_multplyr_null.py --from-link-log build.log      # names parsed from LNK2019 lines
  py -3 tools/dev/gen_multplyr_null.py --selftest
"""
from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HEADER = os.path.join(ROOT, "code", "multplyr.h")
TARGET = os.path.join(ROOT, "code", "multplyr_null.cpp")
BEGIN = "// GENERATED-STUBS-BEGIN"
END = "// GENERATED-STUBS-END"

_DECL = re.compile(r"^\s*(?:static\s+)?(?:virtual\s+)?(?P<ret>(?:const\s+)?[A-Za-z_][\w:]*(?:\s*[*&]+)?)\s+(?:\*\s*)?(?P<name>\w+)\s*\((?P<params>[^)]*)\)\s*(?P<const>const)?\s*;",
                   re.S)
_LINK = re.compile(r"MultiPlayer::(\w+)\(")


def class_body(header_text: str) -> str:
    i = header_text.find("class MultiPlayer {")
    if i < 0:
        sys.exit("class MultiPlayer not found in " + HEADER)
    j = header_text.find("\n};", i)
    return header_text[i:j]


def declarations(body: str) -> dict:
    """-> {name: (return type, params without defaults, is const)} for every member declared without an inline body."""
    out = {}
    # join continuation lines: a declaration ends at ';' or opens a body at '{'
    buf = ""
    for ln in body.splitlines():
        s = ln.split("//")[0]
        if s.strip().startswith("}"):
            buf = ""
            continue
        buf += " " + s.strip()
        if ";" in s or "{" in s:
            m = _DECL.match(buf.strip())
            if m and "{" not in buf:
                params = re.sub(r"\s*=\s*[^,)]+", "", m.group("params")).strip()
                if params == "void":
                    params = ""
                out[m.group("name")] = (m.group("ret").strip(), params, bool(m.group("const")))
            buf = ""
    return out


def stub(name: str, ret: str, params: str, const: bool) -> str:
    sig = f"{ret} MultiPlayer::{name}({params})" + (" const" if const else "")
    if ret == "void":
        return sig + " {}"
    if ret == "bool":
        return sig + " { return false; }"
    if ret.endswith("*"):
        return sig + " { return nullptr; }"
    return sig + " { return " + ret + "(); }"


def render(names, decls: dict) -> list:
    lines, missing = [], []
    for n in sorted(set(names)):
        d = decls.get(n)
        if not d:
            missing.append(n)
            continue
        lines.append(stub(n, *d))
    if missing:
        sys.exit("not declared without a body in multplyr.h: " + ", ".join(missing))
    return lines


def write(lines: list) -> None:
    raw = open(TARGET, "rb").read().decode("utf-8")
    i, j = raw.find(BEGIN), raw.find(END)
    if i < 0 or j < 0:
        sys.exit("markers missing in " + TARGET)
    i = raw.find("\n", i) + 1
    new = raw[:i] + "".join(l + "\n" for l in lines) + raw[j:]
    open(TARGET, "wb").write(new.encode("utf-8"))


def selftest() -> int:
    body = """class MultiPlayer {
	public:
		void setup (void);
		bool isHost (void) {
			return(false);
		}
		long hostSession (char* sessionName, char* playerName, long maxPlayers = 8);
		MC2Session* getSessions (long& sessionCount);
		static void resetAll (void);
		void update (void);
};"""
    d = declarations(body)
    fails = []
    if "isHost" in d:
        fails.append("inline bodies are not stubbed")
    if d.get("hostSession") != ("long", "char* sessionName, char* playerName, long maxPlayers", False):
        fails.append(f"defaults stripped: {d.get('hostSession')}")
    if stub("getSessions", *d["getSessions"]) != "MC2Session* MultiPlayer::getSessions(long& sessionCount) { return nullptr; }":
        fails.append(stub("getSessions", *d["getSessions"]))
    if stub("resetAll", *d["resetAll"]) != "void MultiPlayer::resetAll() {}":
        fails.append(stub("resetAll", *d["resetAll"]))
    if stub("hostSession", *d["hostSession"]).split("{")[1].strip() != "return long(); }":
        fails.append("value-initialized return")
    for f in fails:
        print("gen_multplyr_null selftest FAIL: " + f)
    print("gen_multplyr_null selftest: " + ("FAIL" if fails else "ok"))
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--names", help="comma-separated member names")
    ap.add_argument("--from-link-log", help="a build log; MultiPlayer::name( occurrences are the names")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    names = []
    if args.names:
        names += [n for n in args.names.split(",") if n]
    if args.from_link_log:
        names += _LINK.findall(open(args.from_link_log, encoding="utf-8", errors="replace").read())
    if not names:
        ap.error("--names or --from-link-log")
    decls = declarations(class_body(open(HEADER, encoding="utf-8", errors="replace").read()))
    lines = render(names, decls)
    write(lines)
    print(f"gen_multplyr_null: wrote {len(lines)} stubs to {os.path.relpath(TARGET, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
