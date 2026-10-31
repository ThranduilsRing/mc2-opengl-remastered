"""Farm queue tests use child Python processes, never the engine or SSH."""
import json
import os
from pathlib import Path
import sys
import subprocess
import shutil
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts import console_child
from tools.farm.runner import atomic_json, poll_once
from tools.farm import host


def test_reseed_selection_and_incremental_changes(tmp_path):
    install, harness = tmp_path / 'install', tmp_path / 'harness'
    install.mkdir()
    harness.mkdir()
    (install / 'mc2.exe').write_bytes(b'verified')
    import hashlib
    sha = hashlib.sha256(b'verified').hexdigest()
    (install / '.deployed_manifest.csv').write_text(
        'manifest_version,v1\nrelpath,sha256,bytes,src_commit\n'
        f'mc2.exe,{sha},8,abc\nmc2.pdb,unused,10,abc\n')
    (install / 'mission.fst').write_bytes(b'stock')
    (install / 'data').mkdir()
    (install / 'data/stock.fit').write_text('loose stock data')
    (install / 'data/session.log').write_text('skip')
    subprocess.run(['git', 'init', '-q', str(harness)], check=True)
    (harness / 'scripts').mkdir()
    (harness / 'scripts/run_smoke.py').write_text('pass')
    (harness / 'scripts/untracked.log').write_text('skip')
    subprocess.run(['git', '-C', str(harness), 'add', 'scripts/run_smoke.py'], check=True)
    files, exe = host.seed_files(install, harness)
    assert exe['src_commit'] == 'abc'
    assert set(files) == {'install/mc2.exe', 'install/mission.fst',
                          'install/.deployed_manifest.csv', 'install/data/stock.fit', 'reference/mc2.exe',
                          'harness/scripts/run_smoke.py'}
    inventory = {name: [p.stat().st_size, p.stat().st_mtime] for name, p in files.items()}
    assert host.changed_files(files, inventory) == {}
    assert host.changed_files(files, {k.upper(): v for k, v in inventory.items()}) == {}
    inventory['install/mc2.exe'][0] += 1
    inventory['install/mission.fst'][1] -= 10
    assert set(host.changed_files(files, inventory)) == {'install/mc2.exe', 'install/mission.fst'}
    (install / 'mc2.exe').write_bytes(b'corrupt')
    with pytest.raises(ValueError, match='manifest'):
        host.seed_files(install, harness)


def job(tmp_path, value):
    (tmp_path / 'jobs').mkdir()
    atomic_json(tmp_path / 'jobs' / 'sample.json', value)
    poll_once(tmp_path)
    return json.loads((tmp_path / 'results/sample/result.json').read_text())


def test_output_environment_and_exit(tmp_path):
    result = job(tmp_path, {'argv': [sys.executable, '-c',
        'import os; print(os.environ["TEST_FARM"]); print(os.environ["MC2_FARM_RESULT_DIR"]); print(os.environ["MC2_SMOKE_ARTIFACT_ROOT"]); exit(7)'],
        'env': {'TEST_FARM': 'visible'}, 'cwd': str(tmp_path)})
    assert result['status'] == 'completed' and result['returncode'] == 7
    log = (tmp_path / 'results/sample/job.log').read_text()
    assert 'visible' in log and str(tmp_path / 'results/sample') in log
    assert str(tmp_path / 'results/sample/smoke') in log
    assert (tmp_path / 'jobs/sample.done').exists()
    poll_once(tmp_path)  # Completed jobs are not executed twice.
    assert json.loads((tmp_path / 'results/sample/result.json').read_text()) == result


def test_timeout_reaps_child(tmp_path):
    result = job(tmp_path, {'argv': [sys.executable, '-c', 'import time; time.sleep(30)'], 'timeout': .1})
    assert result['status'] == 'error' and 'TimeoutExpired' in result['error']


def test_malformed_job_does_not_block_queue(tmp_path):
    result = job(tmp_path, {'argv': 'must not execute a shell string'})
    assert result['status'] == 'error' and 'ValueError' in result['error']


def test_partial_upload_is_ignored(tmp_path):
    (tmp_path / 'jobs').mkdir()
    (tmp_path / 'jobs/partial.upload').write_text('{')
    poll_once(tmp_path)
    assert not (tmp_path / 'results').exists()


def test_bad_job_does_not_prevent_next_job(tmp_path):
    (tmp_path / 'jobs').mkdir()
    (tmp_path / 'jobs/0.json').write_text('{')
    atomic_json(tmp_path / 'jobs/1.json', {'argv': [sys.executable, '-c', 'print("next")']})
    poll_once(tmp_path)
    assert json.loads((tmp_path / 'results/0/result.json').read_text())['status'] == 'error'
    assert json.loads((tmp_path / 'results/1/result.json').read_text())['returncode'] == 0


def test_host_submit_publishes_only_after_upload(tmp_path, monkeypatch, capsys):
    path = tmp_path / 'job.json'
    path.write_text('{"argv": ["python", "--version"]}')
    calls = []
    monkeypatch.setattr(sys, 'argv', ['host', 'submit', str(path)])
    monkeypatch.setattr(host.uuid, 'uuid4', lambda: SimpleNamespace(hex='a' * 32))
    monkeypatch.setattr(host.subprocess, 'run', lambda argv, **kw: calls.append(argv))
    monkeypatch.setattr(host, 'remote', lambda args, script: calls.append(script))
    host.main()
    assert calls[0][-1].endswith('/' + 'a' * 32 + '.upload')
    assert '.upload -Destination' in calls[1] and '.json' in calls[1]
    assert capsys.readouterr().out.strip() == 'a' * 32


def test_host_submit_exe_stages_then_rewrites_job(tmp_path, monkeypatch):
    path = tmp_path / 'job.json'
    path.write_text('{"argv": ["run", "{EXE}", "--out", "{RESULT}/proof"], "env": {"X": "{ROOT}/d"}}')
    exe = tmp_path / 'mc2.exe'
    exe.write_bytes(b'MZ')
    delta = tmp_path / 'delta'
    delta.mkdir()
    calls, uploaded = [], []
    monkeypatch.setattr(sys, 'argv', ['host', 'submit', str(path), '--exe', str(exe), '--data-delta', str(delta)])
    monkeypatch.setattr(host.uuid, 'uuid4', lambda: SimpleNamespace(hex='b' * 32))
    monkeypatch.setattr(host, 'remote', lambda args, script: calls.append(('ps', script)))
    monkeypatch.setattr(host, 'payload_archive', lambda exe, dst: dst.write_bytes(b'zip'))

    def fake_scp(args, *paths, recursive=False):
        calls.append(('scp', str(paths[-1])))
        if str(paths[-1]).endswith('.upload'):
            uploaded.append(json.loads(Path(paths[0]).read_text()))
    monkeypatch.setattr(host, 'scp', fake_scp)
    host.main()
    work = 'C:/mc2farm/work/' + 'b' * 32
    result = 'C:/mc2farm/results/' + 'b' * 32
    assert uploaded == [{'argv': ['run', work + '/mc2.exe', '--out', result + '/proof'], 'env': {'X': work + '/d'}}]
    kinds = [c[0] for c in calls]
    assert kinds == ['ps', 'scp', 'scp', 'scp', 'ps', 'scp', 'ps']
    assert calls[3][1].endswith('/payload.zip')
    assert 'Expand-Archive' in calls[4][1]


def test_exe_payload_matches_deploy_and_isolated_from_seed(tmp_path, monkeypatch):
    import deploy_payload as deploy
    import zipfile

    source = tmp_path / 'lane'
    (source / 'scripts').mkdir(parents=True)
    (source / 'scripts/deploy_payload.py').touch()
    build = source / 'build64-ninja'
    build.mkdir()
    exe = build / 'mc2.exe'
    exe.write_bytes(b'lane exe')
    entries = [('shaders/include/new.hglsl', 'shader'),
               ('shaders/vulkan/new.frag.spv', 'shader'),
               ('data/defs/ui/PACKAGE.fit', 'defs'), ('avcodec.dll', 'dll'),
               ('3rdparty/smaa/LICENSE', 'support'), ('mc2.pdb', 'pdb')]
    items = []
    for rel, kind in entries:
        path = build / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(rel.encode())
        items.append((str(path), rel, kind))
    monkeypatch.setattr(deploy, 'enumerate_payload', lambda *a, **kw: items)
    monkeypatch.setattr(deploy, 'git_head', lambda *a: 'a' * 40)
    monkeypatch.setattr(deploy, 'require_head_exe', lambda *a: {'sha': 'a' * 40, 'dirty': False})
    archive = tmp_path / 'payload.zip'
    host.payload_archive(exe, archive)
    staged = tmp_path / 'staged'
    staged.mkdir()
    with zipfile.ZipFile(archive) as payload:
        assert 'mc2.pdb' not in payload.namelist()
        for rel, kind in entries:
            if kind != 'pdb':
                assert payload.read(rel) == rel.encode()
        payload.extractall(staged)
    shutil.copy2(exe, staged / 'mc2.exe')
    report = deploy.staleness_report(str(staged))
    assert report['has_manifest'] and not report['stale'] and not report['missing']
    (staged / entries[0][0]).write_bytes(b'seed shader')
    assert deploy.staleness_report(str(staged))['stale'] == [entries[0][0]]
    # Payload is expanded before choosing junctions: touched trees are copied.
    assert host.STAGE_PS.index('Expand-Archive') < host.STAGE_PS.index('foreach')
    assert 'Test-Path "$s/delta/$($e.Name)"' in host.STAGE_PS
    if os.name == 'nt':
        root = tmp_path / 'farm'
        install = root / 'install'
        (install / 'shaders/include').mkdir(parents=True)
        (install / 'data').mkdir()
        (install / 'data/seed.txt').write_text('keep shared data')
        seed = install / entries[0][0]
        seed.write_bytes(b'seed shader')
        stage = root / 'stage/sample'
        stage.mkdir(parents=True)
        shutil.copy2(exe, stage / 'mc2.exe')
        shutil.copy2(archive, stage / 'payload.zip')
        subprocess.run(['powershell', '-NoProfile', '-Command',
                        host.STAGE_PS % {'r': root.as_posix(), 'id': 'sample'}],
                       check=True, **console_child.child_kwargs())
        work = root / 'work/sample'
        report = deploy.staleness_report(str(work))
        assert not report['stale'] and not report['missing']
        (work / entries[0][0]).write_bytes(b'job shader')
        assert seed.read_bytes() == b'seed shader'
        (root / 'results/sample').mkdir(parents=True)
        (root / 'results/sample/result.json').write_text('{}')
        subprocess.run(['powershell', '-NoProfile', '-Command',
                        host.CLEAN_PS % {'r': root.as_posix(), 'id': 'sample'}],
                       check=True, **console_child.child_kwargs())
        assert not work.exists()
        assert (install / 'data/seed.txt').read_text() == 'keep shared data'




def test_host_data_delta_requires_exe(tmp_path, monkeypatch):
    path = tmp_path / 'job.json'
    path.write_text('{"argv": ["x"]}')
    monkeypatch.setattr(sys, 'argv', ['host', 'submit', str(path), '--data-delta', str(tmp_path)])
    with pytest.raises(SystemExit) as result:
        host.main()
    assert result.value.code == 2


def test_payload_rejects_unattributable_exe_before_packaging(tmp_path, monkeypatch):
    import deploy_payload as deploy
    (tmp_path / 'scripts').mkdir()
    (tmp_path / 'scripts/deploy_payload.py').touch()
    exe = tmp_path / 'build/mc2.exe'
    exe.parent.mkdir()
    exe.write_bytes(b'wrong revision')
    monkeypatch.setattr(deploy, 'git_head', lambda *a: 'a' * 40)
    def refuse(*args):
        raise ValueError('embedded sha does not match HEAD')
    monkeypatch.setattr(deploy, 'require_head_exe', refuse)
    archive = tmp_path / 'payload.zip'
    with pytest.raises(ValueError, match='does not match HEAD'):
        host.payload_archive(exe, archive)
    assert not archive.exists()


def test_perf_job_stages_both_payloads(tmp_path, monkeypatch):
    path = Path(__file__).resolve().parents[2] / 'tools/farm/perf.json'
    a, b = tmp_path / 'a.exe', tmp_path / 'b.exe'
    a.write_bytes(b'a')
    b.write_bytes(b'b')
    monkeypatch.setattr(sys, 'argv', ['host', 'submit', str(path), '--exe', str(a), '--exe-b', str(b)])
    monkeypatch.setattr(host.uuid, 'uuid4', lambda: SimpleNamespace(hex='a' * 32))
    archives, calls, published = [], [], []
    monkeypatch.setattr(host, 'payload_archive', lambda exe, dst: (archives.append(exe), dst.write_bytes(b'zip')))
    monkeypatch.setattr(host, 'remote', lambda args, script: calls.append(script))
    def upload(args, *paths, **kwargs):
        if str(paths[-1]).endswith('.upload'):
            published.append(json.loads(Path(paths[0]).read_text()))
    monkeypatch.setattr(host, 'scp', upload)
    host.main()
    assert archives == [a, b]
    job = published[0]
    assert '{ROOT' not in json.dumps(job)
    assert f"C:/mc2farm/work/{'a' * 32}-b" in job['argv']
    assert 'paired_cpu.py' in job['argv'][1]
    assert calls[-1].endswith('.json -ErrorAction Stop')
    assert sum('Expand-Archive' in script for script in calls) == 2


@pytest.mark.parametrize('status,code,expected', [('completed', 0, 0), ('completed', 9, 1), ('interrupted', None, 1)])
def test_host_wait_propagates_result(monkeypatch, status, code, expected):
    monkeypatch.setattr(sys, 'argv', ['host', 'wait', 'a' * 32])
    monkeypatch.setattr(host, 'remote', lambda *args: json.dumps({'status': status, 'returncode': code}))
    with pytest.raises(SystemExit) as result:
        host.main()
    assert result.value.code == expected


def test_host_rejects_remote_path_injection(monkeypatch):
    monkeypatch.setattr(sys, 'argv', ['host', 'wait', '../other'])
    with pytest.raises(SystemExit) as result:
        host.main()
    assert result.value.code == 2


def test_farm_deploy_lease_stays_on_farm_disk(tmp_path):
    registry = tmp_path / 'leases.json'
    script = ('from scripts.smoke_lib import deploy_lease as d; '
              'assert str(d.LEASE_REGISTRY_PATH) == __import__("os").environ["MC2_DEPLOY_LEASE_PATH"]; '
              'assert d.LEASE_REGISTRY_PATH.with_suffix(".lock") == d._lock_path(); '
              'd._write_registry({})')
    subprocess.run([sys.executable, '-c', script],
                   cwd=Path(__file__).resolve().parents[2],
                   env=dict(os.environ, MC2_DEPLOY_LEASE_PATH=str(registry)), check=True,
                   **console_child.child_kwargs())
    assert json.loads(registry.read_text()) == {}


@pytest.mark.skipif(os.name != 'nt', reason='Windows scheduler priority')
def test_runner_lowers_own_priority(tmp_path):
    (tmp_path / 'stop').touch()
    script = ('import ctypes, sys; from tools.farm.runner import main; '
              'sys.argv = ["runner", "--root", sys.argv[1]]; main(); '
              'k = ctypes.WinDLL("kernel32"); '
              'k.GetCurrentProcess.restype = ctypes.c_void_p; '
              'k.GetPriorityClass.argtypes = [ctypes.c_void_p]; '
              'assert k.GetPriorityClass(k.GetCurrentProcess()) == 0x4000')
    subprocess.run([sys.executable, '-c', script, str(tmp_path)],
                   cwd=Path(__file__).resolve().parents[2], check=True,
                   **console_child.child_kwargs())


def test_deployed_runner_replaces_interrupted_and_requeued_jobs(tmp_path):
    # Exercise the actual standalone entry point and seeded helper, outside repo depth.
    from tools.farm import runner
    deployed = tmp_path / 'runner.py'
    shutil.copy2(runner.__file__, deployed)
    helper = tmp_path / 'harness/scripts'
    helper.mkdir(parents=True)
    shutil.copy2(console_child.__file__, helper / 'console_child.py')
    jobs = tmp_path / 'jobs'
    jobs.mkdir()
    (jobs / 'old.running').write_text('{}')
    (jobs / 'old.done').write_text('stale')
    (jobs / 'sample.done').write_text('stale')
    atomic_json(jobs / 'sample.json', {'argv': [sys.executable, '-c',
        'from pathlib import Path; Path("stop").touch(); print("deployed job ran")'], 'cwd': str(tmp_path)})
    proc = subprocess.Popen([sys.executable, str(deployed), '--root', str(tmp_path)],
                            **console_child.child_kwargs(redirected=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
    try:
        stdout, stderr = proc.communicate(timeout=15)
        assert proc.returncode == 0, (stdout, stderr)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate(timeout=10)
    assert json.loads((tmp_path / 'results/old/result.json').read_text())['status'] == 'interrupted'
    assert json.loads((tmp_path / 'results/sample/result.json').read_text())['returncode'] == 0
    assert 'deployed job ran' in (tmp_path / 'results/sample/job.log').read_text()
