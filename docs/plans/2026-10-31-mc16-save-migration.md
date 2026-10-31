# W95 mc2_16 first-save migration

Owner ruling: regenerate this reference from a first save of the merged
executable only after proving the pre-flip save differs by ObjectSlot tagging,
the W94 save still loads and continues at least 60 ticks, and masks do not grow.

The clean merged executable bb81ecd0fcf2 produced two byte-identical first
saves at the original 1000/1100 save/load frames and simulation speed 1.
The W94 save was written by c0eba1a0ac1c; its archived runner fingerprints
that executable. All 4,729 native object records are encoded, decoded and
reencoded with the production value codecs and match the merged ObjectSlots
exactly. The only section change is required OBJS v1, empty payload, +20
container bytes. Every unrelated W94 packet, FIT text and existing section
is identical. The W94 save loads in the merged executable and continues 601
contiguous ticks. Section digests are preserved; no SIMID continuation
equality is claimed for this music-dependent mission.

The previous historical pin stopped at D+E and carried the already accepted
Packet G gap: ABL packet 5702 and required ABLE. The comparison source for
the ObjectSlot proof is therefore the actual W94 first save, not a fabricated
upgrade of that older pin. The new reference is regenerated from the merged
first save. Its provenance retains the old pin and explains both migrations.
Object run-mask bytes shrink 30 -> 0; build masks remain 0. No other baseline,
mask or determinism pin changes. No exemption is needed; both new captures
are identical despite the mission's known music-dependent continuation.

Evidence: E:/mc2-archive-2026-10/proofs/merge95/mc16-proof.json,
mc16-migrate.log and legacy-w94-mc2_16/run/mc2_16.log. The two proof scripts
validate archived SHA256s and all bytes before writing the new reference.
