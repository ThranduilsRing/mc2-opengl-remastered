# Water shore lift

Assignment: mc2-slot08 / claude/water-shore-lift / base f269b312d8328aaafccec0b53a15f23ce5775efb.

Implemented: disable mode-3 depth lift only for the current mission's imported visual terrain bake. Sim-only terrain keeps the previous tile-top cover. SSR untouched. Shared production shader math has doctests for both paths.

Next: compile production shaders and engine; shader contract rows; box build/unit/smoke-lite; VK A/B at mc2_01_saved_205644, a low shore, and mc2_10 night. Review montages. Final Windows compile and lint. Record receipts and cleanup before hand-back.

Initial proof at 1832b669f: PC vk_shaders_compile PASS (92.61 s). Box build exposed a base include-order bug: appear.h hid <atomic> behind DAPPEAR_H, so already including dappear.h made std::atomic undefined. Moved standard includes outside that guard in a separate prerequisite repair. Windows build and targeted doctests are in progress. Final receipts belong under E:/mc2-archive-2026-10/proofs/box/mc2-slot08/<tip8>/; capture baseline uses the original base water VS SPIR-V with the final executable, then ab.py --compare-only.
