<!-- doc-status: current | reviewed:2026-10-15 | lane 6 cleanup review -->
# Lane 6 producers cleanup review (2026-10-15)

Scope: baa0b195a (census) and 242b9582c (producers), tip 542349344. Read-only review: nothing was built or run.
Counts: **2 blocker, 2 should-fix, 4 nit.**

## Blockers (TGA validator/decoder differential, from the coordinator's added check)

- **B1** `GameOS/gameos/texture_decode.cpp:89-91` admits RLE (type 10) at bpp 16, and the comment at :80 says "RLE payloads are
  bounded by the loader". They are not. `utils/Image.cpp:598` allocates `w*h*2` bytes, but `mergeBytes` (`Image.cpp:572-577`)
  writes 4 bytes per pixel at a 2-byte stride, so the write runs past the heap buffer. `finishDecode` (`texture_decode.cpp:114`)
  then reads `w*h*4` from that buffer. Trigger: an 18-byte header with type 10, bpp 16, 64x64, followed by one RLE packet
  `0xFF xx xx` repeated. This bug was already in the old path, but the new validator now passes it. Fix: reject type 10 at bpp 16
  in `tgaFits`, or make the loader allocate 4 bytes per pixel for 16-bit input.
- **B2** `texture_decode.cpp:91` does not bound `w*h` for RLE, and `Image.cpp:469/:598` computes `width*height*pixelSize` in
  `int`. Trigger: type 10, bpp 32, width = height = 0x8000, plus a few RLE packets. The product `2^30*4` wraps to 0, so
  `new[0]` is allocated and the decode loop writes up to 2^30 pixels into it. Fix: in `tgaFits`, reject
  `w*h*(bpp/8) > INT_MAX`, or a cap such as 8192x8192, for every image type.
  - The file path has the same problem. `decodeFile` (`texture_decode.cpp:147-158`) never calls `tgaFits`, so
    `Image::loadTGA(FILE*)` (`Image.cpp:426-428`, then `:539`) wraps the same way for type 2 at 32768x32768x32. Fix: read the
    header in `decodeFile` and run the same validator before loading.
- Checked and clean:
  - Uncompressed type 2 from memory: the validator bound (`:90`) equals the loader's memcpy range (`Image.cpp:469-478`), and the
    colour-map bytes are ignored by both.
  - 16-bit uncompressed: the output is `w*h*4` and the input is checked.
  - RLE packets that run past the end of the input: `Image.cpp` checks `len` before every packet and clamps runs to `num_pixels`.
  - Paletted and grey types (1, 3, 9, 11): both the validator and the loader reject them. This matches the old behaviour, which
    returned false.

## Should-fix

- **S1** `GameOS/gameos/vulkan_mech_island.cpp` `get_tex_view` / `get_scheme_set` (`pending` path): `gos_GetTextureRgba` returns
  null both while a texture is still loading and when `ensureRgbaMirrorFromGpu` gives up for good (`gameos_graphics_internal.h`:
  a failed or all-zero readback is not retried for the same content serial). In the second case the whole
  albedo/AO/normal set stays dummy for every frame, where the old import still drew it. Fix: return a tri-state (`ready` /
  `pending` / `unavailable`) and set `pending` only for `pending`.
  - The same decline now silently drops all-zero `.ktx2` textures in the legacy-shape and overlay islands, which used to import
    them. Log this once per handle.
- **S2** The `texture_decode.cpp:79-80` comment states a bound that does not exist (see B1/B2). Fix: rewrite it to say what
  `tgaFits` actually guarantees once B1/B2 are fixed.

## Nits

- **N1** `gos_static_prop_batcher_mapload.cpp:62-63`: `clear()` keeps the previous map's capacity, so the CPU staging copy is
  bounded by the largest map seen rather than freed at map change. The header comment at `vulkan_color_prop_island.h:44` implies
  it is freed. Fix: add `shrink_to_fit()` (or swap with an empty vector) at map load, or reword the comment.
- **N2** `vulkan_color_prop_island.cpp` `upload_geometry_once`: a null `map()` now returns false after both `make_buffer` calls
  have run. Fix: confirm the retry path frees or reuses `s->vbo` / `s->ibo` rather than allocating them again.
- **N3** `texture_decode.cpp:154,176`: a failed load logs at Debug, while every other rejection logs at Warn with the
  `[TEXTURE_DECODE v1]` tag. Fix: use the same tag and level so "one log line per rejection" holds at default verbosity.
- **N4** `gos_terrain_lod_chunk.cpp` `s_fieldStore`: the versions restart at 1 after Destroy/Init, while the island's
  `*VersionCopied` values survive. Init/Destroy only run with renderer init and shutdown, so this is harmless today. Fix: seed the
  versions from a process-wide counter, or document that the island must be reset together with the store.

## Verified clean

- `texture_decode.{h,cpp}` and `terrain_field_store.{h,cpp}` contain no `gl*` calls and no `static` state. The only file static is
  the documented `s_fieldStore`, which `gos_TerrainLodChunk_Init` creates and `_Destroy` resets.
- The decode policy is byte-identical to the GL code it replaced:
  - magenta key: same RGB test, PNG only, behind the gate;
  - Detect becomes Alpha/Solid: same rule;
  - Solid forces alpha to 255 on RGBA8;
  - mipmap opt-in: same rule;
  - placeholder texels `FF FF 00 FF`: identical to the old `0xFF00FFFF`, and the format is still left unresolved.
  - The old STOPs are now a log line plus `return false`, and `createHardwareTexture` returns false. The handling there is
    unchanged.
- The CPU copy is the same bytes as the old mirror (RGBA8 adopted, RGB8 expanded with alpha 255, mip level 0, upload row order).
  The islands' old `glGetTexImage(GL_RGBA)` returned those same level-0 bytes, so no texel and no swizzle changes. BC7 still reads
  back once, inside the texture object, which now restores the previous binding.
- Lifetime: `gos_GetTextureRgba` returns an immutable `shared_ptr` that outlives a texture release. The overlay re-imports when
  the pointer changes; the mech and legacy islands key on the handle, as they did on the GL id before.
- Terrain fields:
  - `patchHeight` bumps the version and rejects out-of-range blocks with a log line. The bounds match what `terrain.cpp:803-824`
    produces.
  - The GL SSBO uploads read from the store's bytes.
  - The cement copy falls back to zero when the words do not cover the map.
- `Mc2ShadowGeomFeed` and `vulkan_shadow_island_live.*` are unchanged. The GL shared VBO/IBO still exist, so lane 2's readback
  still works.
- Census spot-check: T1, T2 and T4 (true GL readbacks, confirmed in the pre-change code), P4 (`vulkan_mech_island.cpp:880-918`:
  CPU feed, with GL used only for the `MC2_VK_MECH_FEED_AUDIT` compare) and P9 (`gameos_graphics_tparams.cpp:635`, CPU only).
  All five are classified correctly.
- Comments explain WHAT and WHY (except S2). Producers were redesigned, not wrapped. Neither commit diff contains CR, 0x08 or
  NUL bytes.
