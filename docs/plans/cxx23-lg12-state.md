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

Next: commit by top-level directory; regenerate artifacts; ratchet without baseline
changes; box build/unit/printf corpus/smoke-lite, determinism 03/24, saveload 01/10;
one box lint; archive receipts, clean box lane and remove worktree.
