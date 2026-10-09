"""
tools/mc2x_import/manifest.py -- provenance manifest + marker emission for the
MC2X importer. Stdlib only, no emoji.
"""
import csv, json, os, subprocess


def tool_version(): return "1.0"


def git_commit(repo_hint):
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=repo_hint,
                              capture_output=True, text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout.strip() or "unknown"
    except Exception:
        return "unknown"


def source_fingerprint(source):
    import hashlib
    h = hashlib.sha256()
    for rel in ("data/objects/object2.pak", "tgl.fst", "mission.fst", "mc2xres.dll"):
        p = os.path.join(source, rel.replace("/", os.sep))
        h.update(rel.encode())
        h.update(str(os.path.getsize(p)).encode() if os.path.isfile(p) else b"-")
    return h.hexdigest()[:16]


def write_marker(deploy, mod_id, report):
    with open(os.path.join(deploy, "mods", mod_id, "mc2x_import_report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)


def write_reports(deploy, stamp, report, rows):
    d = os.path.join(deploy, "mods", ".import_reports", stamp); os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "import_report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)
    with open(os.path.join(d, "import_manifest.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f); w.writerow(["mod", "rel_path", "size", "sha256", "source"])
        w.writerows(rows)
