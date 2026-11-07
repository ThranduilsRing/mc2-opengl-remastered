# marray.hpp migration plan (DynamicArrayOf / StaticArrayOf -> std containers)

Status: plan revised after review (2026-11-06). B1 and narrowed B2 are done on branch claude/cxx23-marray-plan; B3-B7 not started.
Base nifty 2d042f852.
Counts are `rg` text hits at that base (not clangd-resolved); re-verify per batch.

## 0. Facts that change the picture
- Header is `mclib/stuff/marray.hpp`. MemoryBlock: no use (hits are unrelated: `LargestMemoryBlock`,
  `gMemoryBlockOverhead`, `TreeNode_MemoryBlock_Allocation`). Nothing to migrate.
- DynamicArrayOf (DAO) is already `unique_ptr<T[]>` + `size_t length`, allocated with `make_unique_for_overwrite`:
  NO zero-fill today. `SetLength` copies the overlapping prefix element-wise; `SetStorageLength` (ctor/Assign) frees
  and reallocates without copying. No capacity: every length change reallocates.
- Users are only `mclib/mlr` and `mclib/gosfx` (plus marray.hpp). `code/`, sim, AI, network and mission save
  (`code/*save*`, `mclib/*save*`) never touch these types. The `MemoryStreamIO_*` path here is the Stuff `.mlr`/`.gfx`
  shape and effect asset format, not the mission save or network stream.
- StaticArrayOf (SAO): 30 hits, 29 in marray.hpp itself; one real holder (`MLRTexturePool::textureArray`). Done in B1.
- sizeof: DAO is 16 B (ptr+len); std::vector is 24 B. No `sizeof(<holder class>)` and no whole-holder memcpy found in
  mclib/mlr or mclib/gosfx (rg). Holder layout is not load-bearing.
- MLR is live on VK (tube.cpp:1440 DrawEffect, mlr_gate.cpp:30, mlrsorter.cpp:90, gameos_graphics_draw.cpp:477-495).
  Do not delete it.
- Stream payload (marray.hpp:495-515): uint32 count + count*sizeof(T) raw bytes, native-endian.

## 1. Census (grouped by file family)
Use legend: R=resize (SetLength/ctor length), I=index, P=raw pointer handout (GetData), M=Mem_Copy/memset on data,
S=serialized (MemoryStreamIO_Read/Write), G=feeds GOS draw as raw bytes.

| File(s) | Type | Element | Hits | Use |
|---|---|---|---|---|
| mlrprimitivebase.hpp/.cpp | DAO member + static unique_ptr scratch | Vector4D, glm::vec3, Vector2DScalar, RGBAColor, MLRClippingState, uchar, ushort | 26 | R I P S(coords, texCoords, lengths) M(FlashClipTexCoords Mem_Copy) |
| mlrindexedprimitivebase.hpp/.cpp | DAO | ushort index, uchar visibleIndexedVertices, static clipExtraIndex | 5 | R I P S(index) |
| mlr_i_pmesh/tmesh .hpp/.cpp | DAO | uchar testList, Plane facePlanes, lengths | ~10 | R I P |
| mlr_i_c_, _c_dt_, _c_det_ pmesh/tmesh (12 files) | DAO | RGBAColor colors, `DAO*` actualColors, DWORD smallColors (mlr_i_c_pmesh, serialized) | ~38 | I P S(smallColors) (GetColorData handout) |
| mlr_i_l_, _l_dt_, _l_det_ pmesh/tmesh (10 files) | DAO | glm::vec3 normals (mlr_i_l_pmesh, serialized), RGBAColor litColors, `DAO*` actualColors | ~25 | R I P S(normals) |
| mlr_i_dt_pmesh/tmesh .hpp/.cpp | static unique_ptr<DAO> (unused, deleted in B2) | Vector2DScalar texCoords2, clipExtraTexCoords2 | 12 | none live |
| mlr_i_mt_pmesh .hpp/.cpp | DAO, nested DAO<DAO>, DAO<DAO*>, DAO<const T*> | MLRState, Vector2DScalar UV streams (serialized) | 13 | R I P S(UV streams), raw `new DAO` |
| mlrlightmap.cpp, mlreffect, mlrtrianglecloud, mlrindexedtrianglecloud | static unique_ptr<DAO> scratch + members | Vector4D, RGBAColor, Vector2DScalar, MLRClippingState, uchar, int | ~24 | R I P M(memset visibleIndexedVertices) |
| mlrsorter, mlrsortbyorder .hpp/.cpp | DAO | SortData, SortData*, SortAlpha* | 7 | R I P (pointer walk) |
| mlrshape.hpp | DAO | MLRPrimitiveBase* | 1 | I |
| gosvertexpool.hpp | DAO x6 | GOSVertex, GOSVertex2UV, ushort (frame + DB pools) | 6 | R P G (32-B alignment hack on GetData) |
| mlrtexturepool.hpp | SAO | MLRTexture* | 1 | I |
| gosfx debriscloud.hpp/.cpp | DAO | MLRShape*, glm::vec3, Sphere, Scalar, particle struct | ~6 | R(SetLength 0/n) I S(geometry, element-wise at debriscloud.cpp:117-128,186-224) |
| gosfx tube.hpp/.cpp | DAO | glm::vec3, Vector2D, Profile, char, float (static local) | 5 | R I P |
| gosfx particlecloud.hpp, singleton.hpp | DAO<char> | raw effect data blob | 2 | R P M(memset GetData/GetSize) |
| gosfx fcurve.hpp/.cpp | DAO | CurveKey m_keys | 1 (+2 stream calls) | R I S |
| marray.hpp | definitions | - | ~64 | - |

Total about 209 hits (179 DAO + 30 SAO), about 49 files. Per-row hits are approximate (rg counts per file family).

## 2. Risk per use
High (convert last or keep; ~45 sites, estimate):
- H1 S: stream Read/Write of arrays: primitivebase (coords, texCoords, lengths), indexedprimitivebase (index),
  fcurve m_keys, plus the friend templates: ~10 sites. Stream bytes must stay identical. Read the `MemoryStreamIO_*`
  body (marray.hpp, after `CopyArray`) before writing any shim; it was not read for this census.
- H2 G: GOSVertexPool six arrays (6 sites). Raw pointers go to the GOS draw path; `32 - (ptr & 31)` alignment fudge is
  computed from `GetData()` and applied to `GetData()+lastUsed`. A vector with the same fudge is equivalent; a span
  re-seat is not. Keep.
- H3 P: pointer handouts (`GetCoordData`, `GetColorData`, `GetNormalData`, `GetIndexData`; ~25 sites). Safe with
  `vector::data()` provided no resize happens between handout and use. DAO always reallocates on SetLength; vector
  keeps the pointer when capacity allows, so behaviour only gets more stable.
- H4: raw `new DynamicArrayOf<Vector2DScalar>` in mlr_i_mt_pmesh.cpp held by raw pointers in a `DAO<DAO*>`.
  Ownership is resolved: mlr_i_mt_pmesh.cpp:93,187-190 borrows slot 0 and deletes slots [1,passes). About 13 sites, serialized
  UV streams included, so this is B6 work, not B7.
Medium: M sites (`memset(visibleIndexedVertices, 0, n)`, `Mem_Copy` in FlashClipTexCoords, particlecloud memset): keep
memcpy/memset on `.data()`, trivially copyable T only (static_assert).
Low: index/size only, local temporaries, static scratch sized once at init.

Uninitialised-contents or capacity reliance: none confirmed. Every allocation is `make_unique_for_overwrite`, so any
read-before-write reads garbage today; zero-fill can only turn undefined into zero. Per-batch checks: (a) growth by
SetLength then read of the new tail; (b) `GetLength()` used as capacity (use resize only, never reserve-then-size);
(c) tests like `GetData() == nullptr` after `SetLength(0)` (vector::data() may be non-null; rg per batch).

## 3. Ruling proposal
1. Zero-fill: ACCEPT for types with no user-provided constructor (integers, floats, glm, RGBAColor, GOSVertex, which
   already initializes at gosvertex.cpp:22). Vector2DScalar/Vector2DOf (vector2d.hpp:47) and Vector4D (vector4d.hpp:47)
   have user-provided empty constructors, so `vector::resize` leaves them uninitialized, same as DAO today: not zero-fill.
   Allocation is NOT init/load only. Runtime lifetimes: tube.cpp Draw scratch `s_worldPos` grown in Draw
   (tube.cpp:1407-1409); effect creation during play (effect.cpp:669-673) allocating particle blocks (particlecloud.cpp:324)
   and tube profile/mesh storage (tube.cpp:575,606). Per-frame resize sites also include `FlashClipTexCoords`.
   B4 must treat these as runtime allocations and prove them with Tracy or a count, not assume startup cost.
2. sizeof: ACCEPT (16 -> 24 B per holder). Exceptions, keep: H2 (GOSVertexPool).
   Serialized holders are B6. Do not use a trivially-copyable static_assert for stream element types: Vector2DOf has a
   user-provided copy constructor (vector2d.hpp:56), so the assert would reject it. Use element assignment there.
3. API: keep call-site shape. Free helpers for `MemoryStreamIO_Read/Write(std::vector<T>)` replace the friends; no new
   wrapper class. For MLR pointer handouts use plain `std::span<T>` getters, no custom span type.
4. Each batch is a pure type swap with no behaviour edits.

## 4. Batches (lowest risk first)
| # | Scope | Sites | Target | Determinism note |
|---|---|---|---|---|
| B1 | DONE. SAO removal: marray.hpp class + its stream friends + `MLRTexturePool::textureArray` | ~31 | `std::array<MLRTexture*, N>` | Null-init in mlrtexturepool.cpp ctor/Restart preserved |
| B2 | DONE (narrowed). Delete the unused mt/dt scratch arrays (mlr_i_mt_pmesh.cpp:16-67, mlr_i_dt_pmesh.cpp:17-57, tmesh). Remaining live static scratch (primitivebase, indexedprimitivebase, indexedtrianglecloud, trianglecloud, lightmap, mlreffect) | ~50 | `static std::vector<T>`, same init sizes, storage released at TerminateClass via swap | Render scratch written before read; no sim, no RNG |
| B3 | Locals and small holders: mlrsorter `rawDrawData`, mlrsortbyorder arrays, mlreffect testList, mlr_i_tmesh/pmesh testList + facePlanes, mlrshape list. No serialized holder. | ~25 | `std::vector<T>` | Cull/sort of render lists only; ordering unchanged |
| B4 | gosfx holders: tube, particlecloud, singleton (runtime-allocated, see ruling 1), NOT fcurve and NOT debriscloud geometry | ~15 | `std::vector<T>`; blobs as `std::vector<unsigned char>` with memset kept | Tube blob partitioning (tube.cpp:606-613, 1407-1427) needs alignment, offset and pointer-lifetime review; grow in Draw must not invalidate handed-out pointers. Confirm debris seed feeds a gosfx-local RNG |
| B5 | MLR members without stream use: colors, litColors, `DAO*` actualColors (keep pointer-to-vector alias), testList | ~45 | `std::vector<T>` + `std::span` getters | Render only; `actualColors` must still alias the same vector object |
| B6 | DONE (branch claude/cxx23-marray-b6). The outer `multiTexCoords` slot table, `multiTexCoordsPointers`, `multiState` and ClipPolygon2 stay on DAO. Proof: doctest "std::vector stream payload is byte-identical to DynamicArrayOf" in tests/unit/test_memorystream.cpp. Streamed members: coords, texCoords, lengths, index, smallColors (mlr_i_c_pmesh), normals (mlr_i_l_pmesh), mt UV streams (mlr_i_mt_pmesh, H4), debriscloud geometry (element-wise), fcurve m_keys + Read/Write shim | ~55 | `std::vector<T>`; byte-identical stream shim (uint32 count + raw bytes; element assignment for Vector2DOf) | Prove with a `.mlr`/`.gfx` load-save byte compare, not a game run |
| B7 | Keep: GOSVertexPool (6) on DAO; the alignment offsets at gosvertexpool.hpp:188-190 are computed from GetData | ~6 | leave | GPU upload bytes: compare first-frame vertex dump if ever touched |

If anything remains after B7, shrink marray.hpp to the survivors; otherwise delete it.

## 5. Proof per batch
Targeted Ninja build + `tools/build_contract_harnesses.py --run --unit` + merge gate. B6 adds the offline byte
round-trip. B2/B5 also check that `rg "make_unique<DynamicArrayOf"` drops to the expected count.

## 6. Open questions for the reviewer
- Resolved: MLR is live on VK (section 0); payload layout is uint32 count + raw bytes (section 0).
- Run a clangd pass to replace the rg census with exact per-site counts before B2.
