"""Submit, wait for, or fetch a farm job using the existing SSH connection."""
from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import re
import subprocess
import sys
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='Kaitlyn@192.168.68.107')
    parser.add_argument('--key', type=Path,
                        default=Path(os.environ.get('HOME', str(Path.home()))) / '.ssh/mc2farm_ed25519')
    sub = parser.add_subparsers(dest='action', required=True)
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
            remote(args, f"New-Item -ItemType Directory -Force {ROOT}/stage/{job_id} | Out-Null")
            scp(args, args.exe, f'{args.host}:{ROOT}/stage/{job_id}/mc2.exe')
            if args.data_delta:
                scp(args, args.data_delta, f'{args.host}:{ROOT}/stage/{job_id}/delta', recursive=True)
            remote(args, STAGE_PS % {'r': ROOT, 'id': job_id})
            work = f'{ROOT}/work/{job_id}'
            job = substitute(job, {'{EXE}': f'{work}/mc2.exe', '{ROOT}': work})
            tmpdir = tempfile.TemporaryDirectory()
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
