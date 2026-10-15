"""Mission ingress witness through the real tick hooks and the leased lane3 smoke harness.

Run after the orchestrator commits, builds and deploys the reviewed executable.
Exit 1 means the witness failed. The drain/retirement negative-control executables
must each produce that failure; this tool never manually drains or retires orders.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools/dev_shell"))
import console_child
from mc2_cmd import send_command


def authored_fixture(stock, case):
    """A stock mission with one forced alternate; the alternate's pilot must not win."""
    stock = re.sub(r"(?mi)^FITend[ \t]*\r?$", "", stock)
    sections = re.split(r"(?m)^(?=\[)", stock)
    blocks = {}
    for i, section in enumerate(sections):
        match = re.match(r"\[([^]]+)\]", section)
        if match:
            blocks[match[1].lower()] = i

    def value(section, key):
        match = re.search(r"(?mi)^\S+\s+" + re.escape(key) + r"\s*=\s*([^\r\n]+)", section)
        if not match:
            raise ValueError(f"stock fixture missing {key}")
        return match[1].strip()

    def replace(section, key, entry):
        return re.sub(r"(?mi)^\S+\s+" + re.escape(key) + r"\s*=[^\r\n]*", entry, section)

    parts = sorted(int(key[4:]) for key in blocks if re.fullmatch(r"part\d+", key))
    original = next(n for n in parts if int(value(sections[blocks[f"part{n}"]], "CommanderID")) == 1
                    and int(value(sections[blocks[f"part{n}"]], "Exists")) == 1
                    and int(value(sections[blocks[f"part{n}"]], "Active")) == 1)
    selected = max(parts) + 1
    part = sections[blocks[f"part{original}"]]
    pilot = int(value(part, "Pilot"))
    alternate_pilot = next(int(value(sections[blocks[f"part{n}"]], "Pilot")) for n in parts
                           if int(value(sections[blocks[f"part{n}"]], "Pilot")) != pilot)
    squad = max(int(value(sections[blocks[f"part{n}"]], "SquadNum")) for n in parts) + 1
    part = replace(part, "SquadNum", f"ul SquadNum = {squad}")
    alternate = re.sub(r"^\[Part\d+\]", f"[Part{selected}]", part)
    alternate = replace(alternate, "Pilot", f"ul Pilot = {alternate_pilot}")
    part = replace(part, "IndicesOfAlternatives", f"l[15] IndicesOfAlternatives = {selected}," + "-1," * 14)
    sections[blocks[f"part{original}"]] = part
    squads = list(dict.fromkeys(int(value(sections[blocks[f"part{n}"]], "SquadNum")) for n in parts))
    if "squads" in blocks:
        sections[blocks["squads"]] = ""
    forced = "[Squads]\n" + "".join(f"ul Squad{i} = {int(s == squad)}\n" for i, s in enumerate(squads))
    x, y = (float(value(part, key)) for key in ("PositionX", "PositionY"))
    points = [[x, y, 0.0, 0], [x + 16, y + 16, 0.0, 0]]
    route = "l OrderType = 2\nl OrderStance = 4\nul WaypointCount = 2\n"
    for i, point in enumerate(points):
        route += "".join(f"f Waypoint{i}{axis} = {coordinate}\n" for axis, coordinate in zip("XYZ", point))
    if case == "malformed":
        route += "f Waypoint2X = 1\n"
    if case == "suppressed":
        index = blocks[f"warrior{pilot}"]
        sections[index] += ('Brain {\n compatibilityMode = "Enhanced"\n'
                            ' PrimaryOPORD { type = Guard }\n}\n')
    expectation = {"status": "Installed", "original": original, "selected": selected,
                   "pilot": pilot, "stance": 4, "kind": 7, "points": points}
    return "".join(sections) + "\n" + alternate + route + "\n" + forced + "\nFITend\n", expectation


def same_authored_coordinate(actual, authored):
    """Require the same finite float32 after round trip, with no fixed epsilon.

    The engine stores float32 and prints its shortest round-trip decimal. Accept
    only that authored float's rounding cell (half an ULP on each side at 2880:
    0.0001220703125); even the adjacent float32 coordinate must fail.
    """
    if type(actual) not in (int, float) or type(authored) not in (int, float):
        return False
    try:
        actual32 = struct.unpack("!f", struct.pack("!f", actual))[0]
        authored32 = struct.unpack("!f", struct.pack("!f", authored))[0]
    except (OverflowError, struct.error):
        return False
    return math.isfinite(actual32) and math.isfinite(authored32) and actual32 == authored32


def authored_evidence_matches(data, expected, case):
    """Empty evidence cannot pass a disconnected loader or an unconsumed pending receipt."""
    if not isinstance(data, dict):
        return False
    if case == "valid":
        installations = data.get("installations")
        if not isinstance(installations, list) or len(installations) != 1 or data.get("diagnostics") != []:
            return False
        actual = installations[0]
        if not isinstance(actual, dict) or actual.keys() != expected.keys():
            return False
        if any(actual[key] != value for key, value in expected.items() if key != "points"):
            return False
        points = actual["points"]
        if not isinstance(points, list) or len(points) != len(expected["points"]):
            return False
        return all(isinstance(point, list) and len(point) == 4 and point[3] == authored[3]
                   and all(same_authored_coordinate(a, b) for a, b in zip(point[:3], authored[:3]))
                   for point, authored in zip(points, expected["points"]))
    status, reason = ("Rejected", "stray_key") if case == "malformed" else ("Suppressed", "enhanced_opord")
    diagnostic = {"status": status, "original": expected["original"], "selected": expected["selected"],
                  "pilot": expected["pilot"], "reason": reason}
    return data.get("installations") == [] and data.get("diagnostics") == [diagnostic]


def authored_witness(smoke, send, expected, case, now=time.monotonic, sleep=time.sleep,
                     startup_timeout=3720, runtime_timeout=25):
    deadline = now() + startup_timeout
    while smoke.poll() is None and now() < deadline:
        try:
            reply = send("authored_order_evidence")
            if (isinstance(reply, dict) and reply.get("ok") is True and reply.get("version") == 1
                    and isinstance(reply.get("data"), dict)
                    and isinstance(reply["data"].get("installations"), list)
                    and isinstance(reply["data"].get("diagnostics"), list)):
                break
        except (OSError, ValueError):
            pass
        sleep(0.25)
    else:
        return False
    deadline = now() + runtime_timeout
    while smoke.poll() is None and now() < deadline:
        try:
            reply = send("authored_order_evidence")
            if isinstance(reply, dict) and reply.get("ok") is True and reply.get("version") == 1:
                data = reply.get("data")
                if authored_evidence_matches(data, expected, case):
                    print(f"authored {case}: {json.dumps(data, sort_keys=True)}", flush=True)
                    return True
        except (OSError, ValueError):
            pass
        sleep(0.25)
    print(f"authored {case}: installation/diagnostic evidence failed", flush=True)
    return False


def run_authored(args):
    """Only new scratch mod files are written; the installed stock mission stays untouched."""
    from mc2x_import.fst import extract_fst_file
    install = args.install.resolve()
    allowed = Path("A:/Games/mc2-opengl/mc2-win64-lane3").resolve()
    if install != allowed:
        raise ValueError("authored runtime witness requires the lane3 install")
    scratch = install / "mods" / ("s4b2-witness-" + uuid.uuid4().hex[:12])
    scratch.mkdir(parents=True, exist_ok=False)
    stock_copy = scratch / "stock-mc2_01.fit"
    loose = install / "data/missions/mc2_01.fit"
    if loose.is_file():
        stock_copy.write_bytes(loose.read_bytes())
    elif not extract_fst_file(install / "mission.fst", "data/missions/mc2_01.fit", str(stock_copy)):
        raise ValueError("stock mc2_01.fit unavailable")
    stock = stock_copy.read_text(encoding="latin-1")
    print(f"retained authored scratch fixture: {scratch}", flush=True)
    fixture = scratch / "data/missions/mc2_01.fit"
    fixture.parent.mkdir(parents=True)
    (scratch / "mod.json").write_text(json.dumps({"id": scratch.name, "name": "S4-b2 authored witness"}))
    for case, gate in (("valid", "0"), ("malformed", "0"), ("suppressed", "0"), ("suppressed", "1")):
        text, expected = authored_fixture(stock, case)
        fixture.write_text(text, encoding="latin-1")
        (scratch / f"{case}-gate{gate}.fit").write_text(text, encoding="latin-1")
        env = dict(os.environ, MC2_DEV_SHELL="1", MC2_DEV_SHELL_PORT=str(args.port),
                   MC2_SMOKE_REQUIRE_FINGERPRINT="1", MC2_ACTIVE_MOD=scratch.name,
                   MC2_MOD_DEPS="", MC2_BRAIN_MISSIONFIT_OPORD=gate)
        command = [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--mission", "mc2_01",
                   "--duration", "30", "--keep-logs", "--deploy", "lane3"]
        with subprocess.Popen(command, cwd=ROOT, env=env, **console_child.child_kwargs()) as smoke:
            evidence_case = "valid" if case == "suppressed" and gate == "0" else case
            passed = authored_witness(smoke, lambda command: send_command(command, port=args.port, timeout=1),
                                      expected, evidence_case,
                                      startup_timeout=float(env.get("MC2_HOST_LEASE_TIMEOUT") or 3600) + 120)
            try:
                status = smoke.wait(timeout=120)
            except subprocess.TimeoutExpired:
                smoke.terminate()
                smoke.wait(timeout=15)
                return 124
        print(f"authored case={case} opord_gate={gate} passed={passed} smoke={status}", flush=True)
        if not passed or status:
            return 1
    return 0


def witness(smoke, send, now=time.monotonic, sleep=time.sleep, startup_timeout=3720,
            runtime_timeout=25):
    """Three automatically consumed batches cross the 8192-receipt capacity."""
    started = now()
    deadline = started + startup_timeout
    last_report = None
    next_report = started

    def request(command, batch, attempt=0):
        nonlocal last_report, next_report
        try:
            reply = send(command)
            if not isinstance(reply, dict) or type(reply.get("ok")) is not bool or reply.get("version") != 1:
                raise ValueError(f"invalid dev-shell v1 reply: {reply!r}")
            report = json.dumps(reply, sort_keys=True)
        except (OSError, ValueError) as error:
            reply = None
            report = f"{type(error).__name__}: {error}"
        identity = (command, batch, report)
        if attempt or identity != last_report or now() >= next_report:
            print(f"mission ingress elapsed={now() - started:.2f}s batch={batch} "
                  f"attempt={attempt} command={command} response={report}", flush=True)
            last_report = identity
            next_report = now() + 30
        return reply

    # Lease waits and mission loading precede the listener; neither spends the play budget.
    while smoke.poll() is None and now() < deadline:
        reply = request("ping", 0)
        if reply is not None:
            if not reply["ok"]:
                return False
            break
        sleep(0.25)
    else:
        print("mission ingress readiness failed: harness exited or startup deadline expired", flush=True)
        return False

    deadline = now() + runtime_timeout
    for batch in range(3):
        attempt = 0
        while smoke.poll() is None and now() < deadline:
            attempt += 1
            reply = request("order_ingress_fixture_submit", batch + 1, attempt)
            # A lost admission reply is ambiguous: replay could submit the next batch twice.
            if reply is None:
                print("mission ingress submission ambiguous: not replaying", flush=True)
                return False
            if reply["ok"]:
                break
            sleep(min(0.25, max(0, deadline - now())))
        else:
            print(f"mission ingress batch {batch + 1}/3 submission failed: "
                  "harness exited or play deadline expired", flush=True)
            return False
        while smoke.poll() is None and now() < deadline:
            reply = request("order_ingress_fixture_check", batch + 1)
            if reply is not None and reply["ok"] and now() < deadline:
                break
            sleep(0.25)
        else:
            print(f"mission ingress batch {batch + 1}/3 check failed: "
                  "harness exited or play deadline expired", flush=True)
            return False
        print(f"mission ingress batch {batch + 1}/3 installed and automatically retired", flush=True)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=19874)
    parser.add_argument("--authored", action="store_true", help="four live Part fixtures, including alternate identity")
    parser.add_argument("--install", type=Path, default=Path("A:/Games/mc2-opengl/mc2-win64-lane3"))
    args = parser.parse_args()
    if args.authored:
        return run_authored(args)
    env = dict(os.environ, MC2_DEV_SHELL="1", MC2_DEV_SHELL_PORT=str(args.port),
               MC2_SMOKE_REQUIRE_FINGERPRINT="1")
    command = [sys.executable, str(ROOT / "scripts/run_smoke.py"), "--mission", "mc2_01",
               "--duration", "30", "--keep-logs", "--deploy", "lane3"]
    with subprocess.Popen(command, cwd=ROOT, env=env, **console_child.child_kwargs()) as smoke:
        passed = witness(smoke, lambda command: send_command(
            command, port=args.port, timeout=10 if command.endswith("_submit") else 1),
            startup_timeout=float(env.get("MC2_HOST_LEASE_TIMEOUT") or 3600) + 120)
        try:
            status = smoke.wait(timeout=120)
        except subprocess.TimeoutExpired:
            smoke.terminate()
            smoke.wait(timeout=15)
            print("mission ingress runtime harness TIMEOUT")
            return 124
    print(f"mission ingress runtime witness passed={passed} smoke={status}")
    return 0 if passed and status == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
