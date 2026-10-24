"""Cook stock prop sidecars in the sibling tier layout consumed at runtime."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import struct

from batch_cook import _cook_one_bc7, _DEFAULT_KTX_TOOL


def current(source, output):
    if not output.is_file() or output.stat().st_mtime_ns < source.stat().st_mtime_ns:
        return False
    header = output.read_bytes()[:80]
    return len(header) == 80 and header[:12] == b'\xabKTX 20\xbb\r\n\x1a\n' and struct.unpack_from('<I', header, 12)[0] == 145 and struct.unpack_from('<I', header, 40)[0] > 1


def cook(tgl, jobs=6):
    # Stock texels are never enlarged. All quality selections can share their
    # compressed native resolution; upgraded art can supply larger tier files.
    sources = sorted((tgl / '128').glob('*.tga'))
    def one(source):
        output = tgl / '512' / source.with_suffix('.ktx2').name
        if not current(source, output):
            _cook_one_bc7(source, output, str(_DEFAULT_KTX_TOOL), 0, preset='normal')
        for tier in (128, 256, 1024):
            sibling = tgl / str(tier) / output.name
            if tier == 128:
                continue  # Preserve source-manager sidecars and baseline semantics.
            if not current(source, sibling):
                sibling.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(output, sibling)
        return 1
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        count = sum(pool.map(one, sources))
    print(f'[PROP_COOK] sources={count} tiers=256,512,1024 format=BC7_UNORM mips=complete')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tgl', type=Path, required=True)
    parser.add_argument('-j', type=int, default=6)
    args = parser.parse_args()
    cook(args.tgl, args.j)
