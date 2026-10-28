"""Submit, wait for, or fetch a farm job using the existing SSH connection."""
from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import console_child


def remote(args, script):
    encoded = base64.b64encode(script.encode('utf-16le')).decode('ascii')
    return subprocess.check_output(['ssh', '-i', str(args.key), '-o', 'BatchMode=yes',
                                    args.host, 'powershell -NoProfile -EncodedCommand ' + encoded],
                                   text=True, **console_child.child_kwargs(redirected=True))


def remote_python(args, code):
    encoded = base64.b64encode(code.encode()).decode('ascii')
    return remote(args, "$ErrorActionPreference='Stop'; & "
                  "C:/Users/Kaitlyn/AppData/Local/Python/bin/python.exe -c "
                  f'"import base64;exec(base64.b64decode(\'{encoded}\'))"; '
                  'if ($LASTEXITCODE) { throw "remote Python failed" }')


def seed_files(install, harness):
    """Runtime payload plus interpreted harness; never build trees or run outputs."""
    manifest = install / '.deployed_manifest.csv'
    rows = list(csv.DictReader(manifest.read_text(encoding='utf-8-sig').splitlines()[1:]))
    exe = next(r for r in rows if r['relpath'] == 'mc2.exe')
    if hashlib.sha256((install / 'mc2.exe').read_bytes()).hexdigest() != exe['sha256']:
        raise ValueError('install mc2.exe does not match deployed manifest')
    files = {}
    for row in rows:
        rel = Path(row['relpath'])
        if rel.is_absolute() or '..' in rel.parts:
            raise ValueError('unsafe manifest path')
        if rel.suffix.lower() == '.pdb' or (rel.suffix.lower() == '.exe' and rel.name != 'mc2.exe'):
            continue
        if rel.parts[0] in ('tools', 'scripts', 'importer', '3rdparty'):
            continue
        files['install/' + rel.as_posix()] = install / rel
    # Stock game archives/configs are not all owned by the deploy manifest.
    for pattern in ('*.fst', '*.dll', '*.cfg'):
        for path in install.glob(pattern):
            files['install/' + path.name] = path
    files['install/.deployed_manifest.csv'] = manifest
    files['reference/mc2.exe'] = install / 'mc2.exe'
    tracked = subprocess.check_output(['git', '-C', str(harness), 'ls-files',
                                       'scripts', 'tools', 'tests/smoke', 'tests/telemetry/tag-registry.json',
                                       'Gates', 'system.cfg'], text=True)
    for name in tracked.splitlines():
        path = Path(name)
        if ('tests' in path.parts and not name.startswith(('tests/smoke/', 'tests/telemetry/'))):
            continue
        if (name.startswith('scripts/') or name.startswith('tests/smoke/')
                or name.startswith(('Gates/', 'tests/telemetry/')) or name == 'system.cfg'
                or (name.startswith('tools/') and (len(path.parts) == 2
                    or path.parts[1] in ('repo_intel', 'mc2x_import', 'mod_install', 'farm')))):
            if path.suffix in ('.py', '.json', '.yaml', '.yml', '.csv', '.txt', '.h', '.cfg'):
                files['harness/' + name] = harness / path
    # The destination is Windows: manifest/glob casing can differ (sdl2.dll).
    unique = {name.casefold(): (name, path) for name, path in files.items()}
    return dict(unique.values()), exe


def changed_files(files, inventory):
    inventory = {name.casefold(): value for name, value in inventory.items()}
    return {name: path for name, path in files.items()
            if name.casefold() not in inventory
            or inventory[name.casefold()][0] != path.stat().st_size
            or abs(inventory[name.casefold()][1] - path.stat().st_mtime) > .001}


def reseed(args):
    files, exe = seed_files(args.install.resolve(), args.harness.resolve())
    inventory = json.loads(remote_python(args, """
import json
from pathlib import Path
root = Path('C:/mc2farm')
assert not list((root / 'jobs').glob('*.running')), 'cannot reseed during a farm job'
print(json.dumps({p.relative_to(root).as_posix(): [p.stat().st_size, p.stat().st_mtime]
    for folder in ('install', 'harness', 'reference')
    for p in (root / folder).rglob('*') if p.is_file()}))
"""))
    changed = changed_files(files, inventory)
    size = sum(p.stat().st_size for p in changed.values())
    with tempfile.TemporaryDirectory(prefix='farm-reseed-') as temp:
        archive = Path(temp) / 'seed.tar'
        with tarfile.open(archive, 'w') as tar:
            for name, path in changed.items():
                tar.add(path, arcname=name, recursive=False)
            metadata = json.dumps({'src_commit': exe['src_commit'], 'sha256': exe['sha256']}).encode()
            info = tarfile.TarInfo('reference/seed.json')
            info.size = len(metadata)
            tar.addfile(info, io.BytesIO(metadata))
        transferred = archive.stat().st_size
        subprocess.run(['scp', '-q', '-i', str(args.key), '-o', 'BatchMode=yes', str(archive),
                        f'{args.host}:C:/mc2farm/seed.upload.tar'], check=True,
                       **console_child.child_kwargs())
        print(remote_python(args, """
import hashlib, json, tarfile
from pathlib import Path
root = Path('C:/mc2farm')
with tarfile.open(root / 'seed.upload.tar') as tar:
    tar.extractall(root, filter='data')
meta = json.loads((root / 'reference/seed.json').read_text())
for name in ('install/mc2.exe', 'reference/mc2.exe'):
    assert hashlib.sha256((root / name).read_bytes()).hexdigest() == meta['sha256'], name
(root / 'seed.upload.tar').unlink()
print(json.dumps(meta))
""" ).strip())
    print(json.dumps({'changed_files': len(changed), 'payload_bytes': size,
                      'transferred_bytes': transferred, 'GB': transferred / 1e9}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='Kaitlyn@192.168.68.107')
    parser.add_argument('--key', type=Path,
                        default=Path(os.environ.get('HOME', str(Path.home()))) / '.ssh/mc2farm_ed25519')
    sub = parser.add_subparsers(dest='action', required=True)
    seed = sub.add_parser('reseed')
    seed.add_argument('--install', type=Path, required=True)
    seed.add_argument('--harness', type=Path, required=True)
    submit = sub.add_parser('submit')
    submit.add_argument('job', type=Path)
    wait = sub.add_parser('wait')
    wait.add_argument('id')
    wait.add_argument('--timeout', type=float, default=900)
    fetch = sub.add_parser('fetch')
    fetch.add_argument('id')
    fetch.add_argument('destination', type=Path)
    args = parser.parse_args()
    if args.action == 'reseed':
        reseed(args)
        return
    if args.action == 'submit':
        json.loads(args.job.read_text(encoding='utf-8'))
        job_id = uuid.uuid4().hex
        subprocess.run(['scp', '-i', str(args.key), '-o', 'BatchMode=yes', str(args.job),
                        f'{args.host}:C:/mc2farm/jobs/{job_id}.upload'], check=True,
                       **console_child.child_kwargs())
        remote(args, f"Move-Item -LiteralPath C:/mc2farm/jobs/{job_id}.upload "
                     f"-Destination C:/mc2farm/jobs/{job_id}.json -ErrorAction Stop")
        print(job_id)
        return
    if not re.fullmatch('[0-9a-f]{32}', args.id):
        parser.error('id must be the 32-hex identifier returned by submit')
    if args.action == 'fetch':
        args.destination.mkdir(parents=True, exist_ok=True)
        subprocess.run(['scp', '-r', '-i', str(args.key), '-o', 'BatchMode=yes',
                        f'{args.host}:C:/mc2farm/results/{args.id}', str(args.destination)], check=True,
                       **console_child.child_kwargs())
        return
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        text = remote(args, f"if (Test-Path C:/mc2farm/results/{args.id}/result.json) "
                            f"{{ Get-Content -Raw C:/mc2farm/results/{args.id}/result.json }}")
        if text.strip():
            result = json.loads(text)
            print(json.dumps(result, indent=2))
            raise SystemExit(0 if result['status'] == 'completed' and result['returncode'] == 0 else 1)
        time.sleep(3)
    raise SystemExit('wait timed out; job remains owned by the desktop runner')


if __name__ == '__main__':
    main()
