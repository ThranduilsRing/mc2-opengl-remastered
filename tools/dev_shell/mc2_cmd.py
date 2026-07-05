#!/usr/bin/env python3
"""DEV-SHELL-1 client — send one command to a running mc2.exe.

Engine side: gate MC2_DEV_SHELL=1, socket 127.0.0.1:9877 (MC2_DEV_SHELL_PORT).
Protocol: newline-delimited JSON {"type":cmd,"params":{...}} ->
{"ok":bool,"error":str|null,"version":1,"data":{...}}.

Usage:
  py -3 tools/dev_shell/mc2_cmd.py ping
  py -3 tools/dev_shell/mc2_cmd.py reload_shaders [--force] [--name PROG]
  py -3 tools/dev_shell/mc2_cmd.py screenshot [--name myshot]
  py -3 tools/dev_shell/mc2_cmd.py last_screenshot
  py -3 tools/dev_shell/mc2_cmd.py get_gate --name MC2_TONEMAP_V2
  py -3 tools/dev_shell/mc2_cmd.py set_gate --name MC2_X --value 1
  py -3 tools/dev_shell/mc2_cmd.py quit
"""
import argparse
import json
import socket
import sys

def send_command(cmd_type, params=None, host="127.0.0.1", port=9877, timeout=10.0):
    req = json.dumps({"type": cmd_type, "params": params or {}}) + "\n"
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall(req.encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    line = buf.split(b"\n", 1)[0]
    return json.loads(line.decode())

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("command")
    p.add_argument("--name")
    p.add_argument("--value")
    p.add_argument("--force", action="store_true")
    p.add_argument("--collect", choices=["true", "false"],
                   help="framegraph: flip runtime pass-stats/GPU-timer collectors on/off")
    p.add_argument("--source", choices=["scene", "backbuffer"],
                   help="screenshot source: scene FBO (missions, default) or backbuffer (menus/front-end)")
    p.add_argument("--port", type=int, default=9877)
    args = p.parse_args()

    params = {}
    if args.name:
        params["name"] = args.name
    if args.value is not None:
        params["value"] = args.value
    if args.force:
        params["force"] = True
    if args.source:
        params["source"] = args.source
    if args.collect:
        params["collect"] = args.collect

    try:
        reply = send_command(args.command, params, port=args.port)
    except ConnectionRefusedError:
        print("connection refused — is mc2.exe running with MC2_DEV_SHELL=1?", file=sys.stderr)
        return 2
    print(json.dumps(reply, indent=2))
    return 0 if reply.get("ok") else 1

if __name__ == "__main__":
    sys.exit(main())
