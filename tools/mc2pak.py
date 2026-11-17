#!/usr/bin/env python3
"""Deterministic stock FST import and standard ZIP/Zstandard list/extract/verify."""
import sys
if sys.version_info < (3, 14):
    sys.exit("mc2pak: Python 3.14 or newer is required (ZIP Zstandard)")
import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import time
import zipfile
import zlib
from compression import zstd

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mc2x_import.fst import fst_entries, decompress_entry

COMMENT = b'MechCommander 2 asset pack (ZIP container, Zstandard entries). List/extract/pack with tools/mc2pak.py or the asset viewer.'

def key(name):
    return name.lower().replace('\\', '/')

def safe_path(root, name):
    target = (root / name).resolve()
    if not target.is_relative_to(root.resolve()) or '\\' in name or ':' in name:
        raise ValueError(f'unsafe entry name: {name}')
    return target

def writable(path):
    if path.resolve().is_relative_to(Path('A:/Games/mc2-opengl').resolve()):  # drive-path: ok owner-protected install, never an output default
        raise ValueError('writes inside A:/Games/mc2-opengl are forbidden')  # drive-path: ok identifies the protected install

def fast_hash(data):
    h, mask = 14695981039346656037, (1 << 64) - 1
    end = len(data) // 8 * 8
    for (w,) in struct.iter_unpack('<Q', data[:end]):
        h = ((h ^ w) * 0x9E3779B97F4A7C15) & mask
        h ^= h >> 29
    for b in data[end:]:
        h = ((h ^ b) * 1099511628211) & mask
    return f'{h:016x}'

def decode(entry):
    off, comp, real, name, is_lz, data = entry
    if real > 1 << 30 or off > len(data) or comp > len(data) - off:
        raise ValueError(f'invalid FST entry: {name}')
    raw = data[off:off + comp]
    # Equal sizes are ambiguous: stock badpilots.csv is a valid equal-size zlib stream.
    if comp == real:
        if len(raw) > 2 and raw[0] == 0x78 and (raw[0] * 256 + raw[1]) % 31 == 0:
            try:
                result = zlib.decompress(raw)
                if len(result) == real:
                    return result
            except zlib.error:
                pass
        return raw
    result = decompress_entry(off, comp, real, is_lz, data)
    if len(result) != real:
        raise ValueError(f'FST decoded size mismatch: {name}')
    return result

def archive_paths(install):
    text = (install / 'system.cfg').read_text(encoding='latin-1')
    section = re.search(r'(?ims)^\[FastFiles\](.*?)(?=^\[|\Z)', text)
    if not section:
        raise ValueError('system.cfg has no [FastFiles] section')
    names = re.findall(r'(?im)^\s*st\s+File(\d+)\s*=\s*"([^"]+)"', section[1])
    if not names:
        raise ValueError('system.cfg has no registered archives')
    result = []
    for _, name in sorted(names, key=lambda item: int(item[0])):
        candidates = [install / name, install / 'data/assets' / name]
        path = next((p for p in candidates if p.is_file()), None)
        if path is None:
            raise ValueError(f'missing source archive: {name}')
        result.append(path)
    return result

def source(path, entries):
    with path.open('rb') as f:
        header = f.read(8 + len(entries) * 266)
    return dict(name=key(path.name), size=path.stat().st_size,
                toc_hash=fast_hash(header), entries=len(entries))

def info(name, method, source_name=''):
    i = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
    i.compress_type = method
    i.compress_level = 19 if method == zipfile.ZIP_ZSTANDARD else None
    i.create_system = 3
    i.external_attr = 0o100644 << 16
    i.comment = source_name.encode('utf-8')
    return i

def pack(install, out):
    writable(out)
    paths = archive_paths(install)
    manifest = {'format': 1, 'sources': [source(p, fst_entries(p)) for p in paths]}
    out.parent.mkdir(parents=True, exist_ok=True)
    temp = out.with_name(out.name + '.tmp')
    seen = {'mc2pak.json'}
    try:
        with zipfile.ZipFile(temp, 'w', allowZip64=True) as z:
            z.comment = COMMENT
            z.writestr(info('mc2pak.json', 0), json.dumps(manifest, separators=(',', ':')).encode())
            for path in paths:
                for entry in fst_entries(path):
                    name = key(entry[3])
                    safe_path(Path('.'), name)
                    if name in seen:
                        continue
                    seen.add(name)
                    raw = decode(entry)
                    compressed = zstd.compress(raw, level=19)
                    method = 0 if name.endswith('.ktx2') or len(compressed) > len(raw) * .95 else 93
                    z.writestr(info(name, method, key(path.name)), raw)
        os.replace(temp, out)
    finally:
        temp.unlink(missing_ok=True)
    return f'pack entries={len(seen)-1} bytes={out.stat().st_size} sha256={hashlib.sha256(out.read_bytes()).hexdigest()}'

def verify(path, install=None):
    with zipfile.ZipFile(path) as z:
        try:
            bad = z.testzip()
        except (zstd.ZstdError, zlib.error, EOFError):
            for item in z.infolist():
                try:
                    z.read(item)
                except (zstd.ZstdError, zlib.error, EOFError, zipfile.BadZipFile) as e:
                    raise ValueError(f'corrupt entry: {item.filename}: {e}') from e
            raise
        if bad:
            raise ValueError(f'corrupt entry: {bad}')
        items = z.infolist()
        if not items or items[0].filename != 'mc2pak.json' or items[0].compress_type != 0:
            raise ValueError('missing first stored manifest')
        manifest = json.loads(z.read(items[0]))
        if manifest.get('format') != 1 or not isinstance(manifest.get('sources'), list):
            raise ValueError('invalid manifest')
        sources = {s['name']: s for s in manifest['sources']}
        names = set()
        for item in items:
            name = item.filename
            safe_path(Path('.'), name)
            if name != key(name) or name in names or item.flag_bits & 9:
                raise ValueError(f'invalid entry: {name}')
            names.add(name)
            if name != 'mc2pak.json' and item.comment.decode('utf-8') not in sources:
                raise ValueError(f'unknown source: {name}')
        if install:
            seen = {'mc2pak.json'}
            for p in archive_paths(install):
                entries = fst_entries(p)
                if sources.get(key(p.name)) != source(p, entries):
                    raise ValueError(f'source changed: {p.name}')
                for e in entries:
                    name = key(e[3])
                    if name in seen:
                        continue
                    seen.add(name)
                    if hashlib.sha256(z.read(name)).digest() != hashlib.sha256(decode(e)).digest():
                        raise ValueError(f'identity mismatch: {name}')
            if seen != names:
                raise ValueError('entry set differs from source winners')
    return f'verify entries={len(names)-1} OK'

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('pack'); p.add_argument('--install', type=Path, required=True); p.add_argument('--out', type=Path)
    for command in ('list', 'extract', 'verify'):
        p = sub.add_parser(command); p.add_argument('pak', type=Path)
        if command == 'extract':
            p.add_argument('outdir', type=Path); p.add_argument('--match', default='*')
        if command == 'verify':
            p.add_argument('--against-install', type=Path)
    args = parser.parse_args(argv)
    start = time.monotonic()
    try:
        if args.command == 'pack':
            result = pack(args.install, args.out or args.install / 'paks/stock.mc2pak')
        elif args.command == 'verify':
            result = verify(args.pak, args.against_install)
        else:
            with zipfile.ZipFile(args.pak) as z:
                if args.command == 'list':
                    for i in z.infolist():
                        print(f'{i.filename}\t{i.file_size}\tmethod={i.compress_type}\t{i.comment.decode("utf-8")}')
                    result = f'list entries={len(z.infolist())}'
                else:
                    writable(args.outdir)
                    count = 0
                    for i in z.infolist():
                        if fnmatch.fnmatchcase(i.filename, args.match):
                            target = safe_path(args.outdir, i.filename)
                            target.parent.mkdir(parents=True, exist_ok=True)
                            target.write_bytes(z.read(i)); count += 1
                    result = f'extract entries={count}'
        print(f'{result} seconds={time.monotonic()-start:.3f}')
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, zipfile.BadZipFile, zlib.error,
            zstd.ZstdError, EOFError, struct.error) as e:
        print(f'mc2pak: {e}', file=sys.stderr)
        return 1

if __name__ == '__main__':
    sys.exit(main())
