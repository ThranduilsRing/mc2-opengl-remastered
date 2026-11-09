# VFS string bounds lane

Assignment: mc2-slot04, claude/vfs-stringview, base f269b312d8328aaafccec0b53a15f23ce5775efb.

Implemented bounded NormalizeKey, path classification and cache identity inputs;
CRT cache paths remain explicitly NUL-terminated. Direct callers pass views.
Added bounds, short/empty/null input and embedded-NUL checks to PathsAndMods;
existing cases cover cache reload and mod precedence.

Initial ratchet: PASS, no reduction against baseline; recorded VFS LG06 = 21.
Next: final ratchet, compile, VFS/mod rows and PathsAndMods doctests; commit;
deploy to the orchestrator-seeded vfs-stringview install, smoke-lite and six mods;
final lint and cleanup. No generated baseline edits.
