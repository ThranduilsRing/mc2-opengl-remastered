<!-- doc-status: current | reviewed:2026-10-11 -->
# Lane 3 state (debug lane; worktree A:/Games/mc2-lane3, branch claude/lane3b-mc215 from nifty 257f39925, install mc2-win64-lane3)

Session 2026-10-11 20:15 -> (cap 02:15). Task: find + fix the mc2_15 heap-corruption writer. Previous session's trail: see git
`git show 018c09a06:.claude/cleanup/lane3-state.md` (quarantine/page-guard/ASan evidence, Task B jaegermech data gap).

## What the 19:53 bundle proves (crash_2026-10-11_195315, plain exe cdee50511, frame 3781, std::bad_alloc)
Read with `.claude/cleanup/lane3-diag/mdinfo.py` (pure-Python minidump reader + dbghelp symbolizer; no cdb on this host).
- `reinjectPersistentStatic` iterates `s_persistentStaticStore` (MSVC list nodes: _Next, _Prev, key@+0x10, vector@+0x18). The loop node
  pointer (rbx, saved by `_Insert_counted_range`'s prologue at stack 0x6c7b5cf260) was **0x19a3a270029**: misaligned, pointing into
  the middle of a GpuStaticPropInstance array. So a node's `_Next` field was overwritten.
- The bytes of that value are `29 00 27 3a 9a 01 00 00`: only the LOW TWO BYTES of a valid pointer (0x19a3a27xxxx) were replaced,
  by `)` `\0` -- the tail of a formatted string ending in `)`.
- Reading "key" from the garbage node gave 0x3f8000 (a float 1.0f fragment); `s_bucketsByType[0x3f8000]` created a fresh bucket node
  (0x19a25ca61d0, zeros, reinjCount=0x459a4be2), then `block.size()` = (garbage last - garbage first)/112 = 0x00989e52459a4be2
  (= r15 at the throw) -> `insert` asked for ~4e16 elements -> bad_alloc. The exit code difference (bad_alloc vs 0xC0000374) is only
  which neighbour the same writer hit.
- The earlier 10 quarantine minidumps (crash_2026-10-11_1237..1252) contain the same writer's text: `553810841` `<1 variable byte>`
  `-199839715553810841)` NUL at offset 0 of a freed, 0xDD-filled block. The two numbers differ by exactly 199e15; neither is a
  float32 value and the second is not a double value either -> they are int64 (or double*1e15) renderings; no literal in exe/data.
- Both victims (48-byte and 56->64-byte LFH blocks) sit right after the writer's buffer: an OVERFLOW of a small (<=64 byte)
  process-heap block by 18..47+ bytes. The previous lane's page guard on gos_Malloc blocks <=512 did not fire -> the buffer is not
  from gos_Malloc/operator new (direct malloc/strdup/HeapAlloc, a DLL, or >512 bytes).

## Diagnostic in flight: `.claude/cleanup/lane3-diag/sandwich_patch.py` (apply|revert; NEVER commit)
Interleaved canary/hole malloc blocks in classes 32..128 (3 waves, frames 2/400/1200), verified every frame from draw_screen; a hit
prints `[SANDWICH] HIT` + hex/ASCII of the neighbourhood (the previous block = the writer's buffer = the string's start) to stderr
(captured in `tests/smoke/artifacts/<ts>/mc2_15.log`) and arms a DR0 write watchpoint on every thread so the next write of the same
address produces a crash bundle with the writer's stack.

## Runs this session (mc2_15 tier2, lane3 install)
| # | exe | result | fps | frame | note |
|---|-----|--------|-----|-------|------|
| 1 | sandwich diag | (running) | | | |

## Hygiene
- build64-ninja configured + built in A:/Games/mc2-lane3 (untracked). Install holds crashes/ bundles (keep: evidence).
- Nothing pushed; diagnostic patches applied by script only.
