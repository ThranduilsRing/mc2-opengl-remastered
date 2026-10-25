# Shader technique skill audit

Source: https://github.com/gokayfem/shader-skills, pinned commit `a56e7738d3f22698e795f036b4919f0734ab621d` (MIT); detached reference checkout: `E:/mc2-ref/refs/shader-skills`.

Chosen skills: **none**. Read every upstream SKILL.md (one file: `skills/shader-art-coding/SKILL.md`). It fails the technique-only acceptance rule: Workflow steps 3 and 6 prescribe `python3 scripts/glsl_tools.py search`, `bundle`, and `check`; the following paragraph explicitly says to run the scripts. No upstream commands were executed, and no rejected prompt was installed.

Its water, terrain, noise and PBR reference topics are relevant, but they are supporting documents of that rejected skill, not independent installable skills. The entry point also requires Shadertoy output, prohibits #version/custom uniforms, and insists on replacing sine hashes even on keep-the-look ports: unsuitable as unchanged Vulkan engine guidance. Skipped the whole skill, including SDF/raymarch/generative-art sections (no current engine use justifying a separate import). No rewritten or selectively edited substitute was created.
