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

import importlib.util

def _load_fst():
    # Load fst.py under a private module name: no sys.path edit and no 'mc2x_import' sys.modules entry, so the
    # tools/mc2x_import package cannot shadow or be shadowed by the mc2x_import.py script module of the same name.
    spec = importlib.util.spec_from_file_location('_mc2pak_fst', Path(__file__).resolve().parent / 'mc2x_import' / 'fst.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

_fst = _load_fst()
decompress_entry = _fst.decompress_entry

def fst_entries(path):
    """The FST's entries, or ValueError when the file is not a whole FST. A file preallocated by an unfinished copy (robocopy
    reserves the full size before writing) reads as magic 0 / count 0; the packer used to accept that and ship a pak without
    that archive's entries (73 MB instead of 214 MB, 2026-11-22). Every pak source must have a valid magic, entries, and every
    entry's payload inside the file."""
    entries = _fst.fst_entries(path)
    with open(path, 'rb') as f:
        magic = f.read(4)
    size = Path(path).stat().st_size
    if magic not in (b'\xaf\xec\xdd\xfa', b'\xaf\xec\xdd\xca') or not entries:
        raise ValueError(f'not a valid FST (bad magic or no entries; unfinished copy?): {path}')
    if 8 + len(entries) * 266 > size or any(e[0] < 8 + len(entries) * 266 or e[0] + e[1] > size or not e[3] for e in entries):
        raise ValueError(f'truncated or inconsistent FST table: {path}')
    return entries

COMMENT = b'MechCommander 2 asset pack (ZIP container, Zstandard entries). List/extract/pack with tools/mc2pak.py or the asset viewer.'

ALIASES = 'mc2pak.aliases'  # Store entry, "alias<TAB>target<TAB>source" lines: names whose payload equals another entry's
RESERVED = ('mc2pak.json', ALIASES)

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
    by_hash, aliases = {}, []
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
                    digest = hashlib.sha256(raw).digest()
                    if digest in by_hash:  # identical payload: stored once, this name goes in the alias table
                        aliases.append(f'{name}\t{by_hash[digest]}\t{key(path.name)}\n')
                        continue
                    by_hash[digest] = name
                    # zipfile compresses at level 19 inside writestr (the one real compression); a level-1 probe only picks stored vs zstd.
                    method = 0 if name.endswith('.ktx2') or len(zstd.compress(raw, level=1)) > len(raw) * .95 else 93
                    z.writestr(info(name, method, key(path.name)), raw)
            if aliases:
                z.writestr(info(ALIASES, 0), ''.join(aliases).encode())
        os.replace(temp, out)
    finally:
        temp.unlink(missing_ok=True)
    return f'pack entries={len(seen)-1} aliases={len(aliases)} bytes={out.stat().st_size} sha256={hashlib.sha256(out.read_bytes()).hexdigest()}'

def read_aliases(z):
    """alias name -> (target name, source); empty when the pak has no alias table."""
    try:
        text = z.read(ALIASES).decode('utf-8')
    except KeyError:
        return {}
    table = {}
    for line in text.split('\n')[:-1] if text.endswith('\n') else text.split('\n'):  # the engine splits on \n only: CRLF, empty lines and NUL are rejected here too
        parts = line.split('\t')
        if len(parts) != 3 or not all(parts) or '\r' in line or '\0' in line:
            raise ValueError(f'invalid alias line: {line!r}')
        name, target, src = parts
        try:
            safe_path(Path('.'), name)
        except ValueError:
            raise ValueError(f'invalid alias: {name}') from None
        if name in table or name in RESERVED or name != key(name) or name.startswith('/') or '..' in name.split('/'):
            raise ValueError(f'invalid alias: {name}')
        table[name] = (target, src)
    return table

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
            if name not in RESERVED and item.comment.decode('utf-8') not in sources:
                raise ValueError(f'unknown source: {name}')
        aliases = read_aliases(z)
        for name, (target, src) in aliases.items():
            safe_path(Path('.'), name)
            if name != key(name) or name in names or target not in names or target in RESERVED or src not in sources:
                raise ValueError(f'invalid alias: {name}')
        names.discard(ALIASES)
        names.update(aliases)
        def read_name(name):
            return z.read(aliases[name][0] if name in aliases else name)
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
                    if hashlib.sha256(read_name(name)).digest() != hashlib.sha256(decode(e)).digest():
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
                aliases = read_aliases(z)
                items = [i for i in z.infolist() if i.filename not in RESERVED]
                if args.command == 'list':
                    for i in items:
                        print(f'{i.filename}\t{i.file_size}\tmethod={i.compress_type}\t{i.comment.decode("utf-8")}')
                    for name, (target, src) in aliases.items():
                        print(f'{name}\t{z.getinfo(target).file_size}\tmethod=alias\t{src}\t-> {target}')
                    result = f'list entries={len(items) + len(aliases)} aliases={len(aliases)}'
                else:
                    writable(args.outdir)
                    count = 0
                    jobs = [(i.filename, i) for i in items] + [(n, z.getinfo(t)) for n, (t, _) in aliases.items()]
                    for name, i in jobs:
                        if fnmatch.fnmatchcase(name, args.match):
                            target = safe_path(args.outdir, name)
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
