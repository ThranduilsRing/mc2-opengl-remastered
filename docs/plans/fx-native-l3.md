# Native FX render boundary (L3)

Assignment: slot05, claude/fx-native-l3, starting tip d1424c300 (L2).
Scope: native material identities, generation-safe mesh resources, reusable
indexed frame packets and explicit frame view; bridge/island support and tests.
L4/L5 retain responsibility for migrating game callers from gosFX.

Inspection: old bridges resolve MLR pool indices; tubes expand indices into
64-byte vertices; old mesh cache keys are primitive addresses and opaque draws
are remapped to alpha. L2 geometry is independent, with material chains and
immutable L1 meshes, but deliberately has no game callers yet.

Implemented (25 files): native material composition/identities, owned epoch
mesh cache, 40-byte indexed vertices/16-byte draws, reusable frame vectors,
unique-material resolution through textures service, explicit FrameView and
Batcher SubmitNative/FlushNative. All native families/passes share ordered
submission. Legacy producers remain compatibility adapters for L4/L5; live
tubes use indexed compact vertices, mesh opaque state is preserved, and reload
retires GPU frames and invalidates upload/bridge/native caches together.
New doctests cover reload at same address, colors/pass UVs, blend/clamp,
permission composition, resolve-once, retained capacity and shader word layout.
Green so far: vfx_particle.vert production compiler/reflection. First local
Ninja compile is running (old build tree needs substantial cache warming).
Next: finish compile; run fx_render/VfxDrawPlan and L2 fx_playback doctests,
covering shader/layout/native-boundary rows, final-tip compile and one lint.
Then box smoke-lite and one day/night ab.py capture, inspect montage.
Evidence destination: E:/mc2-archive-2026-10/proofs/fx-native-l3.
No install/game/copy created. Scout scratch: G:/mc2-tmp/scout (remove own report).
