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

Completed: traced detail-normal, slab-detail and road early-out paths; added
focused camera poses. Next: baseline captures and atlas inspection, patch,
commit, final-tip proof, inspect montages, cleanup and hand-back.
