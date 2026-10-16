"""Rebuild numbered MPEG-4 frames and a deterministic 440 Hz PCM WAV; retain SHA-256s."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parents[2]))
from scripts.console_child import child_kwargs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg"))
    args = parser.parse_args()
    if not args.ffmpeg:
        parser.error("ffmpeg executable required")
    frames = ROOT / "assets-source"
    frames.mkdir(exist_ok=True)
    movies = ROOT / "mod/data/movies"
    sounds = ROOT / "mod/data/sound"
    movies.mkdir(parents=True, exist_ok=True)
    sounds.mkdir(parents=True, exist_ok=True)
    # Decimal labels make captures identifiable without installed fonts.
    digits = ("111101101101111", "010110010010111", "111001111100111", "111001111001111",
              "101101111001001", "111100111001111", "111100111101111", "111001010010010")
    for number, digit in enumerate(digits):
        rgb = bytearray()
        for y in range(64):
            for x in range(96):
                lit = 36 <= x < 60 and 12 <= y < 52 and digit[((y-12)//8)*3 + (x-36)//8] == "1"
                rgb.extend((255, 255, 255) if lit else (number * 30, 32, 224 - number * 24))
        (frames / f"frame{number:02}.ppm").write_bytes(b"P6\n96 64\n255\n" + rgb)
    video = movies / "e6e_numbered.mp4"
    command = [args.ffmpeg, "-y", "-r", "4", "-i", str(frames / "frame%02d.ppm"),
               "-an", "-vcodec", "mpeg4", "-pix_fmt", "yuv420p", str(video)]
    result = subprocess.run(command, capture_output=True, timeout=60, **child_kwargs(redirected=True))
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    with wave.open(str(sounds / "e6e_tone.wav"), "wb") as wav:
        wav.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
        wav.writeframes(b"".join(struct.pack("<h", round(8192 * math.sin(2 * math.pi * 440 * n / 22050)))
                                for n in range(11025)))
    files = sorted(frames.glob("*.ppm")) + [video, sounds / "e6e_tone.wav"]
    hashes = {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}
    (ROOT / "assets.json").write_text(json.dumps({"recipe": "8 decimal-numbered RGB frames, 4 fps; 0.5s mono s16 22050Hz 440Hz amplitude 8192",
                                                "sha256": hashes}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
