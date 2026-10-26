# Native Linux

Build on a Linux filesystem (WSL ext4, not `/mnt/a`). Keep a full Git history and
materialized LFS/vendor assets: launch provenance checks require Git ancestry.
The tested toolchain is Clang/libc++ 20, lld, Vulkan-Headers 1.4.350,
DXC 1.9.2609 and Slang 2026.20. Install system development packages for SDL2,
SDL2_mixer, SDL2_ttf, zlib, FFmpeg (avformat/avcodec/avutil/swscale/swresample),
plus CMake, Ninja, glslang-tools and spirv-cross. Set `JOBS` to the lane allocation.

```sh
cmake -S . -B build64-ninja -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_CXX_FLAGS=-stdlib=libc++ -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld \
  -DVulkan_INCLUDE_DIR="$HOME/toolchains/vulkan/include" \
  -DMC2_DXC="$HOME/toolchains/dxc/bin/dxc" \
  -DMC2_SLANGC="$HOME/toolchains/slang/bin/slangc"
nice -n 10 ninja -C build64-ninja -j"$JOBS" mc2
cmake -S tests/unit -B build64-tests -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++-20 -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
nice -n 10 ninja -C build64-tests -j"$JOBS" mc2_tests
nice -n 10 ./build64-tests/mc2_tests
```

Set `INSTALL` and `PROOF_DIR` to the lane's assigned install and proof paths.
Deploy committed HEAD into one explicit stock-derived install with
`python3 scripts/deploy_payload.py "$INSTALL" --target game --ninja --allow-new-target --classic-assets`.
The launcher, crash dialog and socket dev shell remain Windows-only. Linux uses
SDL Vulkan surfaces and Tracy's monotonic clock for guests without invariant TSC.

Install Xvfb, mesa-vulkan-drivers and Python pytest/psutil. Then run through the harness:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json LP_NUM_THREADS=4 \
OMP_NUM_THREADS=1 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
nice -n 10 xvfb-run -a python3 tools/sim_determinism.py check mc2_10 \
  --install "$INSTALL" --sim-speed 4 --out "$PROOF_DIR"
```

Linux chains live in `tests/smoke/sim_determinism/linux/`; Windows pins stay separate.
The Linux fixture selects 320x180, Potato shadows and no spatial AA for software
rendering. Its vegetation smoke floors are inapplicable; the determinism tool
still requires 300 contiguous ticks, seed/data identity and every simulation digest.
Use the committed stock-plus-overlay data pin. Extra texture `.cookstamp` receipts
in a seed install change its data identity; restore the pinned dataset before recording.
