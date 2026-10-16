<!-- doc-status: current | reviewed:2026-10-16 -->
# C++23 step 4 B5 (shared assets -> one owner and one release path): before-review of `cxx23-step4-b5-notes.md`, B5.0

WHAT: the before-review B5 starts from, in the B4 shape: the sealed census, what the batch changes, the invariants every sub-batch keeps, the proof per sub-batch, the risk list.
Written by lane 1 (2026-10-16) because the brief asked for it; it changes no proof standard. Where a proof needs a lane or a claim lane 1 does not hold, it says so and stops.
WHY: B3 ran without one and its census could not be re-run; B4 started from a script and a review and had no surprises in its census. B5 does the same.

## 1. Census (sealed)
- Command: `py -3 tests/analyze/b5_asset_census.py` (summary), `--check` (exit 1 when the tree differs from the list), `--write` (a sub-batch commit rewrites the list by intent).
  The list is `.claude/cleanup/cxx23-step4-b5-callsites.txt`.
- Count at the seal: **400 sites in 70 files** = B5.1 texture 125 (release 26, acquire 99), B5.2 TG shapes 93, B5.3 appearance types 143, B5.4 object types 8, B5.5 heap owners 31.
- What the script counts, and does not: the two texture release APIs and the three acquisition calls by name; `appearanceHeap` / `tglHeap` `Malloc` and `Free`; `X::operator new/delete` of
  an asset class; `new` of a type / shape / appearance-type class; `delete` of an expression named like a type, shape, appearance, texture or heap, minus an explicit list that belongs
  to other batches (an actor's own `delete appearance`, other heaps, UI icon memory). It does NOT count the editor, tools or GameOS (the same APIs, other owners), `new` of an
  object instance (B7), or a `delete` of an expression whose name does not say what it is (a miss; `OTHER_BATCH_DELETE` and `ASSET_NAME` are the two places to widen).
- The unequal pairs, explained: `tglHeap` 43 / 26 and `appearanceHeap` 21 / 10 are bulk-released at mission end (`tglHeap->destroy()`, `delete appearanceHeap`); 99 texture acquirers
  against 26 releases means most holders never release (notes sections 2 and 5).

## 2. What B5 changes
A shared asset gets one owner. Textures: a move-only `TextureRef` over the manager's refcount, one release function in place of two. TG shapes and appearance types: the raw buffers they
take from `tglHeap` / `appearanceHeap` become members of the class that frees them, the registries own their types with `unique_ptr`, and the heaps and the asset classes' `operator new/delete`
go in B5.5. The `numUsers` unload stays. The class-routed heaps of other subsystems (`objectCache`, `guiHeap`, `missionHeap`) are B6 / B7.

## 3. Invariants (every sub-batch)
1. **No release is added where none exists** (notes F2). Texture residency and cache hits are render-visible; a missing release is a leak that this batch preserves and names.
2. **Counts from files are checked before they size anything** (B4 invariant 2, ruling 5): shape, triangle, animation-frame and node counts read from a `.tgl`, `.msl` or `.fit` get a bound and a log line.
3. **No view outlives a resize** (B4 invariant 3): weapon-node tables, shape vertex / triangle arrays and animation tracks are sized once; the views that instances keep are named in the commit.
4. **A buffer that the heap released in bulk now dies with its owner**: the commit names the object whose destructor frees it, and the mission reload delta shows the owner does (H2).
5. **The `numUsers` count transitions are unchanged**: a doctest pins get / get / remove / remove on a fake type before the registry changes ownership.
6. **Save layout untouched**: object type ids and appearance names are in saves; B5 changes who owns the type, not its id or name.
7. **No static-duration owner**; one family per commit, at most 25 edited files, CRLF kept (`git ls-files --eol` and a byte check before a line-ending-sensitive edit), comments WHAT+WHY.
8. **Include-only first** where a batch adds a standard header (D16); `mclib/texture_ref.h` is its own include-only commit with its doctest and no callers.
9. **Every header change is proved by `tools/build_all_targets.py`** (the editor and tools include `txmmgr.h`, `tgl.h`, `msl.h`); forced edits in `editor/` or `tools/` are listed in the commit.

## 4. Sub-batches and proof (proof-by-domain ruling: lane 1 is a sim lane; render-side owners add the same-exe GL capture pairs)
| sub-batch | content | proof per commit |
|---|---|---|
| B5.1a | `texture_ref.h`, `releaseNode`, doctest, no callers | build; doctest; objhash of the rebuilt TUs (only `txmmgr.cpp` changes) |
| B5.1b | 12 `removeTexture` sites to `releaseNode`; delete `removeTexture` | build; doctest; `sim_determinism check` x4 at `--sim-speed 4`; saveload x5; GL pair mc2_01 day and mc2_10 night viewed, plus the repeat pair |
| B5.1c | 14 `removeTextureNode` sites to `TextureRef` members; delete `removeTextureNode` | as B5.1b |
| B5.2 | TG shape buffers and `new/delete` of shapes | as B5.1b, plus the mission reload delta |
| B5.3 | appearance types | as B5.2 |
| B5.4 | object types | sim x4, saveload x5 |
| B5.5 | heaps and class operators | `build_all_targets`, sim x4, tier1; owner sites in `code/mission.cpp` are Sol's |
Last commit of each sub-batch: tier1, ASan doctest, campaign round trip once on its final exe.

## 5. Risk list
| risk | where | handling |
|---|---|---|
| a texture freed under a second user | the node-based release unconditionally destroys | `releaseNode` decrements; shared nodes survive; the pair is viewed |
| a texture kept alive by a leaked reference now freed | adding releases (H1) | forbidden by invariant 1 |
| the `gosimage` release was a no-op or hit another node (notes 5.3) | B5.1b | the pair is viewed; the finding goes in the commit; a visible effect gets a BUGS row |
| `get_gosTextureHandle` removal changes residency order | the 11 UI / paint sites | the call cached the texture in and then released it; removing it removes an upload that was thrown away; the pair is viewed |
| bulk release hid a leak (H2) | tglHeap / appearanceHeap buffers | reload delta before and after; the owner's destructor is named |
| weapon-node tables read per frame | `nodeData`, `nodeUsed`, `nodeRecycle` | size once, view stays valid (invariant 3) |
| shared header ripple | `txmmgr.h`, `tgl.h`, `msl.h` | `build_all_targets`; PCH hazard: include-only first |
| render proof lane | B5.1b onward | lane 1 ran the B4 pairs under the orchestrator's assignment; the same assignment is assumed and recorded in section 6 |
| editor and tools | `Editor.cpp` owns its own `tglHeap` | not edited here; `build_all_targets` and `run_editor_smoke` where editor-reachable |

## 6. Open (orchestrator)
1. Render-proof assignment for B5.1b onward (assumed as for B4: lane 1 runs and views the pairs).
2. Whether this lane-written before-review stands for B5.2 / B5.3 (the shape and appearance batches change the most code), or an Opus pass reads it first. The plan's rule is an Opus before-review per step 4 batch.
3. B5.5 needs Sol's `code/mission.cpp` owner sites (tglHeap creation and destruction at 847-908 and 1150-1191, `appearanceTypeList` deletion at 1142).
