# Shadow submission lane

Assignment: mc2-slot01, claude/shadow-submit, base 609d072946c4256da454185255879b59da285a12.

Change: preserve mover masks and skin-generated index/instance ranges; batch
commands by bind-pose/skinned vertex buffer in each shadow map. Retired frame
slots own command buffers; map slices never overwrite earlier commands.
Allocation, upload and capacity failures fall back to all direct draws.
Static indirect batching and the performance ceiling are unchanged.

Attribution: draws.shadow_mover counts mover API calls; shadow.mover_records
counts their original direct commands; shadow.local_mover_records isolates
the commands for local-light tiles. draws.shadow_local is the subset of
total API calls made for local-light tiles (including static bakes). Static
calls = draws.shadow - draws.shadow_mover. Local calls overlap those groups.

Proof directory: E:/mc2-archive-2026-10/proofs/shadow-submit.
Pending: baseline smoke receipt; final build, shadow math/caster doctests,
shadow contract rows, sync-validation smoke on mc2_01/mc2_10, final lint,
local commit, receipts and cleanup. No PC timing or captures.
