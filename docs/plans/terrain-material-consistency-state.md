# Terrain material consistency

Assignment: slot07, claude/terrain-material-consistency; requested base f269b312d.
Starting tip: 8fc0c6c89 (contains the Linux build fix, clean checkout).
Findings 5, 13, 16 read; water and far-LOD shaders excluded.

Plan: inspect atlas rim; capture baseline; crossfade normal samplers at fw
0.4..0.6; replace altitude POM attenuation with footprint attenuation; share
moderate concrete relief; blend road edge coverage over shaded ground.
Proof: production Ninja build; shader/layout contract rows; box build/unit/
smoke-lite; ab.py montages for slope zooms, runway and mc2_10_saved_204523.
No performance claims. Local commits only.

Completed: traced detail-normal, slab-detail and road paths; added focused
camera poses. Inspected runway0000.tga: 248 distinct alpha values, including
the authored soft shoulder. Source contact sheet is in the proof directory.
Patch: anti-tile crossfade 0.4..0.6, POM footprint fade 0.4..0.8, concrete
relief 0.65 shared by material weights and pad detail, road coverage 0.3..0.7
composited over shaded ground with uniform derivative flow.

Baseline night capture passed at ffa93d56a (shader tree equals starting tip).
First day attempt failed on missing ignored bookmark; force-added the new
bookmark and restarted the day capture. Git MSYS scp reports child_copy /
Win32 error 299; use Windows native OpenSSH for box transport in this run.
Next: final-tip Windows compile, shader contracts and box build/unit/lint/
smoke-lite/captures; ab.py comparison and viewed montages; scratch cleanup.
Evidence and final status: E:/mc2-archive-2026-10/proofs/terrain-material-consistency/.
