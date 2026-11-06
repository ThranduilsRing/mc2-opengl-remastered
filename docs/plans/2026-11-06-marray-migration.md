# marray.hpp migration plan (DynamicArrayOf / StaticArrayOf -> std containers)

Status: PLAN ONLY, no code changed. Base nifty 2d042f852. Needs Astra adversarial review before any conversion.
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
- StaticArrayOf (SAO): 30 hits, 29 in marray.hpp itself; one real holder (`MLRTexturePool::textureArray`).
- sizeof: DAO is 16 B (ptr+len); std::vector is 24 B. No `sizeof(<holder class>)` and no whole-holder memcpy found in
  mclib/mlr or mclib/gosfx (rg). Holder layout is not load-bearing.

## 1. Census (grouped by file family)
Use legend: R=resize (SetLength/ctor length), I=index, P=raw pointer handout (GetData), M=Mem_Copy/memset on data,
S=serialized (MemoryStreamIO_Read/Write), G=feeds GOS draw as raw bytes.

| File(s) | Type | Element | Hits | Use |
|---|---|---|---|---|
| mlrprimitivebase.hpp/.cpp | DAO member + static unique_ptr scratch | Vector4D, glm::vec3, Vector2DScalar, RGBAColor, MLRClippingState, uchar, ushort | 26 | R I P S(coords, texCoords, lengths) M(FlashClipTexCoords Mem_Copy) |
| mlrindexedprimitivebase.hpp/.cpp | DAO | ushort index, uchar visibleIndexedVertices, static clipExtraIndex | 5 | R I P S(index) |
| mlr_i_pmesh/tmesh .hpp/.cpp | DAO | uchar testList, Plane facePlanes, lengths | ~10 | R I P |
| mlr_i_c_, _c_dt_, _c_det_ pmesh/tmesh (12 files) | DAO | RGBAColor colors, `DAO*` actualColors, DWORD smallColors (local) | ~38 | I P (GetColorData handout) |
| mlr_i_l_, _l_dt_, _l_det_ pmesh/tmesh (10 files) | DAO | glm::vec3 normals, RGBAColor litColors, `DAO*` actualColors | ~25 | R I P |
| mlr_i_dt_pmesh/tmesh .hpp/.cpp | static unique_ptr<DAO> | Vector2DScalar texCoords2, clipExtraTexCoords2 | 12 | R I P |
| mlr_i_mt_pmesh .hpp/.cpp | DAO, nested DAO<DAO>, DAO<DAO*>, DAO<const T*> | MLRState, Vector2DScalar | 13 | R I P, raw `new DAO` |
| mlrlightmap.cpp, mlreffect, mlrtrianglecloud, mlrindexedtrianglecloud | static unique_ptr<DAO> scratch + members | Vector4D, RGBAColor, Vector2DScalar, MLRClippingState, uchar, int | ~24 | R I P M(memset visibleIndexedVertices) |
| mlrsorter, mlrsortbyorder .hpp/.cpp | DAO | SortData, SortData*, SortAlpha* | 7 | R I P (pointer walk) |
| mlrshape.hpp | DAO | MLRPrimitiveBase* | 1 | I |
| gosvertexpool.hpp | DAO x6 | GOSVertex, GOSVertex2UV, ushort (frame + DB pools) | 6 | R P G (32-B alignment hack on GetData) |
| mlrtexturepool.hpp | SAO | MLRTexture* | 1 | I |
| gosfx debriscloud.hpp/.cpp | DAO | MLRShape*, glm::vec3, Sphere, Scalar, particle struct | ~6 | R(SetLength 0/n) I |
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
- H4: raw `new DynamicArrayOf<Vector2DScalar>` in mlr_i_mt_pmesh.cpp (~lines 104, 278) held by raw pointers in a
  `DAO<DAO*>` (ownership and leaks unclear); nested `DAO<DAO<T>>` statics. About 13 sites. Convert last.
Medium: M sites (`memset(visibleIndexedVertices, 0, n)`, `Mem_Copy` in FlashClipTexCoords, particlecloud memset): keep
memcpy/memset on `.data()`, trivially copyable T only (static_assert).
Low: index/size only, local temporaries, static scratch sized once at init.

Uninitialised-contents or capacity reliance: none confirmed. Every allocation is `make_unique_for_overwrite`, so any
read-before-write reads garbage today; zero-fill can only turn undefined into zero. Per-batch checks: (a) growth by
SetLength then read of the new tail; (b) `GetLength()` used as capacity (use resize only, never reserve-then-size);
(c) tests like `GetData() == nullptr` after `SetLength(0)` (vector::data() may be non-null; rg per batch).

## 3. Ruling proposal
1. Zero-fill: ACCEPT everywhere. Allocation is init/load time. Only per-frame resize sites are
   `FlashClipTexCoords` (`SetLength(num)`, up to 1024 Vector2D) and the clip-extra copies; `vector::resize` reuses
   capacity, so those get cheaper. Only measurable cost: GOSVertexPool one-time zero-fill of about
   (8192+2048+1)x2 vertex arrays plus DB pools, startup only. No hot path relies on overwrite semantics; if Tracy ever
   shows one, use a default-init allocator for that holder only.
2. sizeof: ACCEPT (16 -> 24 B per holder). Exceptions, convert last / keep: H1 (serialized), H2 (GOSVertexPool),
   H4 (raw-new nested).
3. API: keep call-site shape. Free helpers for `MemoryStreamIO_Read/Write(std::vector<T>)` replace the friends; no new
   wrapper class. For MLR pointer handouts use plain `std::span<T>` getters, no custom span type.
4. Each batch is a pure type swap with no behaviour edits.

## 4. Batches (lowest risk first)
| # | Scope | Sites | Target | Determinism note |
|---|---|---|---|---|
| B1 | SAO removal: marray.hpp class + its stream friends if unused + `MLRTexturePool::textureArray` | ~31 | `std::array<MLRTexture*, N>` | Null-init replaces uninit; render only, no sim |
| B2 | Static scratch buffers: `unique_ptr<DAO<..>>` and init/teardown in primitivebase, indexedprimitivebase, indexedtrianglecloud, trianglecloud, lightmap, mlreffect, mlr_i_dt_*, mt_pmesh statics | ~50 | `static std::vector<T>` sized once at init; drop the unique_ptr | Render scratch written before read; no sim, no RNG |
| B3 | Locals and small holders: `smallColors` (12 locals), mlrsorter `rawDrawData`, mlrsortbyorder arrays, mlreffect testList, mlr_i_tmesh/pmesh testList + facePlanes, mlrshape list | ~35 | `std::vector<T>` | Cull/sort of render lists only; ordering unchanged |
| B4 | gosfx holders (debriscloud, tube, particlecloud, singleton), NOT fcurve | ~20 | `std::vector<T>`; blobs as `std::vector<unsigned char>` with memset kept | Visual only. Confirm debris `debrisSeed` feeds a gosfx-local RNG, not the sim RNG, before converting |
| B5 | MLR members without stream use: colors, litColors, normals, `DAO*` actualColors (keep pointer-to-vector alias), testList | ~60 | `std::vector<T>` + `std::span` getters | Render only; `actualColors` must still alias the same vector object |
| B6 | Streamed members: coords, texCoords, lengths, index, fcurve m_keys + Read/Write shim | ~30 | `std::vector<T>`; byte-identical stream shim | Keys sampled at time t; values unchanged. Prove with a `.mlr`/`.gfx` load-save byte compare, not a game run |
| B7 | Keep / last: GOSVertexPool (6), mt_pmesh nested/raw-new (13), leftovers | ~19 | decide after B1-B6; likely leave GOSVertexPool on DAO or one aligned vector | GPU upload bytes: compare first-frame vertex dump if touched |

If anything remains after B7, shrink marray.hpp to the survivors; otherwise delete it.

## 5. Proof per batch
Targeted Ninja build + `tools/build_contract_harnesses.py --run --unit` + merge gate. B6 adds the offline byte
round-trip. B2/B5 also check that `rg "make_unique<DynamicArrayOf"` drops to the expected count.

## 6. Open questions for the reviewer
- Is MLR still live on the draw path, or only Stuff data structures (shape and effect load)? If the latter,
  B6/B7 shrink and parts of the stream layer may be deletable.
- Confirm the `MemoryStreamIO_*(DAO)` payload layout (not read here).
- Run a clangd pass to replace the rg census with exact per-site counts before B2.
