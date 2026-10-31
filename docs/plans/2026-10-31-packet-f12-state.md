Assignment: A:/Games/mc2-lane9, claude/cxx23-packet-b, base c0eba1a0a.
Pre-flip clean HEAD 242e3e1bc reproduces both mc2fx and mc2fx_preview
LNK2019/LNK1120 drawArena failures in the existing build-alltargets tree (2s).
Receipt: E:/mc2-archive-2026-10/proofs/lane9/cxx23-f/f12-baseline-link.log.
Both source lists, shape.cpp and render_view_store.cpp are identical to main
c0eba1a0a (git diff empty): imported closure failure, independent of flip.
Fix: compile real code/render_view_store.cpp in both curated FX closures.
Next: all-target checkpoint, warm precheck tool-link guard, restore flip and
migration, clean-HEAD shipped builds/deploy and sequential acceptance monitor.
Sole existing install: E:/mc2-installs/claude-cxx23-packet-f; no extras created.

Closure checkpoint: full all-target build PASS (f12-all-targets.log);
guard pytest PASS (f12-guard-tests.log); native_render_hosts row PASS.
Warm FX target links PASS in 2.4s (f12-warm-link-monitor.log).
Precheck configures the FX targets and links both real executables.
Next: restore flip/migration and run clean-HEAD build/deploy/acceptance.
