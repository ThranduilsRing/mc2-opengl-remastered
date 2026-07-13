#!/usr/bin/env python3
"""
make_chime.py -- sound-override half of the "hello world" example mod.

Why this script *synthesizes* audio instead of recoloring a copy of the
stock file (the way recolor_cursor.py does for the texture):

The real stock data/sound/music27.wav (the game's main-menu / "Splash
Screen Loop" tune, see the README for how that filename was verified) is
encoded as Microsoft ADPCM, not plain PCM. Python's standard-library `wave`
module can only *read* uncompressed PCM WAV data (`wave.Error: unknown
format: 2` on the real file) and decoding/re-encoding ADPCM correctly needs
a real audio codec library -- out of scope for a "tiny script" example.

So instead this script generates a brand-new, short PCM16 mono WAV chime
from scratch using only the standard library (`wave`, `math`, `struct` --
no Pillow, no ffmpeg, no third-party audio package) and writes it to
mod/data/sound/music27.wav, i.e. the exact relative path that shadows the
stock file. PCM16 mono is what the stock file's own config declares
(data/sound/sound.snd -> [DigitalMusic]: StreamBitDepth=16,
StreamChannels=1), so this is a drop-in-compatible replacement format even
though the original bytes were compressed differently.

IMPORTANT: read the README's "Known limitation" box before expecting to
hear this in-game -- the engine's music/speech streaming path loads this
file through a raw OS file open that does not consult MC2_ACTIVE_MOD, so
this override will NOT audibly replace the stock tune when played as a mod.
This script/payload is included because the tutorial asked for one real
sound override built the same way as the texture one, and the honest
result -- including the limitation -- is itself the lesson.

Usage:
    py -3 make_chime.py
"""
import math
import os
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
DST = os.path.join(HERE, "..", "data", "sound", "music27.wav")

SAMPLE_RATE = 44100  # matches DigitalStreamBufferSize=88200 in sound.snd (44100Hz * 2 bytes * 1s mono)
AMPLITUDE = 0.35  # keep it well under clipping
# A simple ascending three-note chime (C5, E5, G5), ~0.9s total.
NOTES_HZ = [523.25, 659.25, 783.99]
NOTE_SECONDS = 0.3


def make_tone(freq_hz: float, seconds: float) -> list:
    n = int(SAMPLE_RATE * seconds)
    samples = []
    for i in range(n):
        t = i / SAMPLE_RATE
        # Simple attack/decay envelope so notes don't click.
        envelope = min(1.0, t / 0.02, (seconds - t) / 0.05)
        envelope = max(0.0, envelope)
        value = AMPLITUDE * envelope * math.sin(2 * math.pi * freq_hz * t)
        samples.append(int(value * 32767))
    return samples


def write_wav(dst_path: str) -> None:
    all_samples = []
    for freq in NOTES_HZ:
        all_samples.extend(make_tone(freq, NOTE_SECONDS))

    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    with wave.open(dst_path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)  # 16-bit
        w.setframerate(SAMPLE_RATE)
        frames = b"".join(struct.pack("<h", s) for s in all_samples)
        w.writeframes(frames)

    print(f"wrote {os.path.abspath(dst_path)} ({len(all_samples)/SAMPLE_RATE:.2f}s, "
          f"{SAMPLE_RATE}Hz, mono, 16-bit PCM)")


if __name__ == "__main__":
    write_wav(DST)
