"""Compare complete frozen MLR submission/state records without numeric tolerances."""
import argparse
import hashlib
from pathlib import Path
import struct


def records(data):
    offset = 0
    count = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError('truncated record length')
        size, = struct.unpack_from('<Q', data, offset)
        offset += 8
        if size == 0 or offset + size > len(data):
            raise ValueError('empty or truncated record')
        offset += size
        count += 1
    if count == 0:
        raise ValueError('empty snapshot')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    try:
        before, after = args.before.read_bytes(), args.after.read_bytes()
        counts = records(before), records(after)
    except (OSError, ValueError) as error:
        print(f'invalid snapshot: {error}')
        return 2
    if before != after:
        offset = next((i for i, (a, b) in enumerate(zip(before, after)) if a != b),
                      min(len(before), len(after)))
        print(f'different: first byte={offset} records={counts} bytes={(len(before), len(after))}')
        return 1
    print(f'identical: records={counts[0]} bytes={len(before)} sha256={hashlib.sha256(before).hexdigest()}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
