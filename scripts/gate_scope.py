"""Diff-scoped daytime gate: decide per scoped stage (mods, video-decode) from the candidate diff whether it must run.

Table of what each scoped stage exercises: docs/merge-queue-v2.md ("Diff-scoped gate"). Pure functions; merge_gate.main
feeds in the diff (merge-base of the last complete full-gate head to HEAD) and applies the result as
MC2_GATE_SKIP_REASON="scope: ..." plus MC2_GATE_SCOPE_SKIP=1 on the skipped steps.

Fail safe: an unknown path, a missing/unreadable last-full-gate mark, a diff that cannot be computed, --nightly, or a
mark older than NIGHTLY_MAX_AGE_S all mean "run every stage".
"""
import json
import re
import time
from pathlib import Path

FULL_MARK = "build64-ninja/last-full-gate.json"
NIGHTLY_MAX_AGE_S = 30 * 3600
SCOPED_STAGES = ("mods", "video-decode")

_CPP = (".cpp", ".h", ".hpp", ".inc", ".c", ".cc")
_CPP_ROOTS = ("code/", "mclib/", "gameos/", "rendercore/", "guiruntime/", "renderworld/", "gameadapters/", "hostservices/",
              "editor/", "gui/", "editorbridge/")

# Paths each stage exercises (lowercased, repo-relative). A match means the stage must run.
_MODS_PREFIX = ("data/", "mods/", "examples/mods/", "tools/mod_smoke.py", "tools/mod_", "tools/validate_mod_package.py",
                "tools/mc2mod", "tools/mod_install", "data_tools/", "scripts/mod_", "scripts/validate_mod",
                "tests/mods/", "mclib/file", "mclib/fastfile", "mclib/ffile", "mclib/fst_hash", "mclib/inifile",
                "mclib/mod_", "mclib/model_override", "gameos/gameos/gameos_fileio")
# C++ files whose name says VFS / file / FST / loader / mod / mission / save / asset loading.
_MODS_NAME = re.compile(r"(vfs|fst|fastfile|fileio|file_mod|loader|mission|save|asset_?load|manifest|campaign|(^|[/_.])mod([_./]|s/|$))")
_GUI_ASSET = re.compile(r"(asset|load|texture|atlas|manifest|resource)")
_VIDEO_PREFIX = ("scripts/video_decode_smoke.py", "code/mc2video", "code/mc2movie", "gameos/gameos/sim_overlap",
                 "code/controlgui_chat", "code/tick_commit", "tests/unit/test_playmovie_deferral", "tests/unit/test_tick_commit",
                 "gates/gate_rows_0.inc")
_VIDEO_NAME = re.compile(r"(video|movie|decode|mve|bink|ffmpeg|avsync|av_sync|sim_overlap|audio)")
_VIDEO_EXT = (".bik", ".avi", ".mp4", ".mve", ".webm", ".ogv", ".mkv")
# Known to be unrelated to the stage even though change_domains calls them engine/art.
_NEVER = {"video-decode": ("data/",)}
_VIDEO_NEVER = ("tests/", "scripts/", "tools/")


def _norm(path):
    return path.replace("\\", "/").lower()


def _matches(stage, p):
    if stage == "mods":
        if p.startswith(_MODS_PREFIX):
            return True
        if p.startswith("guiruntime/") and _GUI_ASSET.search(p):
            return True
        return p.startswith(_CPP_ROOTS) and p.endswith(_CPP) and bool(_MODS_NAME.search(p))
    if p.startswith(_VIDEO_PREFIX) or p.endswith(_VIDEO_EXT):
        return True
    return p.startswith(_CPP_ROOTS) and p.endswith(_CPP) and bool(_VIDEO_NAME.search(p))


def touches(stage, path, change_domains):
    """True when `path` can change what `stage` proves. Unknown paths (not a known C++ root, not docs/tooling/shaders/art) say True."""
    p = _norm(path)
    if _matches(stage, p):
        return True
    if stage == "video-decode" and p.startswith(_VIDEO_NEVER):
        return False   # tests/, scripts/ and tools/ never force video unless named above (W123 over-triggered on 16 of them)
    if p.startswith(_NEVER.get(stage, ())):
        return False
    if p.startswith(_CPP_ROOTS) and p.endswith(_CPP):
        return False   # known engine source that does not name this stage's inputs
    return bool(change_domains([p]) - {"docs", "tooling", "shaders", "art"})   # engine-tools / unknown: touches everything


def decide(paths, change_domains, *, nightly=False, mark_age_s=None):
    """{stage: skip reason or None}. None = the stage runs. `paths` None means the diff is unknown."""
    if nightly:
        return {s: None for s in SCOPED_STAGES}
    if paths is None or mark_age_s is None:
        return {s: None for s in SCOPED_STAGES}
    if mark_age_s > NIGHTLY_MAX_AGE_S:
        return {s: None for s in SCOPED_STAGES}   # no all-stage gate in 30 h: this one runs everything
    out = {}
    for stage in SCOPED_STAGES:
        hit = next((p for p in paths if touches(stage, p, change_domains)), None)
        out[stage] = None if hit else f"scope: diff of {len(paths)} path(s) since the last all-stage gate touches no {stage} inputs"
    return out


def read_mark(root, now=None):
    """(head, age seconds) of the last all-stage gate, or (None, None)."""
    try:
        data = json.loads((Path(root) / FULL_MARK).read_text(encoding="utf-8"))
        return data["head"], (now if now is not None else time.time()) - float(data["finished"])
    except (OSError, ValueError, KeyError, TypeError):
        return None, None


def scoped_diff(git, mark_head, head):
    """Paths changed from merge-base(mark_head, head) to head, or None when git cannot say."""
    try:
        base = git("merge-base", mark_head, head)
        return [p for p in git("diff", "--name-only", "-z", base, head).split("\0") if p]
    except Exception:  # noqa: BLE001 - any git failure means "unknown diff": run everything
        return None


def apply_scope(steps, decisions):
    """Mark scope-skipped steps; returns (steps, [skipped stage names])."""
    out, skipped = [], []
    for name, cmd, env in steps:
        reason = decisions.get(name)
        if reason and not env.get("MC2_GATE_SKIP_REASON"):
            env = dict(env, MC2_GATE_SKIP_REASON=reason, MC2_GATE_SCOPE_SKIP="1")
            skipped.append(name)
        out.append((name, cmd, env))
    return out, skipped
