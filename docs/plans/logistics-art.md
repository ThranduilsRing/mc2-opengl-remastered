# Logistics art lane

Assignment: slot11, claude/logistics-art, base f269b312d8328aaafccec0b53a15f23ce5775efb.

Completed: read canonical rules and mech atlas memory. Briefing loads packet 3
of the mission PAK (not a loose per-mission TGA). EditorData_tacmap.cpp generates
the stock 128px image. icon_registry already supports named bay images but HUD
prefers atlas cells; legacy filtering explicitly copies neighbouring cells.

Implemented: cook_logistics_art.py (FST/loose terrain, roads, catalog symbols,
named bay + three HUD front/back sizes, transparent border, sheets); optional
preview loader and named HUD preference with mod-priority fallback; doctest and
offline coordinate/border/packet tests; deploy enumeration for cooked PNGs.
Commits: fbbbc596b, d12205cd2. No pushes.

Cook: 43 maps including all 24 stock maps; two optional maps without colormaps keep
stock fallback. Named bay/HUD images have transparent borders, exact HUD palettes.
Reviewed stock/cooked coordinate comparison (01/03/10) and Starslayer/Hollander.
Mod atlas audit: CVE-G, REAP3, Exodus, MagicExpansion, Omnitech have custom atlases;
Carver5 does not. Details and contact sheets: E:/mc2-archive-2026-10/proofs/logistics-art.
Six source/cook tests passed on box before shipped-art and magenta regressions.
Initial box compile required the prescribed linux-build-fix prerequisite, merged
as b8f2d7aca. Queue unavailable (pueue missing); foreground box runner used.
PC Idle cook/test processes starved and were stopped; cook completed on box using
read-only v0.4 inputs. Private PC install seeded: E:/mc2-installs/pc-lane-11.

Next: commit regenerated art in batches <=25 files. At final tip run
box build/unit/logistics_art/kit_art/lint/smoke-lite and
one Windows Ninja compile, then deploy/capture briefing with
scripts/mechbay_preview_harness.py. Update this note with receipts before hand-back.
Cleanup: own G:/mc2-tmp/logistics-art + pytest scratch and remote cook-input/tar;
retain contact sheets/receipts and the single PC install. Budget <=150 calls.
