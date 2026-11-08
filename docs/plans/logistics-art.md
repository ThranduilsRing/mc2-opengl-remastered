# Logistics art lane

Assignment: slot11, claude/logistics-art, base f269b312d8328aaafccec0b53a15f23ce5775efb.

Completed: read canonical rules and mech atlas memory. Briefing loads packet 3
of the mission PAK (not a loose per-mission TGA). EditorData_tacmap.cpp generates
the stock 128px image. icon_registry already supports named bay images but HUD
prefers atlas cells; legacy filtering explicitly copies neighbouring cells.

Next: implement reproducible terrain/map and icon cook; preserve map coordinates
and mod precedence; add named HUD lookup and fallback doctest; cook review contact
sheets; run covering validators, box build/unit, final Windows compile and one
background logistics capture. Commit locally only. Budget <=150 calls.

Evidence: pending. No installs or scratch created yet.
