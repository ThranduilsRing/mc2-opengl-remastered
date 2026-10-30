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
    script = "$ProgressPreference='SilentlyContinue'\n" + script  # else CLIXML progress noise on stdout
    encoded = base64.b64encode(script.encode('utf-16le')).decode('ascii')
    return subprocess.check_output(['ssh', '-i', str(args.key), '-o', 'BatchMode=yes',
                                    args.host, 'powershell -NoProfile -EncodedCommand ' + encoded],
                                   text=True, **console_child.child_kwargs(redirected=True))


ROOT = 'C:/mc2farm'
# Per-job tree: junction big dirs, hardlink big files, copy small files (the game rewrites
# cfg files in place, and a dir touched by the data delta must not write through to the
# seeded install). {EXE}/{ROOT} in the job JSON are replaced by the remote paths.
STAGE_PS = r"""
$ErrorActionPreference='Stop'
$i='%(r)s/install'; $w='%(r)s/work/%(id)s'; $s='%(r)s/stage/%(id)s'
Expand-Archive -LiteralPath "$s/payload.zip" -DestinationPath "$s/delta" -Force
New-Item -ItemType Directory -Force $w | Out-Null
foreach ($e in Get-ChildItem $i -Force) {
  if ($e.Name -in 'mc2.exe','mc2.pdb') { continue }
  $t = Join-Path $w $e.Name
  if ($e.PSIsContainer) {
    if (Test-Path "$s/delta/$($e.Name)") { Copy-Item $e.FullName $t -Recurse }
    else { cmd /c mklink /J "$t" "$($e.FullName)" | Out-Null }
  } elseif ($e.Length -lt 10MB) { Copy-Item $e.FullName $t }
  else { cmd /c mklink /H "$t" "$($e.FullName)" | Out-Null }
}
if (Test-Path "$s/delta") {
  foreach ($f in Get-ChildItem "$s/delta" -File) { Remove-Item (Join-Path $w $f.Name) -Force -ErrorAction SilentlyContinue }
  Copy-Item "$s/delta/*" $w -Recurse -Force
}
Move-Item "$s/mc2.exe" "$w/mc2.exe" -Force
Remove-Item $s -Recurse -Force
"""
CLEAN_PS = r"""
$ErrorActionPreference='SilentlyContinue'
if (-not (Test-Path '%(r)s/results/%(id)s/result.json')) { exit 0 }  # job not done: leave its tree alone
$w='%(r)s/work/%(id)s'
if (Test-Path $w) {
  foreach ($e in Get-ChildItem $w -Force) {
    if ($e.PSIsContainer -and ($e.Attributes -band [IO.FileAttributes]::ReparsePoint)) { cmd /c rmdir "$($e.FullName)" | Out-Null }
  }
  Remove-Item $w -Recurse -Force
}
Remove-Item '%(r)s/stage/%(id)s' -Recurse -Force -ErrorAction SilentlyContinue
exit 0
"""


def scp(args, *paths, recursive=False):
    cmd = ['scp'] + (['-r'] if recursive else []) + ['-i', str(args.key), '-o', 'BatchMode=yes', *map(str, paths)]
    subprocess.run(cmd, check=True, **console_child.child_kwargs())


def substitute(value, subs):
    if isinstance(value, str):
        for k, v in subs.items():
            value = value.replace(k, v)
        return value
    if isinstance(value, list):
        return [substitute(x, subs) for x in value]
    if isinstance(value, dict):
        return {k: substitute(v, subs) for k, v in value.items()}
    return value
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
    # The deploy manifest owns the engine payload, not all stock loose data.
    for folder in ('data', 'assets', 'shaders', 'shaders_vk'):
        for path in (install / folder).rglob('*'):
            rel = path.relative_to(install)
            if (path.is_file() and not any(part.lower() in
                    ('savegame', 'cache', '__pycache__', 'crashes') for part in rel.parts)
                    and path.suffix.lower() not in ('.log', '.tmp', '.bak', '.pdb', '.dmp', '.ims')):
                files['install/' + rel.as_posix()] = path
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


def payload_archive(exe, destination):
    """Use deploy's payload list, including baked shaders, without cooking/building."""
    import deploy_payload as deploy
    import zipfile

    source = next((p for p in exe.resolve().parents
                   if (p / 'scripts/deploy_payload.py').is_file()), None)
    if source is None:
        raise ValueError('--exe must be in its source worktree/build tree')
    from contextlib import redirect_stdout
    with redirect_stdout(sys.stderr):
        items = deploy.enumerate_payload(str(source), str(exe.parent), exe.name,
                                         'mc2.pdb', require_build=False)
    hashes = {'mc2.exe': (deploy.sha256_file(str(exe)), exe.stat().st_size)}
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as archive:
        for src, rel, kind in items:
            if kind in ('exe', 'pdb'):
                continue
            path = Path(src)
            if not path.is_file():
                if kind in ('shader', 'defs', 'dll'):
                    raise ValueError(f'missing exe payload: {path}')
                continue
            archive.write(path, rel)
            hashes[rel] = (deploy.sha256_file(str(path)), path.stat().st_size)
        deploy.write_manifest(str(destination.parent), hashes, deploy.git_head(str(source)))
        archive.write(destination.parent / deploy.MANIFEST_NAME, deploy.MANIFEST_NAME)


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
    submit.add_argument('--exe', type=Path, help="run the lane's own exe: job JSON may use {EXE} and {ROOT}")
    submit.add_argument('--data-delta', type=Path, help='small dir overlaid on the seeded install (needs --exe)')
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
        job = json.loads(args.job.read_text(encoding='utf-8'))
        job_id = uuid.uuid4().hex
        upload = args.job
        if args.data_delta and not args.exe:
            parser.error('--data-delta needs --exe')
        if args.exe:
            if not args.exe.is_file():
                parser.error(f'--exe not found: {args.exe}')
            if args.data_delta and not args.data_delta.is_dir():
                parser.error(f'--data-delta is not a directory: {args.data_delta}')
            tmpdir = tempfile.TemporaryDirectory()
            payload = Path(tmpdir.name) / 'payload.zip'
            try:
                payload_archive(args.exe.resolve(), payload)
            except ValueError as exc:
                parser.error(str(exc))
            remote(args, f"New-Item -ItemType Directory -Force {ROOT}/stage/{job_id} | Out-Null")
            scp(args, args.exe, f'{args.host}:{ROOT}/stage/{job_id}/mc2.exe')
            if args.data_delta:
                scp(args, args.data_delta, f'{args.host}:{ROOT}/stage/{job_id}/delta', recursive=True)
            scp(args, payload, f'{args.host}:{ROOT}/stage/{job_id}/payload.zip')
            remote(args, STAGE_PS % {'r': ROOT, 'id': job_id})
            work = f'{ROOT}/work/{job_id}'
            job = substitute(job, {'{EXE}': f'{work}/mc2.exe', '{ROOT}': work,
                                   '{RESULT}': f'{ROOT}/results/{job_id}'})
            upload = Path(tmpdir.name) / 'job.json'
            upload.write_text(json.dumps(job, indent=2), encoding='utf-8')
        scp(args, upload, f'{args.host}:{ROOT}/jobs/{job_id}.upload')
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
        remote(args, CLEAN_PS % {'r': ROOT, 'id': args.id})  # per-job exe tree, if any
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
