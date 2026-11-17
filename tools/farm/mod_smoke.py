"""Private laptop mod mission driver; the desktop runner owns the queue lock."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def command(args, output):
    """Use existing smoke gates and engine save/load fixtures without freezing animation."""
    env = dict(os.environ, MC2_ACTIVE_MOD=args.mod, MC2_SMOKE_FREEZE_ANIM='0',
               MC2_SAVE_DIR=str(args.install / 'savegame'), MC2_DEBUG_STATE_DUMP='1',
               MC2_VISUAL_CAPTURE_FRAME=str(args.capture_frame),
               MC2_VISUAL_CAPTURE_DIR=str(output / 'screenshots'))
    steps = dict(step.split(':', 1) for step in args.steps.split(',') if step)
    if set(steps) - {'save', 'load'}:
        raise ValueError('steps must be save:TICK,load:TICK')
    for step, tick in steps.items():
        if not tick.isdecimal() or int(tick) <= 0:
            raise ValueError('step ticks must be positive integers')
        env['MC2_BOOT_QUICKSAVE_AT' if step == 'save' else 'MC2_BOOT_QUICKLOAD_AT'] = tick
    if 'load' in steps and ('save' not in steps or int(steps['load']) <= int(steps['save'])):
        raise ValueError('load must follow save')
    argv = [sys.executable, str(args.harness / 'scripts/run_smoke.py'), '--exe',
            str(args.install / 'mc2.exe'), '--ref-exe', str(args.install / 'mc2.exe'),
            '--mission', args.mission, '--allow-any-mission', '--duration', str(args.seconds),
            '--keep-logs', '--grace-s', '300']
    return argv, env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--install', type=Path, required=True)
    parser.add_argument('--harness', type=Path, required=True)
    parser.add_argument('--mod', default='')
    parser.add_argument('--mission', default='mc2_01')
    parser.add_argument('--seconds', type=int, choices=range(1, 31), default=30)
    parser.add_argument('--steps', default='', help='optional save:TICK,load:TICK')
    parser.add_argument('--capture-frame', type=int, default=600)
    args = parser.parse_args()
    sys.path.insert(0, str(args.harness / 'scripts'))
    import console_child
    output = Path(os.environ['MC2_FARM_RESULT_DIR'])
    for folder in (args.install / 'savegame', output / 'screenshots'):
        folder.mkdir(parents=True, exist_ok=True)
    try:
        argv, env = command(args, output)
    except ValueError as exc:
        parser.error(str(exc))
    (output / 'mod-job.json').write_text(json.dumps({'argv': argv, 'env': {
        k: v for k, v in env.items() if k.startswith('MC2_')}}, indent=2))
    try:
        # The harness keeps its crash/heartbeat/fingerprint gates and owns the game PID.
        return subprocess.run(argv, cwd=args.harness, env=env, check=False,
                              **console_child.child_kwargs()).returncode
    finally:
        for folder in ('crashes', 'debug_state', 'savegame'):
            source = args.install / folder
            if source.exists():
                shutil.copytree(source, output / folder, dirs_exist_ok=True)


if __name__ == '__main__':
    raise SystemExit(main())
