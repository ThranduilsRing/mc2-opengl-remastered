# Prop record cache lane

Assignment: slot07, claude/prop-record-cache, base 609d072946c4256da454185255879b59da285a12.

Plan: cache CPU packet/run metadata by scene/material/resource generations;
persist uploads per retired frame slot; retain per-frame HZB compaction and commands.
Instance bytes include transient edits, CPU visibility and LOD; registry generation
also invalidates destruction. Map reset and material/texture imports bump counters.

Implemented: exact instance-content/registry scene generation; explicit material
and resource generations; retained CPU packet/run/list metadata and per-slot GPU
uploads. Each slot waits for core and late submission tickets before mutation.
HZB compaction and current-frame command recording still run. Material array-key
replacement refreshes routing; absent optional arrays no longer rebuild tables.
Doctests cover equal-count edits, visibility, destruction, reload and slot keys.

Evidence destination: E:/mc2-archive-2026-10/proofs/prop-record-cache/.
No proof receipts yet. Next: Ninja mc2, vk/props rows, prop_record_cache doctests;
validation smoke-lite in E:/mc2-installs/prop-record-cache; final lint; cleanup.
Source finalization precedes proof so the proved-at SHA is the lane tip. Final
receipt/status is written in the external proof folder without moving that tip.
