#!/usr/bin/env python3
"""DEV-SHELL-1 hot-reload watcher — the edit->see loop, closed.

Polls mtimes; on change sends the matching dev-shell command to the running
mc2.exe (MC2_DEV_SHELL=1):
  *.frag/.vert/.tesc/.tese/.geom/.comp/.hglsl under <deploy>/shaders/
      -> reload_shaders (compile/link errors printed here, loudly)
  data/art/*.fit under <deploy>
      -> ui_reload (re-inits the active front-end screen; only fires if the
         changed .fit is plausibly front-end art, engine decides the rest)

Usage:
  py -3 tools/dev_shell/watch.py A:/Games/mc2-opengl/mc2-win64-v0.4c [--port 9877]
Ctrl+C to stop.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mc2_cmd import send_command

SHADER_EXT = {".frag", ".vert", ".tesc", ".tese", ".geom", ".comp", ".hglsl", ".glsl"}


def scan(deploy):
    seen = {}
    shaders = os.path.join(deploy, "shaders")
    art = os.path.join(deploy, "data", "art")
    for root, _dirs, files in os.walk(shaders):
        for f in files:
            if os.path.splitext(f)[1].lower() in SHADER_EXT:
                p = os.path.join(root, f)
                seen[p] = ("shader", os.path.getmtime(p))
    if os.path.isdir(art):
        for f in os.listdir(art):
            if f.lower().endswith(".fit"):
                p = os.path.join(art, f)
                seen[p] = ("fit", os.path.getmtime(p))
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("deploy", help="deploy dir the running mc2.exe lives in")
    ap.add_argument("--port", type=int, default=9877)
    ap.add_argument("--interval", type=float, default=0.5)
    args = ap.parse_args()

    state = scan(args.deploy)
    print(f"[watch] {len(state)} files ({sum(1 for k,(t,_) in state.items() if t=='shader')} shaders); Ctrl+C stops")
    while True:
        time.sleep(args.interval)
        cur = scan(args.deploy)
        changed_kinds = set()
        for p, (kind, mt) in cur.items():
            old = state.get(p)
            if old is None or old[1] != mt:
                print(f"[watch] changed: {os.path.relpath(p, args.deploy)}")
                changed_kinds.add(kind)
        state = cur
        for kind in changed_kinds:
            cmd = "reload_shaders" if kind == "shader" else "ui_reload"
            try:
                reply = send_command(cmd, {}, port=args.port)
            except OSError as e:
                print(f"[watch] {cmd}: engine not reachable ({e})")
                continue
            if reply.get("ok"):
                d = reply.get("data", {})
                if cmd == "reload_shaders":
                    print(f"[watch] reloaded={d.get('reloaded')} failed={d.get('failed')}")
                else:
                    print(f"[watch] ui_reload: {d.get('screen')} <- {d.get('fit')}")
            else:
                print(f"[watch] {cmd} FAILED:\n{reply.get('error')}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
