"""Freeze physical texture sources without guessing whether painted art is unlit."""
import argparse
import collections
import csv
import hashlib
import json
from pathlib import Path
import struct


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', action='append', required=True)
    ap.add_argument('--out', required=True)
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    counts = collections.Counter()
    with (out / 'texture-sources.csv').open('w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        writer.writerow(['root', 'path', 'bytes', 'encoding_evidence', 'lighting_provenance', 'sha256'])
        for root in map(Path, args.root):
            for p in sorted(root.rglob('*')):
                if not p.is_file() or p.suffix.lower() not in {'.tga', '.jpg', '.jpeg', '.png', '.dds', '.ktx2', '.exr', '.hdr'}:
                    continue
                raw = p.read_bytes()
                encoding = 'unspecified; verify loader and metadata'
                if p.suffix.lower() == '.ktx2' and raw[:12] == b'\xabKTX 20\xbb\r\n\x1a\n':
                    encoding = 'KTX2 vkFormat=' + str(struct.unpack_from('<I', raw, 12)[0])
                elif p.suffix.lower() in {'.exr', '.hdr'}:
                    encoding = 'HDR container; verify linear units at consumer'
                provenance = 'legacy/unknown compatibility; not validated unlit'
                if '.burnin' in p.name.lower():
                    provenance = 'prelit terrain; mission illumination multiplied during bake'
                counts[(encoding, provenance)] += 1
                writer.writerow([str(root), p.relative_to(root).as_posix(), len(raw), encoding, provenance, hashlib.sha256(raw).hexdigest()])
    (out / 'texture-summary.json').write_text(json.dumps({
        'roots': args.root,
        'scope': 'Physical loose sources only. FST/PAK members and embedded GLB images require separate extraction; absence here is not absence in VFS.',
        'classes': [{'encoding': k[0], 'lighting_provenance': k[1], 'count': v} for k, v in sorted(counts.items())],
        'policy': 'No inverse burn-in. sRGB storage does not establish unlit provenance. Unknown mod art stays compatibility-classified.'
    }, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
