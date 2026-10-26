"""Farm queue tests use child Python processes, never the engine or SSH."""
import json
import os
from pathlib import Path
import sys
import subprocess
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.farm.runner import atomic_json, poll_once
from tools.farm import host


def job(tmp_path, value):
    (tmp_path / 'jobs').mkdir()
    atomic_json(tmp_path / 'jobs' / 'sample.json', value)
    poll_once(tmp_path)
    return json.loads((tmp_path / 'results/sample/result.json').read_text())


def test_output_environment_and_exit(tmp_path):
    result = job(tmp_path, {'argv': [sys.executable, '-c',
        'import os; print(os.environ["TEST_FARM"]); print(os.environ["MC2_FARM_RESULT_DIR"]); exit(7)'],
        'env': {'TEST_FARM': 'visible'}, 'cwd': str(tmp_path)})
    assert result['status'] == 'completed' and result['returncode'] == 7
    log = (tmp_path / 'results/sample/job.log').read_text()
    assert 'visible' in log and str(tmp_path / 'results/sample') in log
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
                   env=dict(os.environ, MC2_DEPLOY_LEASE_PATH=str(registry)), check=True)
    assert json.loads(registry.read_text()) == {}
