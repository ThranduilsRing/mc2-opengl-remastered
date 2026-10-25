# Vulkan descriptor occupancy

Regenerate the live GLSL census:

```
py -3 scripts/check-binding-slots.py --json docs/render-backend-seams/binding-slot-occupancy.json
py -3 scripts/check-vulkan-bindings.py --strict
```

`binding_slots` expands local includes per Vulkan stage, strips comments, and
keys buffer, sampler and image descriptors by `(set, binding)`. Different resource
types share this namespace. Distinct resources on the same stage slot fail unless
on exclusive preprocessor branches. Numeric set macros use the including stage's
override before the include's default (`TERRAIN_RESOURCE_SET`). This is a source
census, not a replacement for shader compilation/reflection.

The explicit C++/GLSL lockstep surface is seven live descriptors in
`GameOS/gameos/vulkan_mech_island.cpp`: `MechFrameUBO`, `InstanceBuffer`,
`BoneBuffer`, `uAlbedo`, `LightsData`, `uMechAo`, `uMechNrm`.
The checker reads their C++ descriptor layout records (`b[0..4]`, `b[6..7]`)
and set index in `layouts[]`, then compares set, binding and type with the live
`mech_tier1` stages. Missing pairs fail. Other C++ layouts are outside this
explicit lockstep surface; the retired GL macros and fixture binding are gone.

`vulkan_bindings` scans live source by default (never the saved census), groups
stages using `pipeline-shader-map.json`, and fails on distinct resources sharing
one set/binding in a pipeline. Disjoint pipelines have independent descriptor
layouts. `--strict` also fails on unmodeled shader stages; an empty census fails.
The saved JSON is for inspection or explicit `--json` input only.

Slang descriptors are covered separately by the Slang contracts.
