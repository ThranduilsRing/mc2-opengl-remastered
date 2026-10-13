#!/usr/bin/env python3
"""campaign_save_roundtrip - write a save after a CAMPAIGN start, and load one written by another exe.

WHAT: boots the stock campaign through the logistics front end (MC2_BOOT_TO_BAY launches the first campaign mission, the soak auto-win ends it, the
logistics screens run, the next mission starts), quicksaves at a render frame (MC2_BOOT_QUICKSAVE_AT) and, when `--load FILE` is given, puts FILE where the
quickload reads it (testgame.ims) and quickloads it at --load-at, writing a second save right after (testgame_resave.ims). `logistics FILE` prints the
LogisticsData text packet of a save: the campaign state, the pilots and the inventory that tools/saveload_roundtrip.py's mission-boot saves leave empty.
WHY: the logistics writers and readers (LogisticsMissionInfo / LogisticsPilot / LogisticsMech / LogisticsVariant save and load) run only after a campaign
start. A save written by one exe, loaded by another and written again must equal the original except for the known noise bytes
(tools/save_noise_mask.json); that is the proof that a string-type change in those classes left the on-disk format alone.

  py -3 tools/campaign_save_roundtrip.py capture --out DIR [--deploy INSTALL] [--save-at 2400] [--timeout 150]
  py -3 tools/campaign_save_roundtrip.py capture --out DIR --load OLD/save1.ims --load-at 900     # load a save from another exe, re-save
  py -3 tools/campaign_save_roundtrip.py logistics DIR/save1.ims
  py -3 tools/campaign_save_roundtrip.py compare OLD/save1.ims NEW/save2.ims      # logistics text, version/campaign packets, pilot text bytes
  py -3 tools/saveload_roundtrip.py resave --mission mc2_05 --plant OLD/save1.ims --save-at 1000000000 --load-at 1100 --out DIR   # the mission boot that loads it
  py -3 tools/save_inspect.py diff A.ims B.ims --mask tools/save_noise_mask.json --across-builds

A launch goes through nofocus.spawn (background, host-wide lease). The owner's quicksave is backed up first and put back afterwards.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import save_inspect as si  # noqa: E402
import savegame_backup  # noqa: E402
from smoke_lib import nofocus  # noqa: E402

DEFAULT_DEPLOY = "A:/Games/mc2-opengl/mc2-win64-v0.4"
#: Variables that would change the campaign flow when inherited from the caller's shell.
SCRUB = ("MC2_ACTIVE_MOD", "MC2_MOD_DEPS", "MC2_SOAK_AUTOWIN", "MC2_SOAK_AUTO_PURCHASE", "MC2_SOAK_LANCE_RANDOM", "MC2_SOAK_CHECK_SCREENS",
         "MC2_SOAK_KILL_ENEMY", "MC2_SOAK_PILOT_PROMOTE", "MC2_SOAK_STOP_AT_BAY", "MC2_BOOT_QUICKSAVE_AT", "MC2_BOOT_QUICKLOAD_AT")


def campaign_env(campaign: str, win_after_sec: float, save_at: int, load_at: int | None) -> dict:
    env = {k: v for k, v in os.environ.items() if k not in SCRUB}
    env.update({
        "MC2_BOOT_TO_BAY": campaign,
        "MC2_MENU_CANARY_SKIP_INTRO": "1",
        "MC2_SOAK_AUTOWIN": "1",
        "MC2_SOAK_WIN_AFTER_SEC": str(win_after_sec),
        "MC2_SOAK_AUTO_PURCHASE": "1",
        "MC2_NO_CURSOR_GRAB": "1",
        "MC2_CURSOR_GUARD_OFF": "1",
        "MC2_LOG": "1",
        "MC2_SMOKE_FREEZE_ANIM": "1",
        "MC2_BOOT_QUICKSAVE_AT": str(save_at),
    })
    if load_at is not None:
        env["MC2_BOOT_QUICKLOAD_AT"] = str(load_at)
    return env


def logistics_text(path: str) -> str:
    """The LogisticsData FIT text a save carries (the last text packet that has a [General] block with VariantCount)."""
    found = ""
    for typ, raw in si.load(path)[1]:
        if si.packet_kind(typ, raw) == "text" and b"VariantCount" in raw:
            found = raw.decode("latin-1")
    return found


#: The first bytes of a MechWarriorData record (name, callsign, video, audio, brain strings, skills): byte-identical in every run and across builds
#: (tools/save_noise_mask.json, pilot_text_fields). The logistics pilot text feeds them.
PILOT_TEXT_BYTES = 2168


def compare(a: str, b: str, show: int = 40) -> int:
    """Compare the two saves where the logistics classes can change them: the LogisticsData text packet, the first two packets (version and campaign
    name) and the text bytes of every pilot record. -> 0 when all three are equal. The mission objects (terrain, buildings, movers) are NOT compared:
    a save written during a campaign flow and one re-written in a mission boot differ there by design, and a within-exe re-save is not byte-identical either."""
    import difflib
    pa, pb = si.load(a)[1], si.load(b)[1]
    bad = 0
    la, lb = logistics_text(a), logistics_text(b)
    if la == lb and la:
        print(f"logistics text: identical ({len(la)} bytes)")
    else:
        bad += 1
        print(f"logistics text: DIFFERENT ({len(la)} vs {len(lb)} bytes)" if la and lb else "logistics text: MISSING in one save")
        for ln in list(difflib.unified_diff(la.splitlines(), lb.splitlines(), lineterm="", n=0))[:show]:
            print("  " + ln[:160])
    head = [(x, y) for x, y in zip(pa[:2], pb[:2])]
    same_head = len(head) == 2 and all(x == y for x, y in head)
    print("packets 0-1 (version, campaign name): " + ("identical" if same_head else "DIFFERENT"))
    bad += 0 if same_head else 1
    pilots = [(i, x[1], y[1]) for i, (x, y) in enumerate(zip(pa, pb)) if si.packet_kind(*x) == "MechWarriorData" and si.packet_kind(*y) == "MechWarriorData"]
    off = [i for i, x, y in pilots if x[:PILOT_TEXT_BYTES] != y[:PILOT_TEXT_BYTES]]
    print(f"pilot records: {len(pilots)} compared, text bytes 0-{PILOT_TEXT_BYTES - 1} " + ("identical" if not off and pilots else f"DIFFERENT in packets {off[:10]}"))
    bad += 0 if (pilots and not off) else 1
    return 1 if bad else 0


def add_player_variant(src: str, dst: str, chassis: str, variant: str, mech_variant: str) -> None:
    """Write `dst`: `src` with one player-made variant (no components) added to its LogisticsData packet, and the second inventory mech pointed at it by
    `mech_variant` (a different spelling of the same name). A campaign soak makes no variants, so this is how LogisticsVariant::save, loadVariant and the
    case-folded variant lookup of loadMech get a save to run on."""
    first, pk = si.load(src)
    out = []
    for typ, raw in pk:
        if si.packet_kind(typ, raw) == "text" and b"VariantCount" in raw:
            text = raw.decode("latin-1")
            assert "VariantCount = 0" in text and "[Version]" in text, "the source save already has variants or is not a LogisticsData packet"
            block = f'[Variant0]\r\nst Chassis = "{chassis}"\r\nst VariantName = "{variant}"\r\nl ComponentCount = 0\r\n\r\n'
            text = text.replace("[Version]", block + "[Version]", 1).replace("VariantCount = 0", "VariantCount = 1", 1)
            marker = text.index("[Inventory1]")
            at = text.index("st Variant = ", marker)
            end = text.index("\n", at)
            eol = "\r" if text[end - 1] == "\r" else ""
            text = text[:at] + f'st Variant = "{mech_variant}"' + eol + text[end:]
            raw = text.encode("latin-1")
        out.append((typ, raw))
    si.scs.write(dst, out, False)


def capture(args) -> int:
    deploy = os.path.abspath(args.deploy)
    exe = os.path.join(deploy, "mc2.exe")
    if not os.path.isfile(exe):
        sys.exit(f"exe not found: {exe}")
    out = os.path.abspath(args.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    sdir = savegame_backup.default_dir()
    first_src, second_src = (os.path.join(sdir, n) for n in ("testgame.ims", "testgame_resave.ims"))
    loading = args.load is not None
    # With a load, the quicksave fixture is armed far out of reach: it only makes the engine write the re-save right after the load.
    env = campaign_env(args.campaign, args.win_after_sec, 10**9 if loading else args.save_at, args.load_at if loading else None)
    with savegame_backup.guarded(sdir):
        for stale in (first_src, second_src):
            if os.path.exists(stale):
                os.remove(stale)
        if loading:
            shutil.copy2(args.load, first_src)
        with open(os.path.join(out, "stdout.txt"), "w") as fo, open(os.path.join(out, "stderr.txt"), "w") as fe:
            p = nofocus.spawn([exe], cwd=deploy, stdout=fo, stderr=fe, env=env)
            try:
                p.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                p.kill()
        want = second_src if loading else first_src
        if os.path.exists(want):
            shutil.copy2(want, os.path.join(out, "resave.ims" if loading else "save1.ims"))
        elif not args.keep_going:
            print(f"campaign_save_roundtrip: no save was written ({want}); see {out}/stdout.txt")
            return 1
    print(f"campaign_save_roundtrip: wrote {out}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("--out", required=True)
    c.add_argument("--deploy", default=DEFAULT_DEPLOY)
    c.add_argument("--campaign", default="campaign")
    c.add_argument("--save-at", type=int, default=2400, help="render frame of the quicksave")
    c.add_argument("--load", help="a save written by another exe; quickloaded at --load-at, then saved again")
    c.add_argument("--load-at", type=int, default=900)
    c.add_argument("--win-after-sec", type=float, default=2.0)
    c.add_argument("--timeout", type=int, default=150)
    c.add_argument("--keep-going", action="store_true")
    g = sub.add_parser("logistics")
    g.add_argument("save")
    k = sub.add_parser("compare")
    k.add_argument("a")
    k.add_argument("b")
    v = sub.add_parser("add-variant", help="write a copy of a save with one player-made variant in its LogisticsData packet")
    v.add_argument("src")
    v.add_argument("dst")
    v.add_argument("--chassis", default="data/objects/bushwacker.csv")
    v.add_argument("--name", default="Bush-Whacker 'Test' Z")
    v.add_argument("--mech-name", default="BUSH-WHACKER 'TEST' z", help="how the second inventory mech spells the variant")
    args = ap.parse_args(argv)
    if args.cmd == "logistics":
        sys.stdout.write(logistics_text(args.save))
        return 0
    if args.cmd == "compare":
        return compare(args.a, args.b)
    if args.cmd == "add-variant":
        add_player_variant(args.src, args.dst, args.chassis, args.name, args.mech_name)
        return 0
    return capture(args)


if __name__ == "__main__":
    sys.exit(main())
