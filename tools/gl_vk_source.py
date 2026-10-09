#!/usr/bin/env python3
"""gl_vk_source.py -- repo reader shared by the GL<->VK feature-coverage tools.

WHY: other lanes edit the working tree continuously. The coverage tool must be able to
grade a fixed git revision (--ref HEAD) as well as the working tree, and the unit tests
need an in-memory repo. All three look like one `Source` to the callers.
"""
from __future__ import annotations

import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Source:
    """Read-only view of a repo tree: the working tree, a git ref, or a dict."""

    def __init__(self, root=ROOT, ref=None, files=None):
        self.root, self.ref, self._mem = root, ref, files
        self._ls = None
        self._cache = {}

    def _git(self, *args):
        r = subprocess.run(["git", *args], cwd=self.root, capture_output=True)
        if r.returncode != 0:
            raise RuntimeError("git %s failed: %s" % (" ".join(args), r.stderr.decode("utf-8", "replace")))
        return r.stdout

    def paths(self):
        """Every tracked path (forward slashes), sorted."""
        if self._ls is None:
            if self._mem is not None:
                self._ls = sorted(self._mem)
            elif self.ref:
                out = self._git("ls-tree", "-r", "--name-only", self.ref)
                self._ls = sorted(out.decode("utf-8", "replace").splitlines())
            else:
                out = self._git("ls-files")
                self._ls = sorted(p for p in out.decode("utf-8", "replace").splitlines()
                                  if os.path.isfile(os.path.join(self.root, p)))
        return self._ls

    def glob(self, prefix, exts=None):
        return [p for p in self.paths()
                if p.startswith(prefix) and (exts is None or p.endswith(tuple(exts)))]

    def exists(self, path):
        return self.text(path) is not None

    def text(self, path):
        if path in self._cache:
            return self._cache[path]
        t = None
        if self._mem is not None:
            t = self._mem.get(path)
        elif self.ref:
            r = subprocess.run(["git", "show", "%s:%s" % (self.ref, path)], cwd=self.root,
                               capture_output=True)
            if r.returncode == 0:
                t = r.stdout.decode("utf-8", "replace")
        else:
            p = os.path.join(self.root, path)
            if os.path.isfile(p):
                with open(p, "rb") as f:
                    t = f.read().decode("utf-8", "replace")
        if t is not None:
            t = t.replace("\r\n", "\n")
        self._cache[path] = t
        return t

    def preload(self, paths):
        """Fetch many blobs in one git process (ref mode); a no-op otherwise."""
        if not self.ref or self._mem is not None:
            return
        todo = [p for p in paths if p not in self._cache]
        if not todo:
            return
        req = "".join("%s:%s\n" % (self.ref, p) for p in todo).encode()
        r = subprocess.run(["git", "cat-file", "--batch"], cwd=self.root, input=req,
                           capture_output=True)
        buf, i = r.stdout, 0
        for p in todo:
            nl = buf.index(b"\n", i)
            head = buf[i:nl].split()
            if len(head) >= 2 and head[1] == b"missing":
                self._cache[p] = None
                i = nl + 1
                continue
            size = int(head[2])
            body = buf[nl + 1:nl + 1 + size]
            self._cache[p] = body.decode("utf-8", "replace").replace("\r\n", "\n")
            i = nl + 1 + size + 1


# --- lexing helpers ---------------------------------------------------------

_LEX_RE = re.compile(
    r"//[^\n]*"                      # line comment
    r"|/\*.*?\*/"                    # block comment
    r"|\"(?:\\.|[^\"\\\n])*\""       # string literal
    r"|'(?:\\.|[^'\\\n])*'",         # char literal
    re.S)


def _blank(seg):
    if "\n" not in seg:
        return " " * len(seg)
    return "\n".join(" " * len(line) for line in seg.split("\n"))


def blank_comments(src, keep_strings=True):
    """Comments (and, if keep_strings is False, string/char literal bodies) replaced by
    spaces. Length and newlines are preserved so offsets and line numbers stay valid."""
    def sub(m):
        s = m.group(0)
        if s.startswith("/"):
            return _blank(s)
        if keep_strings or len(s) < 2:
            return s
        return s[0] + _blank(s[1:-1]) + s[-1]
    return _LEX_RE.sub(sub, src)


def line_of(text, offset):
    return text.count("\n", 0, offset) + 1


_TOKEN_CACHE = {}


def has_token(text, token):
    """Whole-identifier occurrence test (comments must already be blanked)."""
    rx = _TOKEN_CACHE.get(token)
    if rx is None:
        rx = _TOKEN_CACHE[token] = re.compile(r"(?<![A-Za-z0-9_])" + re.escape(token) + r"(?![A-Za-z0-9_])")
    return rx.search(text) is not None
