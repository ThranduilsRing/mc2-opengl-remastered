<!-- doc-status: current | reviewed:2026-10-11 -->
# mc2texcook

Cook tool for MC2 OpenGL modding.  Converts TGA/PNG/EXR source art to KTX2
(RGBA8, uncompressed, full mip chain).

## Requirements

- Python 3.8+
- Pillow: `pip install Pillow`

## Usage

```
py -3 tools/mc2texcook/mc2texcook.py <input> --preset <preset> [--output <out.ktx2>]
```

If `--output` is omitted, the output file is written next to the input with
the extension replaced by `.ktx2`.

## Presets

| Preset | vkFormat | Description |
|--------|----------|-------------|
| `albedo` | `VK_FORMAT_R8G8B8A8_SRGB` (43) | sRGB color + alpha, full mip chain |
| `emissive` | `VK_FORMAT_R8G8B8A8_SRGB` (43) | same as albedo |
| `normal` | `VK_FORMAT_R8G8B8A8_UNORM` (37) | linear, no color space conversion |
| `orm` | `VK_FORMAT_R8G8B8A8_UNORM` (37) | R=AO G=roughness B=metalness A=255 |
| `mask` | `VK_FORMAT_R8G8B8A8_UNORM` (37) | grayscale to R=G=B=value A=255; RGBA passthrough |

## Examples

```
# Cook albedo map (sRGB):
py -3 tools/mc2texcook/mc2texcook.py art/mech_body_d.tga --preset albedo

# Cook normal map (linear):
py -3 tools/mc2texcook/mc2texcook.py art/mech_body_n.png --preset normal

# Cook ORM map with explicit output path:
py -3 tools/mc2texcook/mc2texcook.py art/terrain_orm.tga --preset orm --output cooked/terrain_orm.ktx2
```

## Output format (KTX2, supercompression=NONE)

```
offset   0 : magic (12 bytes)  \xabKTX 20\xbb\r\n\x1a\n
offset  12 : vkFormat, typeSize, pixelWidth, pixelHeight,
             pixelDepth, layerCount, faceCount, levelCount,
             supercompressionScheme  (9 x uint32 LE)
offset  48 : dfdByteOffset, dfdByteLength,
             kvdByteOffset, kvdByteLength  (4 x uint32 LE)
offset  64 : sgdByteOffset, sgdByteLength  (2 x uint64 LE)
offset  80 : level index  (levelCount x 24 bytes: 3 x uint64 LE per entry)
offset 80+levelIndex : DFD (80 bytes for RGBA8)
after DFD : mip pixel data, mip 0 (largest) first
```

Level index entry fields: `byteOffset, byteLength, uncompressedByteLength`
(all absolute from start of file; byte_length == uncompressed_byte_length
for sc=NONE).

KV data is empty (kvdByteLength=0).  SGD is absent (sgdByteOffset=0,
sgdByteLength=0).

## Running tests

```
py -3 tools/mc2texcook/tests/test_mc2texcook.py
```

All tests use synthetic in-memory images.  No asset files are required.
