<!-- doc-status: current | reviewed:2026-10-11 -->
# MC2 Mod Virtual Filesystem — Layered Override Model

**Status:** Strategy / design doc (long-term), 2026-06-11.
**Scope:** The filesystem layer underneath the packaging and registry strategies: how a path string becomes bytes, in what layer order, with what trace, and which layer receives writes.
**Siblings:** `mod-packaging-deploy-architecture.md` (owns mod folder layout, package manifest, install/conflict model — this doc implements its §1 consequence "`mods/<id>/` is already the runtime contract"), `data-ownership-registry-strategy.md` (owns who may write which record; its §1.3 "mod overrides resolve at the file layer" *is this layer*), `mc2-modding-toolchain-architecture.md`, `runtime-bridge-architecture.md`. Nothing here contradicts those docs; where they own a decision, this doc cites it.

---

## 1. North star

> **One resolution function, four layers, zero ambiguity. Any tool, any time, can ask "for path P under mod chain C, which file wins and why?" and get the same answer the engine gives — as data, not as a guess. Reads fall through the stack; writes never do: every write has exactly one legal destination layer, and the base game is never it.**

Load-bearing consequences:

1. **The existing `g_modIndex` overlay (`mclib/file.cpp:62-120, 480-560`) is the kernel, not a prototype.** The layer stack below is a generalization of what already ships (active mod > deps > base > FastFile > CD), adding only a *scratch* layer on top. No VFS abstraction class, no mount table object, no rewrite of `File::open`.
2. **Resolution is a pure function of (path, layer stack, on-disk state).** No runtime mutation of precedence, no per-file pinning, no "load order" state outside the active mod's `dependencies` list (`mod.json`, parsed at `file.cpp:393-411 ReadModJson`).
3. **Tools predict, the engine decides** (packaging doc §9). The queryable trace (§5) exists so the registry index (`data-ownership-registry-strategy.md` §4) and the telemetry cockpit can *replicate* engine resolution and verify the replica against `MC2_LOG_FILE_RESOLVE` output. Divergence = tool bug, by definition.

---

## 2. Today's resolution path (code-cited)

This is what actually runs, precisely. All citations are `mclib/file.cpp` in this worktree unless noted.

### 2.1 Index construction — `InitModSearchPaths(modsRoot)` (`file.cpp:487-560`)

- `MC2_ACTIVE_MOD` env unset → prints `[mod] base game mode`, `g_modIndex` stays empty, **zero per-open overhead** (`TryModOpen` bails on `g_modIndex.empty()`, `mclib/file.cpp:592`). The env var is set by the launcher/bat before spawning mc2.exe (`file.cpp:64`).
- Set → reads `mods/<id>/mod.json` for `id`, `name`, `dependencies[]` via the minimal extractor (`JsonGetString`/`JsonGetStringArray`, `file.cpp:350-390`). Missing `mod.json` → folder name becomes id, no deps (`file.cpp:516-521`).
- Indexing order is **highest priority first, first-wins**: active mod's `data/` (`file.cpp:536-546`), then `dep[0]`, `dep[1]`, … (`file.cpp:549-566`). A later (lower-priority) file at an already-indexed key is dropped and logged as `[mod-dup] key winner=[...] loser=[...]` when `MC2_LOG_FILE_RESOLVE=1` (`file.cpp:147-152` inside `IndexModData`).
- Effective priority: **active mod > dep[0] > … > dep[N-1] > base `data/` > FastFile > CD** (`file.cpp:68`, and the printed load-order banner at `file.cpp:526-533`).
- Per-layer disk walk is cached in `mods/<id>/.modindex-cache` (tab-separated text, `file.cpp:155-345`), invalidated by a two-level mtime sweep of `data/` + immediate subdirs (`GetModDataMtime`, `file.cpp:170-204`); `MC2_REBUILD_MOD_CACHE=1` forces a rescan (`file.cpp:296`).

### 2.2 Per-open resolution — `File::open(fName, READ, ...)` (`file.cpp:730+`)

In order, for read mode:

1. **Normalize:** lowercase unless `doNotLower` (`S_strlwr`, `file.cpp:743-744`), then backslash→forward-slash so FST elfHash keys match (`file.cpp:746-752`, see `fastfile.cpp:86` comment).
2. **Mod overlay:** `TryModOpen` (`file.cpp:412-431`) — gate via `ShouldSearchMods` (`file.cpp:59-65`: non-empty, not absolute, no `..`, must start `data/` or `data\`), key via `NormalizeKey` (`file.cpp:67-71`: slashes→`/`, lowercase), O(1) hash lookup, `_open` the absolute winner path. Logs `[mod-hit] [modId] key -> abspath` or `[mod-miss] key`.
3. **Base loose file:** plain `_open(fileName)` relative to CWD = the deploy root (`file.cpp:780`).
4. **Numeric-size-subdir strip:** `data/tgl/128/foo.tga` → `data/tgl/foo.tga` for upscaled loose overrides (`file.cpp:788-806`).
5. **FastFile:** `FastFileFind` (`mclib/fastfile.cpp:74-110`) — elfHash lookup across mounted `.fst` archives.
6. **CD path:** `CDInstallPath + fileName` (`file.cpp:823-860`), with the legacy insert-CD MessageBox loop. Gated by `Environment.checkCDForFiles`.

`fileExists()` (`file.cpp:601-620`) mirrors steps 2/3/5 (mod → `_stat` → FastFile) and is the existence oracle most callers use. `LookupModOwner()` (`file.cpp:434-441`) exposes "which mod owns this key" for diagnostics.

### 2.3 Write path — `File::open(CREATE)` / `File::create` (`file.cpp:754-762, 1111-1130`)

`CREATE` mode does a direct `_open(fileName, _O_CREAT|_O_TRUNC|...)` at the literal path. **There is no layer routing on writes today** — whatever path the caller passes is where bytes land. The editor saves mission `.pak`/`.fit` via this path; the game writes saves and logistics CSVs the same way. This is the gap §6 closes.

### 2.4 Notes on referenced-but-absent code

- `ModHasMissions` (cited in project memory as a `file.cpp` fix for mc2x-compat campaign classification) is **not present in this worktree's `mclib/file.cpp`** — it lives in the root checkout's dirty working tree / another branch lineage. The design below does not depend on it; mod-mission discovery here goes through `EnumerateModCampaignFiles` (`file.cpp:445-463`) and `EnumerateModFitFiles` (`file.cpp:465-485`), which ARE in this worktree.
- `mclib/ffile.cpp` / `fastfile.cpp` are the FastFile (`.fst`) readers; they are a read-only base-distribution format and **stay below all mod layers, unchanged** (packaging doc §9 anti-goal: no FST changes).

---

## 3. Proposed layer stack

Four logical layers, top wins. Layers 1–3 exist today; layer 0 is new.

| # | Layer | Backing | Mutability | Exists today? |
|---|---|---|---|---|
| 0 | **Scratch overrides** | `mods/<activeId>/.scratch/data/...` (or `MC2_SCRATCH_DIR`) | tools + hand edit; engine read-only | NEW (§7) |
| 1 | **Active mod project** | `mods/<activeId>/data/...` | editor/viewer/cook writes land here (§6) | YES — `g_modIndex` active layer |
| 2 | **Installed packaged mods** = the active mod's dependency chain | `mods/<depId>/data/...`, dep[0] > … > dep[N-1] | read-only at runtime (installer-managed, packaging doc §4-5) | YES — `g_modIndex` dep layers |
| 3 | **Base game** | deploy `data/` loose files, then FastFile `.fst`, then CD | **never written by mods/tools** (registry doc §2 writer rules: only mc2.exe save data) | YES — fallthrough chain §2.2 |

Decisions:

- **"Installed mods" ≠ "all folders in mods/".** Only the active mod's declared dependency chain participates, exactly as today (`file.cpp:549`). A mod sitting in `mods/` but outside the chain is invisible. Composition stays the dependency list (packaging doc §4 "single-active-mod-plus-dependencies stays the runtime model"). This doc does NOT introduce a global load-order.
- **Scratch is a child of the active mod**, not a global layer: scratch experiments are *about* a project, and keeping them inside `mods/<id>/.scratch/` makes promote/diff a sibling-directory operation and keeps the packer's existing dotfile-exclusion rule (packaging doc §2: dotfiles never packaged) doing the right thing for free.
- **Base game internal order is preserved verbatim** (loose > size-strip > FastFile > CD). The numeric-subdir strip (§2.2 step 4) stays a base-layer quirk; mod layers don't replicate it (mods should ship files at their real resolved paths).
- Implementation = **one more indexing pass, not a new mechanism**: `InitModSearchPaths` indexes `.scratch/data/` first (before the active mod's `data/`) into the same `g_modIndex`, tagged with `modId = "<activeId>#scratch"`. First-wins ordering does the rest. Estimated diff: ~30 lines.

---

## 4. Resolution algorithm (normative)

For a read of path `P`:

```
1. norm = lowercase(P), '\'→'/'                  // matches NormalizeKey + File::open prep
2. if !ShouldSearchMods(norm): goto 5            // absolute, '..', or not under data/ → base only
3. if g_modIndex has norm: open winner abspath   // index already encodes layers 0–2 first-wins
4. (miss falls through)
5. open deploy-relative norm                     // base loose
6. numeric-size-subdir strip retry               // base loose, legacy upscale quirk
7. FastFileFind(norm)                            // base archive
8. CDInstallPath + norm                          // base CD (if checkCDForFiles)
9. not found
```

Invariants:

- **Deterministic:** same `(P, MC2_ACTIVE_MOD, MC2_SCRATCH on/off, disk state)` → same result. No randomness, no timing dependence, no per-session state besides the startup index.
- **Whole-file granularity.** A layer overrides a path or it doesn't; there is no merging at the VFS layer (merging is a domain concern — registry doc §2's "whole-file shadow" rows).
- **The index IS the layers-0–2 resolution.** There is no second runtime walk; conflicts were resolved at index time and logged. Tools replicating resolution must replicate *index construction order*, not per-open probing.

## 5. Resolution trace (queryable, for tools + registry index)

Two forms, one schema.

### 5.1 Runtime log (engine, evolution of `MC2_LOG_FILE_RESOLVE`)

Today: `[mod-hit]/[mod-miss]/[mod-dup]` printf lines (`file.cpp:108-112, 419-429`). Evolution, staying printf-grep-friendly (the smoke harness and cockpit already scrape stdout):

- `MC2_LOG_FILE_RESOLVE=1` — current behavior, unchanged (compat).
- `MC2_LOG_FILE_RESOLVE=2` — adds base-layer outcomes: `[resolve] <key> layer=<scratch|mod:<id>|base-loose|base-strip|fastfile:<fst>|cd|MISS>` one line per `File::open`, including non-mod opens. This makes the engine a complete oracle, not just a mod-layer oracle.
- `MC2_RESOLVE_TRACE_FILE=<path>` — same records as level 2, written as JSONL to a file instead of stdout, for the telemetry cockpit (`telemetry-oracle-cockpit-architecture.md`) to tail without parsing interleaved logs. One record:

```json
{"t": 12.345, "key": "data/tgl/abuilding.ini", "layer": "mod:mc2x-pbr",
 "path": "A:/.../mods/mc2x-pbr/data/tgl/abuilding.ini",
 "shadowed": ["mod:mc2x-compat", "base-loose"]}
```

`shadowed` is filled only at trace level 2 (cheap: index-time `[mod-dup]` info + one `_stat` on the base path) — it is the "why", and it is what the registry index's `providedBy` field must agree with.

### 5.2 Static trace (tools): `mc2mod resolve <path> [--mod <id>] [--scratch]`

A subcommand of the packaging doc's `tools/mod_install/` CLI: rebuilds the layer stack in Python using the SAME rules (NormalizeKey, ShouldSearchMods, first-wins dep order, scratch-first) and prints the full candidate ladder with the winner marked. Output schema = the JSONL record above plus all candidates. This is what the registry index builder (`tools/registry/build_index.py`, registry doc §4) calls per-file, and what `mc2mod check` uses for conflict prediction (packaging doc §6). **Parity gate:** a smoke run with `MC2_RESOLVE_TRACE_FILE` diffed against the static trace for the same chain; any mismatch fails CI.

---

## 6. Write routing rules (normative)

Today `File::create` writes wherever the caller points it (§2.3). The rule set — enforced first by tool convention, later by an engine-side guard:

| Writer | Writes | Destination layer |
|---|---|---|
| Editor (mission save) | `.fit`/`.pak` pair | **Layer 1** — `mods/<activeMod>/data/missions/` when a mod project is active (packaging doc §4 editor-discovery already states this); base `data/missions/` ONLY in explicit no-mod "base authoring" mode |
| Editor / tools, experimental toggle on | anything | **Layer 0** scratch (§7) |
| Asset Viewer / Workbench | GLBs, `models.json` records, generated manifests | **Layer 1** `mods/<id>/data/model_overrides/` (packaging doc §8.1) |
| Cook tools | derived sidecars (`.ktx2`, `.burnin.jpg`, `.tgl`) | **same layer as their source input** — a cook of a mod-layer source writes next to it in the mod; base-asset cooks write into base (cook doc taxonomy) |
| mc2.exe runtime | saves, logistics CSVs (`save.fit`, variant CSVs) | **base `data/`** — explicitly NOT mod data (registry doc §2 cross-cutting rules; packaging doc §5.3). Player state is not an override. |
| Installer | mod folders, receipts | layers 1–2 under `mods/` only (packaging doc §9: never outside `mods/`) |

Hard rules:

1. **No tool ever writes a file that shadows base under the base layer itself.** "Edit a base asset" as a modding action = copy-up into layer 1 (or 0) and edit the copy. The original is untouched; uninstall is folder deletion (packaging doc §5).
2. **Dependency layers (2) are immutable at runtime and in the editor.** Editing a dep's file = copy-up into the active project. The installer is the only writer of layer 2.
3. **Engine guard (later slice):** a debug `MC2_WRITE_GUARD=1` mode where `File::open(CREATE)` warns (or fails) when the target is under `data/` while a mod is active and the path is not a known save-data path — catches tool bugs cheaply.
4. **Cache writes are exempt:** `.modindex-cache` (engine, `file.cpp:330-345`) and `.scratch/` metadata are machine-local dotfiles inside `mods/<id>/`, never packaged.

---

## 7. Scratch layer semantics

Purpose: uncommitted experiments — "what does this texture look like in-game" — without dirtying the mod project that may be under version control or mid-package.

- **Location:** `mods/<activeId>/.scratch/data/...` (mirror of `data/`). Override root via `MC2_SCRATCH_DIR` for tools that want a shared scratch outside the deploy.
- **Activation:** `MC2_SCRATCH=1` env (default OFF — scratch must be opt-in so a forgotten experiment can't haunt a packaging run or a smoke). When on, indexed at top priority (§3); banner line `[mod] scratch layer ACTIVE: N files` so it is impossible to miss in logs.
- **Diff:** `mc2mod scratch diff` = walk `.scratch/data/` vs `data/`, per-file status (`new` / `overrides-project` / `overrides-dep:<id>` / `overrides-base`) computed via the §5.2 static resolver, plus hash-equality for "identical, drop it".
- **Promote:** `mc2mod scratch promote [paths...]` = move file(s) `.scratch/data/X` → `data/X` (same volume rename, atomic enough), then touch `data/` so `.modindex-cache` invalidates. `--all`, `--discard` for the inverse. Promotion never targets layers 2–3 (rule 6.2).
- **Lifecycle:** scratch is *expected* to be deleted casually. Nothing else references it: never packaged (dotdir), never in the registry index by default (index builder may include it with `--with-scratch` for cockpit views, marked `providedBy: "<id>#scratch"`).
- **Editor integration (later):** an "experimental save" toggle that routes saves to scratch and shows a scratch-status badge per asset — pure UI over the same folder.

---

## 8. PacketFile boundary statement

**Packet-internal overrides are OUT OF SCOPE for the virtual filesystem — permanently, by design.**

- The VFS layer resolves at **file** granularity. A `.pak` (`mclib/packet.h:56 class PacketFile`, positional packets via `seekPacket`/`writePacket`, `packet.h:101/128`) is one file; overriding it means shadowing the whole `.pak` at a higher layer. This is already the supported grain: mission overrides ship the `.fit`+`.pak` pair together (registry doc §2 missions row), and `object2.pak` replacement is wholesale (registry doc §3.1).
- Per-packet override (e.g. "replace only objTypeNum 2201's packet") would require a packet-aware resolution shim inside `PacketFile::seekPacket` — that is **engine format work owned by the registry/packaging lane's future-work ledger** (packaging doc §10 "whole-file shadowing of monoliths… finer granularity is future engine work"), not a VFS feature. If it ever lands, it lands as a domain loader feature, and the VFS contract here does not change.
- Corollary the tools must respect: the registry index records `.pak` packet *counts/slot-presence* for validation (registry doc §3.3) but the resolve trace for anything inside a pak is simply the trace of the pak file itself. PacketFile child-file opens (`File::open(FilePtr parent,...)`, `file.cpp:996`) read through the already-resolved parent handle and need no VFS awareness.

The same statement applies a fortiori to FastFile members: the FST is below all mod layers; mods override its members by shipping the loose file (overlay outranks FastFile, §2.2), never by patching archives.

---

## 9. Caching + invalidation

- **Per-lookup cost today:** with no mod, one `empty()` check (≈free, `file.cpp:413`). With mods: one string normalize + one hash probe per `File::open` — O(1), no disk probing (`file.cpp:69` comment). The scratch layer adds zero per-lookup cost (same index). **No change needed**; per-open perf is a solved problem here.
- **Startup cost:** per-layer directory walk, amortized by `.modindex-cache` (§2.1). Scratch gets its own cache file `.scratch-index-cache` OR — simpler, preferred — **scratch is always walked fresh** (it is small by construction and opt-in; a fresh walk of dozens of files is microseconds and dodges the staleness class entirely).
- **Invalidation triggers:**
  - Layer content change (install/uninstall/promote/file edit) → mtime sweep catches it at next launch; the known hole is the 2-level sweep missing deep edits (packaging doc §10 row 1) → tools that mutate layers (`mc2mod install/uninstall`, `scratch promote`, editor save) **touch the layer's `data/` dir** as a contract, and dev-link launches set `MC2_REBUILD_MOD_CACHE=1`.
  - Chain change (`mod.json` deps edited, `MC2_ACTIVE_MOD` changed) → index is rebuilt every launch from the chain anyway; caches are per-mod-layer and chain-independent, so they remain valid. Nothing to do.
  - **No in-session invalidation.** The index is session-frozen (mod selected at launch, `file.cpp:444-447` no-op Activate/Deactivate stubs are deliberate). Hot-reload of layers is an anti-goal (§10); the iteration loop is relaunch (fast) or the editor's own asset-reload paths.
- **Registry-index coupling:** the registry index records the `modChain` + input mtimes it was built against (registry doc §4 schema); VFS layer changes make it stale, and staleness is detected by the index's own `inputs[]` mtime check — the VFS does not push invalidations anywhere.

---

## 10. Diagnostics

- `MC2_LOG_FILE_RESOLVE=1` — today's `[mod-hit]/[mod-miss]/[mod-dup]` (kept verbatim).
- `MC2_LOG_FILE_RESOLVE=2` — full-ladder `[resolve]` lines incl. base/fastfile/cd/MISS (§5.1).
- `MC2_RESOLVE_TRACE_FILE=<path>` — JSONL trace for the cockpit; cockpit panel shows live per-layer hit counts, top-N shadowed files, and MISS list (a MISS spike during a mission is an early asset-bug signal for smokes).
- `mc2mod resolve` / `mc2mod scratch diff` — static side (§5.2, §7).
- **Parity smoke:** CI job runs tier1 with trace-file on under `MC2_ACTIVE_MOD=mc2x-compat`, diffs engine trace vs static resolver. This is the registry doc's "engine remains the oracle" made executable.
- Startup banner already prints the load order (`file.cpp:526-533`); extend with scratch line and per-layer file counts (mostly already there, `file.cpp:543-565`).

---

## 11. Anti-goals (binding)

- **No VFS abstraction layer / mount-table object / IFileSystem interface.** The mechanism stays: one hash index + the legacy fallthrough in `File::open`.
- **No packet-internal or archive-internal overrides** (§8). No FST changes.
- **No multi-active-mod global load order** — composition is the dependency chain (packaging doc §9).
- **No in-session layer hot-reload** — index is launch-frozen.
- **No write-through resolution** ("open for write resolves like read") — writes are explicitly routed (§6), never resolved.
- **No engine reads of tool artifacts** — engine never opens `package.json`, registry index, scratch metadata, receipts.
- **No case-sensitivity support.** Keys are lowercase-forward-slash forever (§12); mods authored with case-colliding paths are invalid.

## 12. Case + path normalization rules (Windows, normative)

- Canonical key = `NormalizeKey` (`file.cpp:67-71`): `\`→`/`, ASCII `tolower`. All layers, all tools, all trace records use this key form. (`File::open` additionally lowercases the *stored* filename unless `doNotLower`, `file.cpp:743` — tools must not rely on on-disk case.)
- Only `data/...`-relative paths participate in layering (`ShouldSearchMods`, `file.cpp:59-65`); absolute paths and anything containing `..` bypass mods entirely — this is also the traversal-safety boundary and MUST be replicated by the static resolver.
- On-disk file names in mod projects SHOULD be lowercase; the packer warns (not blocks) on mixed case and on two project files whose normalized keys collide (NTFS allows `Foo.tga`+`foo.tga` only on case-sensitive dirs, but a zip can carry both — the validator rejects that).
- Non-ASCII path bytes: undefined behavior today (ANSI Win32 APIs, `FindFirstFileA` etc.); the validator rejects non-ASCII paths in packages rather than pretending to support them.

---

## 13. Risks

| Risk | Mitigation |
|---|---|
| Scratch layer forgotten ON during packaging/smoke → ghost overrides | default OFF, loud banner, packer refuses to pack while `MC2_SCRATCH=1` is set in its env; scratch never packaged (dotdir) |
| 2-level mtime sweep misses deep scratch/project edits | scratch always walked fresh; mutating tools touch `data/`; dev-link uses `MC2_REBUILD_MOD_CACHE=1` (existing pattern) |
| Static resolver drifts from engine (the split-brain class this project keeps re-finding) | parity smoke (§10) is a CI gate, not a someday; the resolver is one small Python module shared by `mc2mod` and `build_index.py` — one replica, not N |
| Trace level 2 perf (printf per open during load storms) | JSONL file path buffered; level 2 documented as diagnostic-only, never in default smoke env |
| Editor saves to wrong layer (deploy split-brain v0.4 vs 0.4c is a known trap) | editor shows active-project + deploy root in the title bar; write-guard env (§6.3) in dev builds |
| `ShouldSearchMods` `data/`-prefix gate silently exempts oddball paths (e.g. CWD-relative non-data reads) from modding | acceptable: those are engine-internal files; documented here as the boundary, validator warns if a package contains files outside `data/` |

## 14. Phased roadmap

1. **P0 — Document + freeze the contract** (this doc) and land `MC2_LOG_FILE_RESOLVE=2` + `MC2_RESOLVE_TRACE_FILE` in `file.cpp`. Pure additive logging.
2. **P1 — Static resolver module** (`tools/mod_install/resolver.py`) + `mc2mod resolve` + parity smoke vs engine trace.
3. **P2 — Scratch layer**: engine indexing pass (`MC2_SCRATCH`), `mc2mod scratch diff/promote/discard`.
4. **P3 — Write routing**: editor active-project save targeting (mostly exists per packaging doc §4), `MC2_WRITE_GUARD`, cook same-layer rule.
5. **P4 — Cockpit integration**: resolve-trace panel, MISS/shadow dashboards; registry index consumes the shared resolver.

## 15. First 5 implementation slices

1. **`MC2_LOG_FILE_RESOLVE=2` + JSONL trace file** in `mclib/file.cpp` (`TryModOpen` + the base/fastfile/cd outcomes in `File::open`). ~80 lines, env-gated, zero cost off. Gate: tier1 unchanged with vars unset; trace file non-empty + well-formed with vars set.
2. **`resolver.py`** replicating `NormalizeKey`/`ShouldSearchMods`/first-wins chain build, + `mc2mod resolve <path>`. Gate: unit tests on the documented quirks (case, backslash, `..`, dup keys, missing mod.json).
3. **Parity smoke**: run one tier1 mission with `MC2_ACTIVE_MOD=mc2x-compat` + trace file; script diffs every traced mod-layer decision vs resolver prediction. Gate: 0 mismatches.
4. **Scratch indexing pass** in `InitModSearchPaths` (`MC2_SCRATCH=1`, walk `.scratch/data/` first, banner, always-fresh walk). ~30 lines. Gate: scratch file shadows project file in-game; vars unset → byte-identical behavior.
5. **`mc2mod scratch diff/promote/discard`** using the shared resolver + atomic rename + `data/` dir touch. Gate: promote then relaunch resolves to promoted file without `MC2_REBUILD_MOD_CACHE`.

## 16. Follow-up prompts (for Opus/Codex)

1. *"Implement slice 1 from `docs/superpowers/strategy/mod-virtual-filesystem-design.md` §15: extend `mclib/file.cpp` with `MC2_LOG_FILE_RESOLVE=2` full-ladder logging and `MC2_RESOLVE_TRACE_FILE` JSONL output covering all six resolution steps in `File::open` (mod/base/strip/fastfile/cd/miss). Env-gated, zero cost when unset, no behavior change to resolution itself. Verify with tier1 smoke (vars unset) + a manual run with `MC2_ACTIVE_MOD=mc2x-compat` and both vars set."*
2. *"Write `tools/mod_install/resolver.py` per §5.2/§15.2 of `mod-virtual-filesystem-design.md`: a pure-Python replica of `mclib/file.cpp` resolution (NormalizeKey, ShouldSearchMods, mod.json dep-chain first-wins index, scratch layer) with a candidate-ladder JSON output, plus pytest coverage of every normalization quirk cited in §12. Then add the parity diff script (§15.3) comparing a `MC2_RESOLVE_TRACE_FILE` run against resolver predictions."*
3. *"Adversarial review of `mod-virtual-filesystem-design.md` against `mod-packaging-deploy-architecture.md` and `data-ownership-registry-strategy.md`: hunt for contradictions in write-routing rules, conflict-model ownership, scratch-layer interactions with the packer's dotfile exclusion, and any place the static resolver could diverge from `mclib/file.cpp` (especially the numeric-subdir strip and `doNotLower` paths). Output a findings table with severity and proposed doc edits."*
