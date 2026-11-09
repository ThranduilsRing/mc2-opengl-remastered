# tools/neural_mat -- offline neural-material workbench (N2)

Python only. No engine, shader or CMake change; nothing here is player-visible. Datasets, checkpoints and run
outputs live outside the tree (`G:/mc2-tmp/neural-n2/`).

## Pipeline

| Module | Role |
|---|---|
| `enhance.py` | analytic enhanced BRDF (20 params, 8 lobes), `evaluate` -> f, T, R; `furnace` |
| `sample_params.py` | deterministic themed parameter + direction sampler (4 classes + 20 % uniform infill) |
| `model.py` | universal decoder (high 4x64 / low 2x32 / tiny 2x16), half/difference inputs, learned frame, GGX proxy head |
| `train.py` | auto-decoder training (per-material latents) |
| `fit_latents.py` | per-texel latent fit against the frozen decoder (best of K, then Adam) |
| `quantize.py` / `reference_decode.py` | INT8 per-channel weights, FP16 first/last layer, cosine fallback; NumPy reference decoder |
| `export_nmat.py` | `.nmat` container, white-furnace energy gate, latent textures (texcook role `neural_latent`, linear BC7) |
| `golden_vectors.py` | N1 `nm_golden_v1` JSON (format_version 1) golden vectors |
| `licence_gate.py` / `matsynth_ingest.py` | CC0-only MatSynth ingest with provenance rows |
| `derisk.py` | three-material FLIP de-risk run |

## Licence policy

Only MatSynth rows with `metadata.license == "CC0"` pass. Sources starting `deschaintre` are excluded even if
labelled CC0. Provenance rows use the `tools/mc2texcook/cliff_rock_provenance.json` "sets" format.

## Golden vectors

N1 format, float tier only: the net is zero-padded to a square width-64 MLP, FP16-exact weights `[k][n]`,
`out_exp` 0. N1's power-of-two-scale INT8 tier is not emitted: this decoder uses per-channel FP32 dequantisation
and FP16 first/last layers, so the exact arithmetic is in the `nmat_ext` block (quantised weights, scales, inputs,
reference outputs).

## Tests

    py -3 -B -m pytest tools/neural_mat/tests tools/content_pipeline/tests -q -p no:cacheprovider

## Full-scale training (box)

    py -3 -m tools.neural_mat.train --out G:/mc2-tmp/neural-n2/full --name high --materials 20000 --dirs 64 --nq 96 --steps 60000 --batch-mat 64 --workers 8 --seed 0

`pip install -r tools/neural_mat/requirements.txt` first. Then quantise (`quantize.quantize`) and export
(`export_nmat.write_nmat`) from the checkpoint; the de-risk driver shows the sequence.

## De-risk run

    py -3 -m tools.neural_mat.derisk --work <dir> --shards <MatSynth test parquet> --orm <orm.ktx2>

Design ideas (half/diff inputs, learned frame rotation, prefiltered-mip fitting) follow the public Slang
neural-shading course; no code was lifted from it.
