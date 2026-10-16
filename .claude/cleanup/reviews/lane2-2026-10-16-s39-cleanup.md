# Lane 2 s39 cleanup review (21d247eed..9113a4554, read-only)

## Verdict
ACCEPT after one doc fix (row 29 cell carries stale text). Code is sound.

## Must-fix
1. `.claude/VK-PARITY-GAPS.md` row 29 evidence cell: after the new s39 text the cell still contains the whole session 38 block ("Session 38 baseline, untextured island. Evidence: ... Cause ... the island is UNTEXTURED by design ... larger pale-yellow soft discs"). The cell now states both "textured" and "UNTEXTURED by design". Delete the s38 block (the numbers 8,800 / 7,471 / 5,121 are already quoted in the s39 text; keep the white-squares sentence in lane2-state only, as done).

## Should-fix
1. Row 29 next step is vague. "(tube / gosFX beam head, the deferred remainder)" names no symbol or file. Name the producer (`gos_tube_ribbon_flush_deferred` / `gos_vfx_mesh_flush`, as in `vulkan_vfx_island.cpp` header) and the oracle pixel box of the four blobs.
2. Muzzle size reference is unexecutable as written: "needs a GL run, not done", but s38 deleted the oracle GL arm. State the reference instead (MLR arm same sprite page and sub-rect, or `capture_ab.py --gl-arm`) and what number closes it.
3. `get_page`: no negative cache. A persistently failing import (e.g. unsupported BC7 format, fence wait) retries every frame, allocates staging, resets the shared cbuf, and logs every frame. Remember the failed `source` pointer per handle.
4. PARITY STATE header still says "(lane 2 session 37)"; make it 39.
5. Pages are never evicted when a handle is freed; 64-page cap then silently drops groups. Log once at the cap.

## Nits
- Header sentence "OPEN: row 29 only ... Row 18 ... CLOSED" leaves a dangling ")" after "pin for it".
- `gos_particle_bridge.cpp` comment line is now over-long.
- Test file has no `<string>` include of its own (relies on `gos_screenshot_slot.h`); add it.

## Checked-OK
1. Re-import trigger is a shared_ptr address compare (`source == t.bytes`), O(1); `source` keeps the vector alive so the address cannot be reused. Image, view, set in `SpritePage`; map declared after pool/sampler/layout so pages release first; sets freed with pool; ledger Manual 0 stated in state and not contradicted.
2. `VfxGroupPush` = vec2+vec2+uint = 20 bytes, offsets 0/8/16, static_assert 20, range VERTEX 0..20, matches the push block in `vfx_particle.vert`. No padding surprise.
3. FS matches `particle_billboard.frag` default path: colour key `r>0.9 && g<0.1 && b>0.9`, head x1.5 on `is_head==1`, discard `a<0.01`, `textureLod(...,0)`. VS uv formula and `kind_flags & 1` head flag as GL. Not-ported blocks default off: soft/lit are `RenderOptions` false defaults (`Gates/render_options.h:47-48`, not in gate_table.json), blackbody and distortion are `Lead1` default false in gate_table.json, age fade defaults 0.0 in the bridge setter; debug mode is 0.
4. `test_screenshot_slot.cpp`: new TEST_CASE well-formed, file all LF (0 CR), ASCII, single doctest + slot include, no duplicates; matches `Slot::refusal`.
5. Comments in the island/shaders are WHAT+WHY; the journal sentence moved out of row 29 to lane2-state.
6. Total line and header say "Open: row 29 only"; row numbers (8,381 / 6,734 / 3,947 vs 8,800 / 7,471 / 5,121) consistent between row and state.
7. Log-tag re-freeze reason stated in lane2-state (`[SCREENSHOT v1]` keys 6 -> 4, three rejection prints became one); baseline diff is that one count.
