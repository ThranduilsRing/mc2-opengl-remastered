<!-- doc-status: current | 2026-10-15 | lane 6 cleanup review (read-only) -->
# Lane 6 cleanup review: KTX2 parse, BC7 blocks, mech feed, shore sidecars

Scope: 3c5172001 fe0de149c e80153837 970156084 (tip d704f7c5a). Read-only; nothing built or run.
Verdict: **0 blockers, 2 should-fix, 5 nits.** Memory-safety discipline holds on every bound checked.

## sRGB answer (hex-prop difference)
- GL: `gos_NewCompressedTexture2D` uploads with the caller's format: `txmmgr_texture.cpp:784` maps vkFormat 146 ->
  `GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM`, 145 -> `GL_COMPRESSED_RGBA_BPTC_UNORM`; `bdactor_sidecar.cpp:235` uses `srgb` (false
  for normal/ORM). GL decodes sRGB->linear on sample for the SRGB format.
- VK now: `gameos_graphics_capi.cpp:206` keeps 145/146 from the GL format; `vk_owned.h texelFormat()` maps them to
  `VK_FORMAT_BC7_UNORM_BLOCK` / `VK_FORMAT_BC7_SRGB_BLOCK`; all three islands create image and view with that format. VK
  samplers carry no colour space (it lives in the view format), so the SRGB view decodes to linear exactly like GL.
- Old path: `glGetTexImage` of the SRGB texture returned the stored (sRGB-encoded) bytes, imported as `R8G8B8A8_UNORM`, so VK
  sampled encoded values as linear (too bright). **The new path matches GL; the old readback was the outlier.** If hex props
  still differ GL vs VK, the cause is not the BC7 colour space (look at the shader's own linearise / the RGBA8 mirror path,
  which is always UNORM on both backends).

## KtxParse (RenderCore/KtxLoader.cpp)
Checked and OK: magic + `size >= 80`; format whitelist (RGBA8 / BC6H / BC7); typeSize 1, depth 0, layers <= 1, faces 1,
supercompression 0; edges 1..16384; levels 1..full chain; level-index length checked as `size-80 < levels*24` (no underflow,
size >= 80 already); per level `length == levelBytes()` in uint64 (max 16384^2*4 = 1 GiB, no overflow) and
`offset > size || length > size - offset` (overflow-safe); total <= levels * 1 GiB fits size_t on x64; copy uses the validated
pairs. `ktxLoadRgba8` reads the whole file then parses. Readers: 12 call sites (hdri, prop finalize x6, terrain atlas,
matalbedo, bdactor, txmmgr x2) + texture_decode = 13, none keeps its own parse. `tools/asset_viewer/Ktx2Decoder.cpp` peeks
the magic itself only to classify the failure, then calls the loader (tool, acceptable).
Doctests (`test_ktx_parse.cpp`): happy BC7, truncated (level / index / header / null), oversized + zero edge, wrong block
size, too many levels, array, cube, Basis, unknown format.

## Should-fix
- **S1 rejections are silent.** `ktxLoadRgba8` drops `KtxParseResult::error` (`KtxLoader.cpp:146`); the header still says
  "Returns false (silent)". A malformed mod sidecar falls back with no log line. Log `path` + `error` once per path at Warn
  (`[TEXTURE_DECODE v1]` or a KTX tag) in `ktxLoadRgba8`.
- **S2 `Unavailable` handled inconsistently.** Mech (`vulkan_mech_island.cpp:532`) does it right (caches the dummy).
  Terrain overlay (`vulkan_terrain_island_overlay.cpp:127`) returns `nullptr` for Pending and Unavailable alike, so an
  Unavailable texture skips its draw every frame, contradicting the producer's "the importers draw it untextured"
  (`gameos_graphics.cpp:902`). Legacy shape (`vulkan_legacy_shape_island.cpp:255`) returns the dummy without caching, so it
  calls `gos_GetTextureTexels` every frame for that handle. Unavailable -> cache dummy (shape) / bind dummy (overlay).

## Nits
- N1 stale comment `vulkan_legacy_shape_island.cpp:251`: "a pre-compressed texture is read back once by its texture object"
  (the readback is deleted; it now imports the blocks).
- N2 `gameos_graphics_internal.h:261` `ensureRgbaMirror` failure uses `std::fprintf(stderr, ...)`; the rest of the commit uses
  `MC2_LOG(Core, Warn, ...)`.
- N3 doctest gaps (code is correct): a huge `byteOffset` with a valid length (the `offset > size` branch), an RGBA8 file,
  and a multi-level file checking `mipByteOffsets`.
- N4 `terrain_field_store.cpp setImage`: edge bound `kMaxSide*4` (65536) admits a 16 GiB R32F assign; the source already
  exists in the caller so it is not an overread, but a tighter bound (e.g. 16384, matching kKtxMaxEdge) is cheaper insurance.
- N5 terrain overlay's re-import leaks the old descriptor set until mission reset (comment admits it; pre-existing).

## Other items checked, OK
- BC7 blocks: `shared_ptr<const vector>` on `gosTexture` (`internal.h:370`), freed with the texture (the overlay's `source`
  copy extends it until that entry is destroyed, by design); size bounded at creation (`byteLen == bc7LevelBytes(w,h)`,
  edges <= 16384, BC7 only, rejection logged and returns `INVALID_TEXTURE_ID` = 0, which bdactor/txmmgr treat as failure).
- HUD path: `ensureRgbaMirror` decodes once per content serial (`mirror_decode_serial_` starts 0xffffffff, prebuilt serial
  1), failed decodes not retried; `decodeBc7` re-checks edge + byte count; doctest present.
- `MC2_VK_MECH_FEED_AUDIT`: no residue in code, Gates/, docs/flags, tier1 docs, tests, seed (only history docs mention it).
- `gos_LightDataSsbo_GetInterop`: definition and the one caller both gone; no other reference.
- Mech feed: `iboBytes` derives from `s_stagingIbo.size()`, vbo pointer nulled on size mismatch, island declines; failed
  `createFilled` resets itself, island resets vbo too. The admitted heredoc edit shows no `$`/backtick/escape damage.
- Shore sidecars: null / size rejected with a log line, version from the unique per-process counter, store recreated per
  terrain init (`gos_terrain_lod_chunk.cpp:488/686`); GL uploads from the store copy, VK imports the same bytes on gen change.
- Prop staging released with swap-to-empty at map change; colour-prop and mech uploads free both buffers on failure.
- Comments are WHAT+WHY, no journal tags added; no CR, 0x08 or NUL in any touched file.
