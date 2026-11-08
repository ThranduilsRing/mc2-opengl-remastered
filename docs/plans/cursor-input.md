# Cursor input lane

Assignment: A:/Games/mc2-slot04, claude/cursor-input, base 8fc0c6c8957673ab0be789403a10b91b852c59b2.

Plan (150 calls maximum): trace stock cursor/decode/focus/load paths; key cursor
art, cache SDL colour cursors with a software fallback, pump load events; add
conversion and shape mapping doctests; commit; Linux build/unit on box and final
Windows Ninja/covering contracts/doctests/lint; isolated PC smoke and menu capture.

Found: UserInput draws StaticInfo through HUD or ImGui foreground; gos_render hides
the OS cursor. Texture decode only applies magenta conversion to PNG, not TGA.
LoadScreen explicitly hides the cursor; Mission::init runs synchronously.

Receipts: none yet. Next: implementation and focused offline proof.
