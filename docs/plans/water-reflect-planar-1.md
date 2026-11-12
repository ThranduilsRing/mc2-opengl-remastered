# Water planar reflection: recording seam

The owner reports a canopy-shaped hole over the reflected cliff at mc2_01
saved_210809. The producer still uses SSR; this change does not fix those pixels.

The brief permits a seam-only change when scene reuse requires a large refactor.
`GameOS/gameos/vulkan_color_prop_island.cpp` now separates `PropRecPlan` and
`record_props_cmds` from main-view preparation, matching `TerrainRecPlan` and
`record_terrain_cmds` in `vulkan_terrain_island_run.cpp`. The main-view callback
uses that recorder. Commands, barriers, alpha coverage and draw order are kept.
The plan supplies attachments, pipelines, descriptors and compaction buffers;
the command recorder does not consult the island or the camera globals.

Remaining integration, in dependency order:

1. Prepare reflected terrain visibility/LOD and static-prop instances from the
   world registry, independent of main-camera visibility. Terrain's public
   SubmitDrawCommands currently accepts the already selected main-view list;
   changing only its MVP cannot recover omitted geometry.
2. Prepare independent view UBOs, prop compaction buffers and compatible pipeline
   variants. Main prop winding is CLOCKWISE; reflection reverses it. Clip real
   world geometry below Terrain::waterElevation in terrain and prop shaders.
   Pin mirror-plane and winding math with offline tests when it is introduced.
3. Give the existing full-resolution RGBA16F reflection image colour-attachment
   usage and dedicated depth. Existing terrain/prop pipelines also write normal
   and optional object-ID attachments; a single-colour reflection pass requires
   compatible variants, not merely substituting its view into the scene pass.
4. Reuse both command recorders with independently prepared reflected plans.
   Keep main-view resources alive and unmodified until their submissions retire.
   Replace record_reflection's compute dispatch with that producer; preserve
   water bindings 8/11 and the approved glint/facet shaders. No history or gate.
5. Shader/layout and sync-val proof, production compile, isolated deploy,
   smoke-lite, then live-clock 4K owner/cliff/13-pose/night sheets. Owner accepts.

All GPU tiers: cost NOT MEASURED. No planar draw exists in this change.
Runtime proof for the visual fix remains NOT RUN. Slot scratch state and proof
receipts are recorded outside the worktree under the assigned slot/proof roots.
