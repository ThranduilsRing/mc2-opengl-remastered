<!-- doc-status: current | reviewed:2026-10-11 -->
# Cite guard

WHAT: `tools/cite_guard.py` finds the `path:line` cites in docs that the giant-file splits made wrong, proposes the exact
replacement, and proves a cheap model's edit changed only cites (plus one status header line). WHY: a model cannot be trusted
to repair a cite without also "improving" the prose around it, so the proof is mechanical and fails closed.

## Loop (Sonnet orchestrator + cheap model)

1. `py -3 tools/cite_guard.py --triage-report [dir]`: one line per doc with cites, broken, stale-line, last commit, status
   header and a newer doc citing the same paths/symbols (Jaccard >= 0.6). The owner decides per doc: repair, mark `stale`, or
   mark `superseded-by`.
2. Per doc to repair: `--fix-suggest <doc>`. A `REPLACE` row names the old and new cite text to apply verbatim; a `KEEP` row has
   no unique answer (deleted file, ambiguous symbol, unknown path), so the cite stays or the doc is marked stale.
3. The model applies only `REPLACE` rows and may add the status header.
4. `--diff-guard --base HEAD <doc>...` exits 1 on any other byte change and names the first divergent line pair. Revert the doc
   on failure; commit one directory at a time by explicit pathspec.

`--check [paths]` verifies cites against HEAD (exit 1 on a finding). It is not a contract row yet: the repo is red by design
until the triage pass lands. Classes: SPLIT, DELETED (commit and date), RENAMED, UNKNOWN, STALE-LINE (line missing, symbol not
within +-3 lines, or line differs from the pre-split file), MISSING-SYMBOL.

Status header, first line only: `<!-- doc-status: current|stale|superseded-by:<path> | reviewed:<YYYY-MM-DD> -->`; the path must be tracked.

## What the guard accepts
A replacement must check out (file, line, symbol), keep the cite's shape and symbol, and, with no symbol, land on the only
candidate (content match against the pre-split file) or stay in the same file, split unit or rename target. A valid symbol-less
cite changes only by gaining a symbol that sits there.

## Exempt (INFO, immutable)
Cites in a fence tagged `text`/`log`/`console`/`output` or holding `[X v1]`, PASS, FAIL or a sha; a line with `(quoted history)`;
the block after `<!-- history -->`. Not scanned: `docs/archive/**`, `build64*`, untracked docs (unless `--include-untracked`),
data lists (`*allowlist*.txt`, `*baseline*.txt`).

## What the reviewer samples
Whether a moved cite still means the same thing: a symbol within 3 lines proves location, not intent, and a content match
assumes the doc was written against the pre-split file. Sample `REPLACE` rows with several candidates and any doc where more
than half the cites moved.
