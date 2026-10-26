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
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
import console_child


def remote(args, script):
    encoded = base64.b64encode(script.encode('utf-16le')).decode('ascii')
    return subprocess.check_output(['ssh', '-i', str(args.key), '-o', 'BatchMode=yes',
                                    args.host, 'powershell -NoProfile -EncodedCommand ' + encoded],
                                   text=True, **console_child.child_kwargs(redirected=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', default='Kaitlyn@192.168.68.107')
    parser.add_argument('--key', type=Path,
                        default=Path(os.environ.get('HOME', str(Path.home()))) / '.ssh/mc2farm_ed25519')
    sub = parser.add_subparsers(dest='action', required=True)
    submit = sub.add_parser('submit')
    submit.add_argument('job', type=Path)
    wait = sub.add_parser('wait')
    wait.add_argument('id')
    wait.add_argument('--timeout', type=float, default=900)
    fetch = sub.add_parser('fetch')
    fetch.add_argument('id')
    fetch.add_argument('destination', type=Path)
    args = parser.parse_args()
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
