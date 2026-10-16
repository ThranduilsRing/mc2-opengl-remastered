# Lane 1 s7 cleanup review (shape load bool, B5.4b, B5.2d)

Range ab0227716..2bed9cbdc, lane 1 commits 607a945f0, bd1b83bd3, 3b59cd606, 2bed9cbdc. Read-only; no builds.

## Verdict
Accept with one should-fix. No must-fix. The one real gap is check (1): the old type of a forced reload is released from typeStore, so nothing ever deletes it.

## Must-fix
None.

## Should-fix
1. code/objtype.cpp load(), forced reload: `typeStore[n].release()` drops ownership of the old type and nothing deletes it afterward. The old type is neither owned by typeStore nor freed by destroy(), so it is never destructed. It leaks its name text, which is the same SSO leak B5.4b set out to close, now limited to the reload case. The behavior matches the pre-change code, which also did not delete it. Either record the old type in a small `retiredTypes` vector (delete in destroy()), or state in the commit that this residual leak is accepted. It is not a double delete: destroy() only sees the new owner.
2. The same comment says "as before", which is a journal phrase. Reword to the WHAT and WHY: "objects still point at the old type, so it is not deleted here".

## Nits
- msl_load.cpp TYPE_NODE branch ignores the bool from TG_TypeNode::LoadBinaryCopy. It always returns true, so this is harmless, but a `(void)` or check would keep callers uniform.
- The comment on `shadowVertexStore` is fine. The `listOfShadowVertices` "view" note is good.

## Checked-OK
- (2) TG_Shape creation: only the two CreateFrom paths (tgl_types.cpp:120 and :278) allocate TG_Shape, and both now placement-construct. There is no `new TG_Shape` elsewhere. The raw free is TG_MultiShape::destroy (msl_load.cpp:409-410), which calls node->destroy() before Free, and destroy() releases the vector. The ctor calls init() and ~TG_Shape calls destroy(); it is idempotent via swap.
- (3) Multi-shape clear-on-failure: `loadedShapes++` happens before the bool check, so the failing shape is included and exactly the loaded ones are deleted. Then numTG_TypeShapes=0, so the later destroy loops over nothing. No double free.
- (4) LoadBinaryCopy callers: the only TG_TypeShape caller (msl_load.cpp:97) checks the bool. The TYPE_NODE caller is the nit above. The other hits are the long-returning multi-shape API, which is unchanged and checked by the asset_viewer callers, or the unrelated _TG_Animation overload.
- (1) The remove() path resets the unique_ptr (delete), table is nulled, and destroy() swaps typeStore out before objectCache goes. The destructor is virtual (objtype.h:91).
- (5) The other new comments say WHAT and WHY. Commit bodies are the right place for the history.
- (6) The close commit body names "LG05 +4" and the bool returns plus the refusals.
