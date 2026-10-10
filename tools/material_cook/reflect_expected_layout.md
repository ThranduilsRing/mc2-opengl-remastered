<!-- doc-status: current | reviewed:2026-10-11 -->
# MaterialGpu — expected shader reflection layout

When a shader binds `MaterialTable` (the `MaterialGpu[]` SSBO), the
`tools/shader_reflect` golden for that shader should contain the following
SSBO entry.  Generate via `py -3 tools/shader_reflect/reflect.py --update`
once the binding is wired to a real shader variant (MaterialGpu-2+).

## Expected SSBO golden fragment

```json
{
  "binding": <TBD — binding 5 is free; assign in MaterialGpu-2>,
  "name": "MaterialTable",
  "members": [
    {
      "name": "materials",
      "offset": 0,
      "array_stride": 32,
      "matrix_stride": null,
      "type": "<spirv-cross anonymous struct ref>"
    }
  ]
}
```

## Per-member expected offsets

These derive from `RenderCore/MaterialGpu.h` `static_assert` block and must
survive as invariants in the reflect golden once the shader is compiled.
The mirror gate (`scripts/check-material-gpu-mirror.sh`) validates field
names and order between the C++ and GLSL files.

| Member                | offset | size | GLSL type |
|-----------------------|--------|------|-----------|
| albedoTex             |  0     | 4    | uint      |
| normalTex             |  4     | 4    | uint      |
| metallicRoughnessTex  |  8     | 4    | uint      |
| emissiveTex           | 12     | 4    | uint      |
| flags                 | 16     | 4    | uint      |
| baseColorFactor       | 20     | 4    | float     |
| metallicFactor        | 24     | 4    | float     |
| roughnessFactor       | 28     | 4    | float     |

Total struct size: 32 bytes.  `array_stride` in spirv-cross reflect: **32**.

## How to validate (MaterialGpu-1)

1. Create a minimal non-runtime shader fixture that includes `material_gpu.hglsl`.
2. Declare `layout(std430, binding=5) readonly buffer MaterialTable { MaterialGpu materials[]; } materialTable_;`.
3. Run `py -3 tools/shader_reflect/reflect.py --update` to bake the golden.
4. Inspect the generated golden: `array_stride` MUST be 32.
5. For each member listed above, verify `offset` matches the table.
6. Commit the golden; CI `reflect.py` guards it from drift.

## Existing bindings in static_prop.frag (coalesce variant)

| binding | name                               |
|---------|------------------------------------|
| 0       | Instances (GpuStaticPropInstance[]) |
| 1       | Colors (uint[])                    |
| 2       | PerType (PerTypeData[])            |
| 3       | ParityOut (uint[])                 |
| 4       | PerDrawData (PerDrawEntry[])       |

Binding **5 is free**. Assign when the MaterialGpu-2 runtime slice lands.
