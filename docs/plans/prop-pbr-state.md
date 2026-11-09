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
Next: compile/contracts/default-table doctest; box build/unit/smoke; compare
base/day/night captures and inspect montages; final PC compile/lint; cleanup.
Base bookmark added; PBR indirect uses a broad SH reflection approximation
(there is no prefiltered specular environment in the native prop feed).
Shader validation: all 79 production GLSL stages compiled clean (check_vk_shaders).
Baseline capture rows all exit 0, but box wrapper task 62 exits 1; images fetched.
First ratchet caught pair .second use; replaced by structured bindings.
Local contracts hit WSL insufficient-system-resources (HCS 0x800705aa), not a
shader failure. Windows Ninja compile started at fb96db293, still pending.
Box combined proof submission at fb96db293 pending; must repeat build/unit/lint
at the next tip after the ratchet fix. No runtime pass claimed yet.

Compile correction: TG_TypeShape owns TinyTexture handles, not filename strings.
Resolve its mcTextureNodeIndex through mc2::textures::name, with safe unknown
fallback when the service/node is unavailable. Prior box jobs 69/72/75 compile
the older invalid source; they are not final proof. Final receipts and remaining
steps will be kept in E:/mc2-archive-2026-10/proofs/prop-pbr/state.md.
