<!-- doc-status: stale | reviewed:2026-10-11 -->
# FIT-PARSE-HARNESS-1-RECON

**Status:** RECON ONLY — no code changes. **Verdict: DEFER** (parser is welded to
the File/FST/heap/gameos subsystem; no clean game-free link).
**Branch / worktree:** `claude/fit-parse-harness-recon-1` @
`A:/Games/mc2-fit-parse-recon`, off nifty `ec410d6b`.
**Parent arc:** [subsystem-harness-arc-1.md](subsystem-harness-arc-1.md).

## Recon questions — answered

### Which fit/inifile parser code can link game-free? — **None cleanly.**
The parser is `mclib/inifile.cpp` (2858 lines), class **`FitIniFile : File`**.
`File` (`mclib/file.cpp`) includes `ffile.h`, `packet.h`, `fastfile.h`,
`utilities.h`, `platform_io.h`, `platform_windows.h`, **`gameos.hpp`**, `heap.h`.
`inifile.cpp` itself also includes `heap.h`, `err.h`, `gameos.hpp`. Linking the
parser pulls the **File / FST archive / packet / heap / gameos** subsystem.

### Do the `readId*Array` helpers enforce capacity? — **YES (good targets, but unreachable cheaply).**
`FitIniFile::readIdIntArray/readIdFloatArray/readIdLongArray`
(inifile.cpp:1523/1619/1714) all take an explicit `numElements` capacity and
enforce it well:
- `numDigits > 9` → `TOO_MANY_ELEMENTS`
- `actualElements > numElements` → `USER_ARRAY_TOO_SMALL` (caller capacity respected)
- bounded `getNextWord(equalSign, elementString, 9)` tokenizer
- `NOT_ENOUGH_ELEMENTS_FOR_ARRAY` / `SYNTAX_ERROR` fail-safe paths

These are exactly the malformed-input edge cases worth proving cheaply — **but**
they read via `File::seek` / `File::readLine`, so calling them needs an opened
`File` with a loaded buffer, not just pure-buffer logic.

### Which callers still trust parsed indices? — out of scope for this verdict.
(Component/inventory-index validation lives in the callers — mech.cpp,
gvehicl.cpp, warrior.cpp, saveload.cpp. Testing them would pull those TUs too;
strictly worse than the parser itself.)

### Can malformed `.fit`/`.mdf` be temp-file fixtures, no mission data? — **YES**, but…
`File::open` (file.cpp) has a plain `_open(fileName, _O_RDONLY|_O_BINARY)`
loose-file fallback when `TryModOpen` returns false (no FST mounted). So a temp
fixture file on disk **would** open without mission data — *if the link and
runtime deps are satisfied*. They are not cheap: `File::open` requires
`systemHeap->Malloc` (heap must be initialized), `gosASSERT` (gameos), `S_strlwr`
(platform_str), and `TryModOpen` (fastfile/FST — returns false cleanly only after
its statics are in a sane state).

### Can the harness call real parser functions, not duplicate them? — only by standing up a subsystem.
To exercise `readIdIntArray` against a temp fixture the harness must link
`inifile.cpp + file.cpp + ffile.cpp + fastfile.cpp + packet.cpp + heap.cpp +
err.cpp + platform_* + utilities.cpp`, **initialize `systemHeap`**, and **provide
a gameos shim** for `gosASSERT` (and any `gos_*` the chain references). That is no
longer "link a clean TU" (the render-state model) — it is "stand up the file
subsystem + stub gameos," which crosses the arc's **over-stubbing** guardrail
(stubs can mask the very bug under test) and the <1s / clean-link principle.

## Why this differs from RENDER-STATE-CONTRACT-HARNESS-1

RENDER-STATE linked `render_contract.cpp` cleanly because the **tested paths
(scope/order) make no GL calls** — glew was a link-time header dep only. Here the
**tested path (`readIdIntArray`) is genuinely I/O-coupled** (`seek`/`readLine`),
and `File::open` needs a live `systemHeap` + `TryModOpen` + `gosASSERT`. The
analogy fails: this is real subsystem standup, not a clean TU link.

## Decision (per arc decision tree)

- ❌ "parser functions link cleanly → implement" — they do **not**.
- ⚠️ "tiny extraction → decide if worth a tier1-gated touch" — the pure helpers
  (`getNextWord`, `textToInt/Long/ULong`) are extractable, but the **array
  capacity guards are welded to `File` streaming** (`seek`/`readLine`). Extracting
  them means refactoring File-streaming into a buffer-cursor abstraction —
  **non-trivial, behavior-risky, load-bearing parse code**. Not justified absent
  an active bug.
- ✅ "drags the engine → defer and pick another target" — **this.**

**DEFER FIT-PARSE-HARNESS-1.** No clean game-free link; the only paths are a
heavy "file-subsystem + gameos-shim" harness (over-stubbing risk) or a
non-trivial production extraction of load-bearing parse code (no active bug to
justify the tier1-gated touch).

## Notes for a future revisit
- `File` has an in-RAM mode (`inRAM` / `fileImage`, file.cpp). If a clean
  "construct File from a memory image, no disk, no FST" entry point exists or is
  added for an independent reason, the parser harness becomes much cheaper —
  revisit then.
- If a real `.fit`/`.mdf` parsing bug surfaces, the capacity guards above are the
  test targets, and the heavier harness (or the in-RAM path) becomes justified.

## Recommended next harness target
Pick another **clean-TU-link or filesystem-contract** target rather than a
subsystem-standup one. Candidates that fit the proven tiers:
- A pure-logic TU with a narrow include surface (like `render_contract.cpp`).
- A second filesystem-contract harness (like the shader harness) — e.g. a
  **deploy-manifest / asset-inventory contract** (assets referenced by
  `deploy_payload.py` / manifests exist on disk; no removed assets referenced).
Avoid `txmmgr` (GL-coupled + currently foreign-WIP-dirty) and `objmgr`
(deferred — already hardened, needs production extraction).
```
git log --oneline -G "<symbols>" -i   # recon against CURRENT nifty HEAD first
```
