"""Serial desktop-session runner. Jobs are atomically published JSON argv lists."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

runner_dir = Path(__file__).resolve().parent
sys.path.insert(0, str(runner_dir / 'harness/scripts'))
sys.path.append(str(runner_dir))  # queue_lock.py sits next to runner.py (also when imported as tools.farm.runner)
if len(runner_dir.parents) > 1:
    sys.path.append(str(runner_dir.parents[1] / 'scripts'))
import console_child
import queue_lock


def atomic_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2), encoding='utf-8')
    temporary.replace(path)


def run_job(path, root):
    output = root / 'results' / path.stem
    output.mkdir(parents=True, exist_ok=True)
    result = {'started': time.time(), 'status': 'error', 'returncode': None}
    try:
        job = json.loads(path.read_text(encoding='utf-8'))
        argv = job['argv']
        if not isinstance(argv, list) or not argv or not all(isinstance(x, str) for x in argv):
            raise ValueError('argv must be a nonempty list of strings')
        timeout = float(job.get('timeout', 600))
        if not 0 < timeout <= 7200:
            raise ValueError('timeout must be in (0, 7200] seconds')
        env = dict(os.environ, **job.get('env', {}), MC2_FARM_RESULT_DIR=str(output))
        env.setdefault('MC2_SMOKE_ARTIFACT_ROOT', str(output / 'smoke'))
        env.setdefault('MC2_HARNESS_GPU', 'any')
        with (output / 'job.log').open('wb') as log:
            child = subprocess.Popen(argv, cwd=job.get('cwd', str(root)), env=env,
                                     stdout=log, stderr=subprocess.STDOUT,
                                     **console_child.child_kwargs(redirected=True))
            result['pid'] = child.pid
            try:
                result['returncode'] = child.wait(timeout=timeout)
                result['status'] = 'completed'
            finally:
                if child.poll() is None:
                    # Only this job's PID and descendants; never kill by image name.
                    if os.name == 'nt':
                        subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'],
                                       stdout=log, stderr=subprocess.STDOUT, check=False,
                                       **console_child.child_kwargs(redirected=True))
                    else:
                        child.kill()
                    child.wait()
    except Exception as exc:
        result['error'] = f'{type(exc).__name__}: {exc}'
    result['finished'] = time.time()
    atomic_json(output / 'result.json', result)
    path.replace(path.with_suffix('.done'))
    return result


def poll_once(root):
    for path in sorted((root / 'jobs').glob('*.json')):
        try:
            # One queue for jobs and the laptop gate's build (p31): take it BEFORE claiming, so a job waiting behind a gate build stays
            # cancellable, and give up after a few seconds so the heartbeat keeps beating.
            with queue_lock.hold(root / 'queue.lock', timeout=5):
                claimed = path.with_suffix('.running')
                try:
                    path.replace(claimed)
                except FileNotFoundError:
                    continue  # desktop cancelled this still-pending job after our directory snapshot
                run_job(claimed, root)
        except TimeoutError:
            return


def main():
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        kernel32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        if not kernel32.SetPriorityClass(kernel32.GetCurrentProcess(),
                                        subprocess.BELOW_NORMAL_PRIORITY_CLASS):
            raise ctypes.WinError(ctypes.get_last_error())
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path('C:/mc2farm'))
    args = parser.parse_args()
    root = args.root.resolve()
    for name in ('jobs', 'results'):
        (root / name).mkdir(parents=True, exist_ok=True)
    # Scheduled task also forbids overlapping instances; lock protects manual launches.
    with (root / 'runner.lock').open('a+b') as lock:
        lock.seek(0)
        if os.name == 'nt':
            import msvcrt
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        # Never replay a job interrupted by reboot: it may have side effects.
        for path in (root / 'jobs').glob('*.running'):
            output = root / 'results' / path.stem
            output.mkdir(parents=True, exist_ok=True)
            atomic_json(output / 'result.json', {'status': 'interrupted', 'returncode': None})
            path.replace(path.with_suffix('.done'))
        while not (root / 'stop').exists():
            atomic_json(root / 'heartbeat.json', {'time': time.time()})  # host.py alive: file age = worker liveness
            poll_once(root)
            time.sleep(1)


if __name__ == '__main__':
    main()
