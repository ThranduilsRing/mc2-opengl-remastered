# Example Mods

This folder is a collection of small, working example mods for MechCommander 2 —
each one is a real, verified mechanism in this checkout, not a hypothetical. If
you're brand new to modding MC2, this page is your map: what each example
teaches, how hard it is, and what to read first.

**This collection is about data mods** — mods that ship as a folder under
`mods/<id>/data/...` and are picked up by the game's mod-overlay mechanism
(`MC2_ACTIVE_MOD`), packaged with `tools/mc2mod/mc2mod.py`, no C++ rebuild
required. If you want to change the *engine itself* — add a new render
feature, change game logic, touch `code/`/`mclib/`/shaders — that's a
different, much larger undertaking; see
[`../../modding-guide.md`](../../modding-guide.md) instead. Everything below assumes
you're staying on the data side of that line.

**Start here, in order:**

1. [`getting-started.md`](getting-started.md) — mod folder anatomy,
   `MC2_ACTIVE_MOD`, the launcher picker, `mc2mod.py` packaging, diagnostics.
   Every example below assumes you've read this.
2. [`mechanism-map.md`](mechanism-map.md) — the reference table of every
   moddable mechanism in the engine, with source citations. Use it to look up
   "can I mod X" before you start writing anything.

## Variant (data) vs. mesh (import) — know which one you need

Two very different things both get called "modding a mech" (or a weapon, or a
building), and mixing them up wastes time:

| | **Variant (data)** | **Mesh (import)** |
|---|---|---|
| What it changes | Stats, loadout, names — plain text/CSV rows | The actual 3D geometry — a glTF file swapped in |
| Mechanism | Whole-file CSV/FIT shadow via the mod overlay | `[Import]` block (mechs) or the model-override registry (props/trees) |
| Example | [`variant-add`](variant-add/), [`weapon-edit`](weapon-edit/) | [`mech-import`](mech-import/), [`building-import`](building-import/), [`tree-import`](tree-import/) |
| Difficulty | Low — no art tools needed | Higher — needs a glTF asset and geometry sanity checks |

If you just want a new loadout, a rebalanced weapon, or a renamed unit, you
want a **variant/data** mod. If you want something to *look* different, you
want a **mesh/import** mod.

## The examples, easiest first

The list below is ordered by real difficulty, not by topic — start at the top
even if a later entry looks more interesting, since the earlier ones establish
things (the overlay mechanism, `mc2mod.py`, diagnostics) the later ones lean
on.

### 1. [`texture-override`](texture-override/) — difficulty: **Easy**

The "hello world" of this collection: recolor the mouse-cursor sprite sheet by
shadowing `data/art/cursors1.tga` through the mod overlay. Also ships a sound
override (`data/sound/music27.wav`) for comparison. **Honest status:** the
texture half works exactly as documented end to end; the sound half does
**not** actually change in-game audio in this checkout — the streamed-audio
load path bypasses the mod-aware file resolver entirely (a verified engine
limitation, not a mistake in the mod), and the README documents this
prominently instead of pretending it works.

### 2. [`weapon-edit`](weapon-edit/) — difficulty: **Easy**

Double the PPC's damage by shipping a modified whole-file copy of
`data/objects/compbas.csv`, the master weapon/component table. Straightforward
whole-file CSV shadow, no gotchas in the mechanism itself. (A former doc-only
caveat about the packaged `.mc2mod`'s sha256 not being reproducible across
pack runs is fixed: `mc2mod.py pack` now pins zip timestamps, so identical
payload bytes always produce an identical package hash.)

### 3. [`weapon-add`](weapon-add/) — difficulty: **Easy**

Adds a brand-new weapon ("Prototype Needle Laser") by filling an unused
`Type=undefined` row in `compbas.csv`, then gives it a real display name via a
small string-override FIT file. Builds directly on `weapon-edit` — read that
one first for the `compbas.csv` column reference.

### 4. [`mission-edit`](mission-edit/) — difficulty: **Easy**, several traps

Remix the first campaign mission (`mc2_01`) by shadowing its entire
`data/missions/mc2_01.fit`: add a 15-minute time limit, raise the resource
budget, rename the first objective, and repaint the enemy lance red. Teaches
the full mission file anatomy (which of a mission's six file types you can
shadow — all of them), plus two verified traps: objective/mission names ignore
your literal-string edits unless you also flip their `...UseResourceString`
booleans, and your *own* units always ignore the `.fit` paint colors (player
prefs win — recolor enemies instead). **Honest status:** mechanism claims are
source-verified with anchors and the payload passed the full offline pipeline
(pack/verify/install/uninstall + resolution replica), but no live in-game
capture was taken — the four edits' visible results are code-derived, user
verification pending. Mission *creation* is out of scope (see below).

### 5. [`variant-add`](variant-add/) — difficulty: **Friction**

Adds a new loadout variant of the stock Atlas chassis (pure stats/loadout —
the mesh is untouched; see the variant-vs-mesh table above). **Honest
status:** the mechanism itself is fully shipped and works on this checkout,
but the README's original walkthrough had a fabricated/incorrect source
citation (a `readInt(11,5,...)` call that doesn't exist in `code/mech.cpp`)
that was flagged and needs a fix before you trust that particular line —
everything else in the doc checks out.

### 6. [`building-import`](building-import/) — difficulty: **Easy**, with a caveat

Swaps the stock hangar building's visual mesh for a glTF model via the static-
prop model-override registry. **Honest status:** the registry itself works
exactly as documented, but it does **not** participate in the
`MC2_ACTIVE_MOD` overlay at all — it reads `data/model_overrides/models.json`
via a raw file open, so `mc2mod.py install`'s normal `mods/<id>/...` placement
silently doesn't work for this mechanism; you install to the base game's
`data/model_overrides/` path directly. The README documents this exception
clearly — read it before you package anything.

**Props (and static-prop-class buildings/scenery) use this same pipeline** —
if you're looking to replace any non-mech, non-tree static geometry, this is
the doc to read, not `mech-import`.

### 7. [`tree-import`](tree-import/) — difficulty: **Easy**, with a caveat

Same model-override registry as `building-import`, applied to the stock
`oak1` tree via its own resolve seam. **Honest status:** same overlay
exception as buildings applies here too (confirmed, not just suspected — the
`.glb` read path bypasses the mod resolver one layer down as well), and there
is an additional open limitation: **LOD chains for tree overrides are
mid-work and not usable yet** — this example intentionally ships single-LOD
only, the one configuration that's actually safe today.

### 8. [`mech-import`](mech-import/) — difficulty: **Medium** (imports go last for a reason)

Swaps a mech's LOD0 mesh for a placeholder glTF box-model via the same
`[Import]` opt-in the stock Atlas already uses. This is the most involved
example in the collection — it touches mesh orientation/scale tuning and has
its own overlay caveat. **Honest status:** the `.ini` shadows correctly
through the normal mod overlay, but the paired `.glb` geometry does **not** —
Assimp's reader bypasses the mod resolver entirely, so (like the prop
examples) you install both files as a **direct drop-in to your own game
install's `data/tgl/` folder**, not a `mods/<id>/` package. Read the "Why
this doesn't overlay cleanly" section before you start.

## Live tools (not a distributable mod)

### [`imgui-editing`](imgui-editing/) — Live-tuning with the in-game ImGui panels

This one is different in kind from everything above: it's a guide to the
in-game ImGui debug panels (Graphics Options, Object Inspector, Renderer
Features) for previewing and live-tuning a mod while it runs, plus how to
persist those tweaks to a small `data/visual_tuning.json` file you *can* ship
in a mod overlay. It is **not itself a packaged, distributable mod** — it's a
workflow you use while building one of the mods above. Part 2 of that doc
(adding your own custom panel) is C++ engine-source work requiring a rebuild,
out of scope for this data-mod collection entirely.

**Honest status:** Parts 1 and 3 (using the shipped panels, persisting via
`data/visual_tuning.json`) are verified working. The final "run it" step has a
real, reproducible blocker on a stock Windows machine: the documented launch
script (`launch-with-tuning.ps1`) doesn't run via a plain double-click
(Windows' default `.ps1` file association opens Notepad, not PowerShell), and
running it from a default PowerShell console throws an execution-policy
error — neither workaround is currently documented. If you hit this, use
`powershell -ExecutionPolicy Bypass -File .\launch-with-tuning.ps1` until the
doc is updated.

## Missions and campaigns: editing shipped, creation still out of scope

Missions/campaigns were historically the single most common type of MC2 mod.
**Editing an existing mission is now covered** — see
[`mission-edit`](mission-edit/) above (entry 4): objectives, time limit,
resource budget, and roster-field edits via a whole-file `.fit` shadow, with
every mission file type's overlay coverage verified in code. What remains out
of scope, honestly: **creating a new mission** (the terrain `.pak` must be
authored with the in-repo mission editor — no walkthrough exists yet) and
**shipping a new mission under a new stem via a mod campaign** (every engine
seam for it is verified and cited in `mission-edit` §9 — mod campaigns are
even auto-injected into the load-campaign list — but no payload-backed,
live-tested example exists yet). Treat those two as planned follow-ups, not
as things this collection can walk you through today.

## Known gaps

Every example above was independently re-verified against this checkout's
source (not just proofread), and most came back clean. The gaps that
surfaced, summarized:

- **`texture-override`** — sound override (`data/sound/music27.wav`) does not
  audibly change in-game music; documented engine limitation, not a bug in
  the mod. No open action items.
- **`weapon-edit`** — no open issues. (A former cosmetic gap — the packaged
  `.mc2mod`'s sha256 wasn't reproducible across pack runs because of zip
  timestamps — was fixed in `tools/mc2mod/mc2mod.py` by pinning member
  timestamps; regression-tested in `tests/mc2mod/test_mc2mod.py`.)
- **`weapon-add`** — no open issues; verification is source-level (no live
  in-game capture was taken, disclosed honestly in the doc).
- **`mission-edit`** — no open issues; verification is source-level plus the
  full offline packaging pipeline (no live in-game capture was taken,
  disclosed honestly in the doc — same status `weapon-add` shipped with).
- **`variant-add`** — one fabricated source citation in the doc's prose (a
  `readInt(11,5,...)` call that doesn't exist) needs correcting; the
  underlying mechanism itself is fine.
- **`building-import`** — mechanism works, but does not go through
  `MC2_ACTIVE_MOD`'s normal overlay; `mc2mod.py install` places files at a
  path the engine never reads for this mechanism. Documented, with the
  correct direct-install path given instead.
- **`tree-import`** — same overlay exception as `building-import`, plus tree
  LOD-chain support is genuinely unfinished (tracked separately as
  `TREE-OVERRIDE-LOD-MVP-1`); this example avoids the broken configuration by
  design.
- **`mech-import`** — same overlay exception again, one layer down (the
  `.glb`, not just the `.ini`); documented with the correct direct-install
  workaround.
- **`imgui-editing`** — the documented final launch step doesn't work
  out-of-the-box on a default Windows install (`.ps1` double-click and
  default execution policy both block it); a working command is given above
  until the doc itself is patched. A secondary doc-sync gap (missing keys in
  `docs/visual-tuning-profiles.md`'s key table, and that page's own silence
  on the mod-overlay bypass) is also open.

None of these gaps block completing any tutorial — every one is either
already worked around in-doc, or (for `variant-add` and `imgui-editing`) a
small, clearly-scoped fix that doesn't change the underlying mechanism's
verdict.
