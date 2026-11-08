# load-waste state

Assignment: slot05, claude/load-waste, base 8fc0c6c895 (Linux build fix).
Scope: bounded two-slot music streaming; remove displacement TGA work;
cooked normals first with original layer-2 CPU alpha; cooked sun metadata;
avoid redundant default sky decode. No sim ordering changes or perf measurements.

Implemented bounded per-slot music queues (4 prebuffered / 8 total, 4096 bytes
each), background Microsoft/IMA ADPCM and PCM decoding (vendored dr_wav 0.14.6,
https://github.com/mackron/dr_libs), independent music handles and resident SFX.
Removed unused displacement reads, prefer provider-aware cooked normals, retain
original normal-layer-2 alpha. Embed sun metadata in KTX2, resolve sky sources
through the texture/file providers, and defer default sky until first use.
Old sky sidecars without metadata retain their EXR sun scan until recooked.

Green at implementation checkpoint: cook pytest (4 cases), git diff --check.
Selected contract filter `audio|terrain|hdri` matches no active rows at this base;
this is a proof gap, not a PASS. Native cases cover streaming/loops/ADPCM,
catalog overflow, independent slots, bounded startup, terrain alpha and KTX sun.
Native proof and cxx17 ratchet are running. Next: resolve any test/compile reds,
final Ninja compile, box build/unit/smoke-lite, final lint. Receipts and final
proved-at SHA go in E:/mc2-archive-2026-10/proofs/load-waste and final hand-back.
No sim mutation is changed; SIMID/saveload and perf measurement are not required
by section 5's load-waste row. No local install or copied exe was created.
