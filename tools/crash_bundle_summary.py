#!/usr/bin/env python3
"""Summarise a local crash bundle; never open paths named by its content journal."""
import argparse
import json
from pathlib import Path

REQUIRED = ("crash.txt", "minidump.dmp", "last_trace.txt", "profile.json",
            "gpu_breadcrumbs.txt", "content.jsonl")


def summarise(folder: Path) -> tuple[str, bool]:
    missing = [name for name in REQUIRED if not (folder / name).is_file() or not (folder / name).stat().st_size]
    lines = [f"Bundle: {folder}", "Files: " + ("complete" if not missing else "MISSING " + ", ".join(missing))]
    for name in ("crash.txt", "gpu_breadcrumbs.txt"):
        path = folder / name
        if path.is_file():
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith(("Fingerprint:", "Code:", "Reason:", "device=")):
                    lines.append(line)
    profile = folder / "profile.json"
    if profile.is_file():
        try:
            info = json.loads(profile.read_text(encoding="utf-8"))
            lines.append(f"Mission: {info.get('mission_name', '?')} frame={info.get('frame', '?')}")
        except (ValueError, UnicodeError):
            missing.append("valid profile.json")
    mods, content, save, invalid = [], [], {}, 0
    journal = folder / "content.jsonl"
    if journal.is_file():
        with journal.open(encoding="utf-8", errors="replace") as stream:
            for line in stream:
                try:
                    record = json.loads(line)
                    if not isinstance(record, dict):
                        raise ValueError("record must be an object")
                except ValueError:
                    invalid += 1
                    continue
                if record.get("kind") == "mod":
                    mods.append(f"{record.get('id')} (priority {record.get('priority')})")
                elif record.get("kind") == "content":
                    content.append(record)
                elif record.get("kind") == "save_point":
                    save = record
    lines += ["Loaded mods: " + (", ".join(mods) or "unavailable"),
              f"Content: {len(content)} identities (fnv1a64; loaded loose files and decompressed FST members)",
              f"Save point: {save.get('operation', '?')} {save.get('path', '?')}"]
    for record in content[-3:]:
        lines.append(f"  {record.get('hash')} {record.get('source')} {record.get('path')}")
    if invalid:
        lines.append(f"WARNING: {invalid} invalid/truncated journal records")
    complete = not missing and not invalid and bool(mods) and bool(content) and bool(save)
    lines.append("Result: " + ("complete" if complete else "incomplete"))
    # Journal strings (mod ids, file names) are untrusted: no raw control characters (terminal escapes) reach the output.
    lines = ["".join(c if c >= " " and c != "\x7f" else f"\\x{ord(c):02x}" for c in line) for line in lines]
    return "\n".join(lines), complete


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    args = parser.parse_args()
    output, complete = summarise(args.bundle)
    print(output)
    return 0 if complete else 1


if __name__ == "__main__":
    raise SystemExit(main())
