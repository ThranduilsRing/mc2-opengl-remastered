# AA default SMAA lane state

Assignment: slot09, claude/aa-default-smaa, base f269b312d.
Legacy SpatialAA=1 already names SMAA in prefs/UI, but present forced FSR for all nonzero values.
Keep 1 as SMAA (fresh and existing files); add explicit opt-in 4=FSR native AA.
No file rewrite/version marker is necessary: old values 0..3 unambiguously name spatial choices.
Both options UIs expose 4; present and sampler bias use the shared prefs temporalAA predicate.
The existing provider deregistration clears jitter when no temporal provider is selected.
Added prefs round-trip/default regression coverage; extended existing bounds checks.

Next: covering contract/doctest rows, final Windows compile, box build/unit/smoke/lint,
active-AA log check, VK day/night A/B; inspect images, record evidence, cleanup, local commit only.
Proof pending; do not treat implementation as validated yet.
