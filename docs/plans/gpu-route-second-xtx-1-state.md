# gpu-route-second-xtx-1

- Assigned slot01/branch; started clean at 120fd3780 (contains supplied base).
- DXGI attached outputs matched to Vulkan LUIDs in scripts/smoke_lib/gpu_adapters.py.
- Shared launcher and merge gate select runtime non-display name@uuid; lease slots compare UUIDs.
- Blender startup uses the same detector and enables the matching Cycles HIP device.
- Targeted pytest: nine modules green, including merge-gate device selection and host leasing.
- Next: final lint, commit, one build/deploy to E:/mc2-installs/gpu-route-second-xtx-1, smoke-lite; HIP proof before appending lane rule.
- Receipts and continued state: E:/mc2-archive-2026-10/proofs/gpu-route-second-xtx-1/ (no mods or pushes).
