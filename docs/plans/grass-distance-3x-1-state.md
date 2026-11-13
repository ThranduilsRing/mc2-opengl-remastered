grass-distance-3x-1: prototype, owner acceptance pending.
Base: play-1113 597ca3720, contains assigned 6dd59cabf.
Reviewed prior sward-road-lod-1 through cae36f4ac; no lateral merge.
Existing two-shader draft retained: grass fade endpoints and region range x3;
projected grass cover converges to zero, existing thinning and footprint shading retained.
Sites: vegetation_structs.glsl vegLodScale; vegetation_region.comp sphere cutoff.
Next: offline shader checks/lint, commit/build, isolated deploy with tree-kit parity,
smoke-lite, all 12 owner poses at 4K, near/mid/far A/B and Tracy airfield/corner.
No acceptance claim. Runtime/proof receipts recorded in slot05 scratch note.
