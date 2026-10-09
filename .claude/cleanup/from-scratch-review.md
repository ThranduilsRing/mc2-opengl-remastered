# From-scratch review: what we'd do differently (read-only survey, 2026-10-09)
Evidence: grep counts + spot reads; Tracy needed for all perf claims.
Facts: ~237 files call getenv (static-local bool per probe); 378 vkCreate*/463 vkDestroy* in GameOS, 0 RAII wrappers in vulkan_*.cpp;
0 std::expected; 11.8k-line gameos_graphics.cpp, 11.2k gos_static_prop_batcher.cpp; GL+VK twins per feature (see gl-vk-duplication.md);
GpuStaticPropInstance hand-mirrored in 7 places; job_system exists only in code/ (frame_jobs.h); one inFlight fence wait per present
(engine_present_vk.cpp:1972) and per-slot wait in vk_image_resource.cpp:157; glGetBufferSubData at gameos_graphics.cpp:4533 (gated).
## Top-10 seams
1. Typed gate registry (replace scattered getenv; declare name/default/owner; one dump) - low effort, huge bug/iteration win.
2. Single-source GPU structs: C++ struct -> generated GLSL include + static_assert/offsetof check (kills 7-way LOCKSTEP).
3. RAII Vk/GL handle wrappers (unique handle + deleter queue) starting in one island.
4. Strong types for coordinate spaces (World/Stuff/GL/Screen; Vec3<Space>) at RenderCore boundary first.
5. Logging facade: std::format + source_location + category/level; printf backend first.
6. Result<T,E> (std::expected) for loaders/VK init instead of bool+out/asserts (needs /std:c++23 flip).
7. Render-graph-lite: pass table + resource declarations over existing RenderCore pass registry; retire GL twins as VK completes.
8. Job system promoted from code/ to engine: cull, terrain chunk, mech/mover update; Tracy first.
9. Unity/PCH + split giant TUs (giant-file-split-plan.md); include-what-you-use on gameos.hpp.
10. SoA for static prop/mech instances and mover hot fields (measure first).
## Don't rewrite
ABL interpreter, MC2 object hierarchy/save format, FST/pak IO, mission logic: mod compatibility and determinism outweigh payoff. No full ECS.
