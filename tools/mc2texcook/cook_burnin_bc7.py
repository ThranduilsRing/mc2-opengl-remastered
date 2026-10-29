"""Cook a burn-in or baked building image to stored BC7, using the CPU.

Never replaces the source. Uses the common content pipeline's mip/BC7/KTX2
implementation; cache is bypassed so independent runs exercise the encoder.
"""
import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.content_pipeline import provenance, texcook


def cook(source, output, preset="burnin"):
    source, output = Path(source), Path(output)
    if source.resolve() == output.resolve():
        raise ValueError("output must not overwrite source")
    if output.suffix.lower() != ".ktx2":
        raise ValueError("output must have .ktx2 extension")
    raw = source.read_bytes()
    rgba = texcook.load_rgba(raw)
    if preset == "burnin":
        # The shipped colormap uses linear UNORM and samples mip 0 only.
        vk, blocks = texcook.encode_levels([rgba], "orm")
        data = texcook.write_ktx2(vk, rgba.shape[1], rgba.shape[0], blocks)
    elif preset == "base_color":
        data = texcook.cook_ktx2(rgba, "base_color", cache_dir=None)
    else:
        raise ValueError(f"unknown preset {preset}")
    vk, w, h, levels = texcook.read_ktx2(data)
    expected_vk, expected_mips = (145, 1) if preset == "burnin" else (146, max(w, h).bit_length())
    if vk != expected_vk or len(levels) != expected_mips:
        raise ValueError("invalid BC7 mip chain")
    for i, level in enumerate(levels):
        if len(level) != ((max(1, w >> i) + 3) // 4) * ((max(1, h >> i) + 3) // 4) * 16:
            raise ValueError(f"invalid BC7 level {i}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_suffix(".ktx2.tmp")
    temp.write_bytes(data)
    temp.replace(output)
    texcook.record_cook(output, provenance.sha256(raw),
                       {"preset": preset, "width": w, "height": h, "mips": len(levels),
                        "encoder": "texconv-cpu", "resize": "none"})
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--preset", choices=("burnin", "base_color"), default="burnin",
                        help="burnin: linear mip 0; base_color: sRGB building mip chain")
    args = parser.parse_args()
    print(cook(args.source, args.output or args.source.with_suffix(".ktx2"), args.preset))


if __name__ == "__main__":
    main()
