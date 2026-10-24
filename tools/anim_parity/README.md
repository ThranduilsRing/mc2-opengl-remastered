# Animation parity

Read-only audit of effective stock FST/loose animation tables, AGL tracks, TGL
hierarchies, and authored pristine GLB LOD0/1/2 exports. Uses numpy and Pillow,
already required by the content tools. No Blender or engine build required.

```powershell
py -3 tools/anim_parity/audit.py
py -3 -B -m pytest tools/anim_parity/test_audit.py -q -p no:cacheprovider
bash scripts/check-contracts.sh --rows '^anim_parity$' -j6
```

Paths may be overridden with `--install`, `--art`, `--catalog`, `--placements`, `--out`.
The report describes coverage and geometric limits. FAIL findings are report
data, not a process error; malformed inputs are retained as unresolved rows.
Exit zero means the report was generated, not that the art passed.

The current kit cooker/importer binds tracks by case-insensitive node name.
Static-parent names, rotation-only pivot distances and bounds heuristics are
WARN, not proof of broken playback. REAL failures cover missing moving nodes,
position-track frame errors and lost animated ancestors in the cooked hierarchy.

`evidence.md` is included verbatim in each generated REPORT.md. It records the
audited engine trigger sites and the reason no in-game proof was attempted.
