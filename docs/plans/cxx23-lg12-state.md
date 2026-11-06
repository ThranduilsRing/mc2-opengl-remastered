# LG12 handoff state

Assignment: mc2-cxxlg12 / claude/cxx23-lg12-format, base 09eef9872.
Reviewed both inherited commits (7 files): field widths, precision, percent escapes,
puts newlines, stream destinations, null strings and integer/bool arguments.
Retained the two MOVE_BD printf calls with %p: MSVC and Linux spell pointers
differently, so {:016X} was not byte-identical on Linux.
Extended GameOS only: five shutdown messages and two bounded buffers.
Cache key: 16 hex digits plus NUL fit the existing 20-byte array; file-name
formatting retains the existing array bound and snprintf truncation behavior.
Runtime/resource formats, crash paths and other lanes' files are untouched.

2026-11-06: rebased all five lane commits onto nifty 10a95ed41 without conflicts.
Reviewed the resulting conversion: stream destinations, precision, field widths,
newlines and flushes are preserved. Log formatting uses an unbounded string;
the cache key fits its array and the filename keeps its pre-existing API bound.
Ratchet --check --worktree: OK, 52 fewer legacy sites than baseline.
No derived inventories or baselines regenerated.
Next at final committed tip: box build unit smoke-lite determinism:mc2_03 saveload;
then box lint, preserve receipts and box_run --cleanup. Keep this worktree.
Rebase obstruction: pre-existing untracked sentry-native moved to
G:/mc2-tmp/lg12-sentry-preserved; git diff --no-index found it identical to the
newly tracked directory. Automatic approval review rejected duplicate deletion;
that cleanup step is skipped, and the preserved directory remains.
