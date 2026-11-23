"""Compare the same-frame HDR linear F16 scene with undithered mode 0 RGB8."""
import argparse
from pathlib import Path
import re

import numpy as np
from PIL import Image


def compare(raw: Path, ppm: Path) -> dict:
    match = re.fullmatch(r"(.+\.frame-(\d+))\.f16\.raw", raw.name)
    if not match or ppm.name != match[1] + ".sdr.ppm":
        raise ValueError("expected a same-prefix, same-frame .f16.raw/.sdr.ppm pair")
    with ppm.open("rb") as stream:
        if stream.readline() != b"P6\n":
            raise ValueError("expected P6 reference")
        expected = f"# frame={int(match[2])} mode=0 dither=0 scene-only\n".encode()
        if stream.readline() != expected:
            raise ValueError("missing same-frame, undithered mode 0 receipt")
    with Image.open(ppm) as image:
        reference = np.asarray(image.convert("RGB"), dtype=np.int16)
    height, width, _ = reference.shape
    linear = np.fromfile(raw, dtype="<f2")
    if linear.size != width * height * 4:
        raise ValueError("F16 byte count does not match reference dimensions")
    linear = linear.reshape(height, width, 4)[::-1, :, :3].astype(np.float64)
    if not np.isfinite(linear).all():
        raise ValueError("non-finite F16 scene")
    below = ((linear >= 0) & (linear <= 1)).all(axis=2)
    if not below.any():
        raise ValueError("no pixels below paper white")
    positive = np.maximum(linear, 0)
    encoded = np.where(positive <= 0.0031308, 12.92 * positive,
                       1.055 * positive ** (1 / 2.4) - 0.055)
    preview = np.floor(np.clip(encoded, 0, 1) * 255 + 0.5).astype(np.int16)
    error = np.abs(preview - reference).max(axis=2)[below]
    violations = int((error > 1).sum())
    return dict(frame=int(match[2]), width=width, height=height,
                compared=int(below.sum()), excluded=int((~below).sum()),
                max_codes=int(error.max()), violations=violations, passed=violations == 0)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("f16", type=Path)
    parser.add_argument("sdr", type=Path)
    args = parser.parse_args()
    try:
        result = compare(args.f16, args.sdr)
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}")
        return 1
    print(("PASS" if result["passed"] else "FAIL") + " " +
          " ".join(f"{key}={value}" for key, value in result.items()) + " bar=1/255")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
