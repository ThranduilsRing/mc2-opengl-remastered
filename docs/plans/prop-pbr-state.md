# Prop PBR lane

Assignment: slot06, claude/prop-pbr, base 8fc0c6c89576.
Lighting/material changes only; no art. Budget <=150 calls.

Inspection: base mech world shader is legacy diffuse plus Blinn specular,
not the PBR asset-preview workflow described by the owner memory. Props already
support override normal/ORM maps; stock packets have no native material row.
Authored prop lighting still stacks baked seed on SH and imposes an alpha floor.

In progress: stock default rows, shared GGX direct lighting, per-fragment prop
composition with SH only and no baked fill; inspector labels.
Baseline box capture job 62 at base (day/night and mc2_03 building faces).
Next: compile/contracts/default-table doctest; box build/unit/smoke; create base
bookmark and compare captures, inspect montages; final PC compile/lint; cleanup.
No proof is green yet.
