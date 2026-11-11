"""--quiet support for chatty scripts: show only what an agent needs, keep everything in a log.

install_quiet(log_path) redirects fd 1 and fd 2 (child processes included) through a filter. Every line goes to the
log file; only these reach the console:
  * verdict / result / PASS / FAIL / FATAL / ERROR / WARN / MISMATCH / STALE / MISSING / Traceback lines,
  * markdown table rows (gate rows) and the text after a "Failures" heading,
  * the DEPLOY_FINGERPRINT lines,
  * lines that name an artifact / log / report / proof path.
A final "[quiet] ... full output: <log>" line tells the reader where the rest is. Default output of every script is
unchanged; only an explicit --quiet turns this on, so existing parsers keep working.
"""
from __future__ import annotations

import atexit
import os
import re
import sys
import tempfile
import threading
import time

KEEP = re.compile(
    r"\b(PASS(ED)?|FAIL(ED|URE|URES)?|FATAL|ERROR|WARN(ING)?|MISMATCH|STALE|MISSING|Traceback|verdict|result)\b"
    r"|DEPLOY_FINGERPRINT|^\s*\||^#{1,4}\s"
    r"|\b(artifacts?|reports?|proof|montage|summary)\b.*([A-Za-z]:[\\/]|\.(log|json|md|png|html|csv)\b)"
    r"|\bfull output\b",
    re.I,
)
# Per-file / per-item noise that would otherwise match KEEP ("deployed x.dll (OK)").
DROP = re.compile(r"\(OK\)\s*$|\bdeployed\b.*\bOK\b|^\s*\[?\w*\]?\s*(copy|copying|skip|skipped|hash)\b", re.I)
AFTER = re.compile(r"^#{1,4}\s*Failures", re.I)  # keep everything after this line


class LineFilter:
    """Feed lines; returns True when the line should be shown. Counts shown/total."""

    def __init__(self, keep=KEEP, drop=DROP, after=AFTER):
        self.keep, self.drop, self.after = keep, drop, after
        self.tail = False
        self.shown = 0
        self.total = 0

    def __call__(self, line: str) -> bool:
        self.total += 1
        text = line.rstrip("\r\n")
        if self.after.search(text):
            self.tail = True
        show = self.tail or (bool(self.keep.search(text)) and not self.drop.search(text))
        if show:
            self.shown += 1
        return show


def filter_stream(src, out, log, flt=None):
    """Copy text lines from src to log (all) and out (filtered). Returns the LineFilter."""
    flt = flt or LineFilter()
    for line in src:
        log.write(line)
        if flt(line):
            out.write(line)
            out.flush()
    return flt


def default_log_path(name: str) -> str:
    base = os.environ.get("MC2_LANE_SCRATCH") or tempfile.gettempdir()
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return os.path.join(base, f"{name}-quiet-{stamp}.log")


def install_quiet(log_path: str | None = None, name: str = "run") -> str:
    """Route fd 1/2 through the filter until process exit. Returns the log path."""
    log_path = log_path or default_log_path(name)
    os.makedirs(os.path.dirname(os.path.abspath(log_path)), exist_ok=True)
    sys.stdout.flush()
    sys.stderr.flush()
    saved = os.dup(1)
    r, w = os.pipe()
    os.dup2(w, 1)
    os.dup2(w, 2)
    os.close(w)
    holder = {}

    def pump():
        src = os.fdopen(r, "r", encoding="utf-8", errors="replace", newline="")
        out = os.fdopen(saved, "w", encoding="utf-8", errors="replace", newline="")
        with open(log_path, "w", encoding="utf-8", errors="replace", newline="") as log:
            flt = filter_stream(src, out, log)
            holder["flt"] = flt
        out.write(f"[quiet] {flt.shown} of {flt.total} lines shown; full output: {log_path}\n")
        out.flush()
        out.close()

    t = threading.Thread(target=pump, name="quiet-pump", daemon=True)
    t.start()

    def restore():
        try:
            sys.stdout.flush()
            sys.stderr.flush()
        except Exception:
            pass
        # Closing our copies of the pipe write end ends the pump (children already exited).
        try:
            nul = os.open(os.devnull, os.O_WRONLY)
            os.dup2(nul, 1)
            os.dup2(nul, 2)
            os.close(nul)
        except OSError:
            pass
        t.join(10)

    atexit.register(restore)
    return log_path


def wanted(args) -> bool:
    # Explicit flag only: an env var would leak into child scripts whose stdout other tools parse.
    return bool(getattr(args, "quiet", False))
