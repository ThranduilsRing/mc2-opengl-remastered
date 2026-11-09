# Logistics minimap 2x

Assignment: A:/Games/mc2-slot02, claude/logistics-art, inherited tip 7b7d950b3.
Requested base f269b312d8328aaafccec0b53a15f23ce5775efb is an ancestor.

Plan (under 150 calls): inspect cooker/UVs; implement pixel-height shoreline,
bilinear color and seam fix; test and recook 43 maps; regenerate sheets; commit
by path in batches of at most 25; box build/unit/smoke-lite plus covering rows;
one Windows default-target build; verified private deploy; briefing 01/03/10
and bay captures; cleanup scratch and record evidence in the proof folder.

Read continuation-20261108.md first. Inherited pc-lane-11 has a failed partial
deploy (stale asset viewer); build both default targets before redeploying it.
No new install required. Old proof receipts apply only to inherited tip.

Implemented: default 1024, bilinear colormap, per-pixel visual-height/fallback
sim-height shoreline with one-pixel AA, joined road-layer filtering (no separate
rounded cell resizes). Ten code tests passed (cooker-code-2x.log); full shipped
art test waits for recook. New tests cover diagonal coast, visual priority and
dimensions, road-cell filtering, and bilinear color. Initial NW-water assertion
was updated to sample deep water instead of the new AA contour.
Final PNGs use an adaptive 256-color palette to keep the doubled art below the
repository's approximate 1 MiB generated-file limit; runtime decoding expands it.

Framing inspected: UiDefs::setElementTextureNode sets full normalized UVs;
drawUiImageElement scales e.rect independently of texture dimensions. The defs
rectangle and page scaling are unchanged at every resolution/aspect; the loader
already accepts 1024. Mod-priority fallback unchanged. No C++ edit needed.

Next: finish 43-map recook and sheets; inspect 01/03/10; commit, then final-tip
contracts, box proof and Windows default build. Verified deploy to retained
pc-lane-11, capture briefing 01/03/10 plus bay, cleanup and external receipts.
