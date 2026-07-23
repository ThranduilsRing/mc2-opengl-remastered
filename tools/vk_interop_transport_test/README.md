# vk_interop_transport_test — offline interop-transport transparency harness

Part of **VK-INTEROP-BRIDGE-1** ("TOOLS / offline harnesses"). A standalone,
headless executable that proves — in **seconds, without launching the game** —
that the GL↔VK zero-copy interop *transport* is transparent for a given render
pass. It exists so per-piece proof no longer depends on the ~460 s full-game
golden-parity launch. That in-game run stays the FINAL confirmation; this is the
fast per-piece gate before it.

## The idea

The interop path and a readback path render the **same VK draw** — only the
image-transport mechanism differs. If both outputs are byte-identical, the
transport is proven transparent for that shader class.

```
                 same golden input, same VkPipeline, same UBO
                                   │
              ┌────────────────────┴────────────────────┐
   (A) READBACK path                          (B) INTEROP path
   VK renders the pass into a plain           VK renders the SAME pass into an
   (non-exported) VkImage                     EXPORTABLE VkImageResource
        │                                          │
   vkCmdCopyImageToBuffer → host bytes        GL imports it zero-copy, waits the
   ("GL upload" stand-in)                     VK→GL semaphore, blits it into a
        │                                     gl-owned tex, glReadPixels → bytes
        └──────────────► ASSERT byte-identical ◄──────┘
                    + forbidden-counters == 0
                    + VK validation errors == 0
```

- **(A)** is the bytes GL *would* have received via a readback+upload round-trip.
- **(B)** never touches the shared image with `vkCmdCopyImageToBuffer` or
  `glGetTexImage`; it only crosses the boundary zero-copy. A **forbidden-counter**
  guard (mirroring the in-engine `#5` assert) asserts those transport ops stayed
  at zero on the shared-image path.

## What it proves / does NOT prove

- **Proves:** for this shader class, the zero-copy shared-image transport
  produces bit-identical output to the readback transport, with no illicit
  copy/readback and no validation error. The transport is transparent.
- **Does NOT prove:** correctness of the *whole* in-game fog/postprocess region
  on real mission pixels. It uses a synthetic golden input and a stand-in shader.
  The in-game `run_golden_parity.py` on a real mission is still the last word.
  It also only covers the format/usage it is instantiated with (RGBA16F color).

## Build & run

Same standalone pattern as `tools/vk_gl_interop_spike`. Links only SDL2, GLEW,
vulkan-1, opengl32; touches nothing in mclib/gameos. Shaders are compiled to
SPIR-V and **embedded at build time** (glslangValidator `--vn`), so the exe has
no runtime shader-file dependency.

```powershell
$CMAKE = "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
cd tools/vk_interop_transport_test
& $CMAKE -G "Visual Studio 17 2022" -A x64 -B build64
& $CMAKE --build build64 --config RelWithDebInfo
build64/RelWithDebInfo/vk_interop_transport_test.exe
```

- **Exit 0** = PASS (byte-identical, forbidden==0, validation-clean).
- **Exit 1** = a setup/render stage failed (message printed).
- **Exit 2** = validation/GL-debug errors present.
- **Exit 3** = outputs DIVERGED (first-divergence byte/texel printed).
- **Exit 4** = interop path issued a forbidden shared-image copy/readback.

A result log is written next to the exe as `vk_interop_transport_test-result.log`.

## Reusing it for another piece (the whole point)

The two transport paths, byte-compare, forbidden counters and validation are all
**pass-agnostic**. To test a different pass:

1. **Different shader:** edit `shaders/pass_under_test.frag` (keep it a pure
   function of the sampled input so both paths stay bit-comparable). Rebuild —
   CMake recompiles+re-embeds the SPIR-V automatically.
2. **Different input content:** edit `buildGolden()` in `main.cpp` (deterministic,
   seeded-constant — no rng/time).
3. **Different format (e.g. depth D32 per T0):** change `IMG_FORMAT`,
   `GL_INTERNAL_FMT`, `BYTES_PER_TEXEL` at the top of `main.cpp`, adjust the
   `glReadPixels` format/type, and switch the aspect/usage in `createTarget`
   to `VK_IMAGE_ASPECT_DEPTH_BIT` / depth usage. T0 already proved D32_SFLOAT
   sampled VK→GL is viable on this GPU, so a depth variant is a minimal edit.
4. **More inputs / a UBO:** extend the descriptor set layout in `buildPipeline`
   and the descriptor writes; both paths share the pipeline, so they stay in sync.

## Notes

- Headless: a hidden 64×64 SDL window supplies the GL context only; all rendering
  is offscreen (256×256 RGBA16F).
- LUID match (GL↔VK same GPU) is checked; single-GPU boxes pass automatically.
- Third-party Vulkan layers (e.g. a Galaxy overlay) may emit naming **WARN**s —
  those are warnings, not errors; the harness counts errors only.
