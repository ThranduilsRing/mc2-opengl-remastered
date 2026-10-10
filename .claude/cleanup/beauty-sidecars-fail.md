<!-- doc-status: stale | reviewed:2026-10-11 -->
# beauty_sidecars check failure: diagnosis

Command (scripts/check-contracts.sh:1156): `py -3 scripts/check-beauty-sidecars.py --no-lanes --quiet` (exit 2).

Output:
- FAIL beauty-sidecars: docs/beauty-sidecar-inventory.txt is stale. Re-run with --write.
- FAIL beauty-sidecars: a cooked sidecar is not installed in an enforced deploy lane (or the inventory is stale). ...

Root cause: the inventory is stale only. Rendered-vs-committed diff is one line:
  + mc2_10/visual_damp.r32 (added to git by 27dc6c58f, not in [cooked])
The second FAIL is the same stale-inventory condition, not a real install gap.

Deploy audit (`--target A:/Games/mc2-opengl/mc2-win64-v0.4`): ok=48 missing=0 drift=5
(mc2_01/visual_height_4x.r32, mc2_17/{shoreline_mask.bounds.txt,shoreline_mask.png,visual_height_4x.r32}, mc2_24/visual_height_4x.r32).
Drift is WARN only (not enforced). mc2_10 visual_damp.r32 is installed and matches.

Runtime sidecar names (engine fopen): control_map.png, height_delta.r32, overlay_v2.png, protected.r8, shoreline_mask.png, visual_damp.r32, visual_height_4x.r32, visual_height_mips.r32.

Untracked (not in cooked corpus, invisible to the gate):
- mc2_01.beauty/{height_delta.r32, protected.r8, sidecar.json}, mc2_24.beauty/{height_delta.r32, protected.r8, sidecar.json}: height_delta.r32 and protected.r8 are runtime names but absent from the deploy mc2_01/mc2_24 .beauty dirs. Real output or leftover: undetermined; needs owner decision.
- mc2_01/ (masks/, report.json): likely leftover.
- mc2_10.beauty/*.bak-bilinear: backups. Do NOT commit. Move out of tree or delete.

Proposed fix (not applied):
1. py -3 scripts/check-beauty-sidecars.py --write
2. py -3 scripts/check-beauty-sidecars.py --no-lanes --quiet   (expect exit 0)
3. git add docs/beauty-sidecar-inventory.txt
   git commit -m "docs(beauty): regenerate sidecar inventory for mc2_10 visual_damp" (+ Co-Authored-By trailer)
4. Set aside (confirm first): the .bak-bilinear files and mc2_01/ dir; decide on the untracked height_delta.r32/protected.r8/sidecar.json (commit with a tracked cook, or delete).
5. Redeploy: not needed for this check. --install only copies missing files; drifted deploy copies (listed above) need an explicit owner-approved copy.
