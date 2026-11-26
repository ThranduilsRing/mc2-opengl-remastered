"""Magic FixABL 1.45 compatibility and deployment isolation."""
import hashlib
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "scripts"))
from deploy_payload import magic_brain_145


@pytest.mark.parametrize('before,after', [
    ('coreAttack(pilotEventParams[0], TACORDER_PARAM_PURSUE);', 'magicAttack(pilotEventParams[0]);'),
    ('coreAttack(0, 0);', 'magicAttack(0);'),
    ('coreAttackTactic(0, 0, TACTIC_REAR, tacticState);', 'magicAttack(0);'),
    ('coreGuard(startposition, -1, AttackStateHandle);', 'coreGuard(startposition, -1);'),
    ('corePatrol(PatrolState, PatrolPath, AttackStateHandle);', 'corePatrol(PatrolState, PatrolPath);'),
    ('coreEscort(moverList[0], 40.0, 135.0, -1);', 'coreEscort(moverList[0]);'),
    ('COREATTACK(getTarget(-1), 0);', 'magicAttack(getTarget(-1));'),
    ('corePatrolDelayed(s, p, getTime, attack);', 'corePatrolDelayed(s, p, getTime);'),
    ('coreWait(1.0, -1, attack);', 'coreWait(1.0, -1);'),
])
def test_fixabl(before, after):
    result = magic_brain_145(before.encode())
    assert result == after.encode()
    assert magic_brain_145(result) == result


def test_unrelated_bytes_and_comments_unchanged():
    source = b'// coreAttack(0, 0);\r\n/* coreGuard(x, -1, y); */\r\nprint("coreAttack(");\r\n'
    assert magic_brain_145(source) == source


def test_original_library_is_only_in_named_profile():
    library = ROOT / 'mods/magic-brain-1.45/data/missions/corebrain.abx'
    source = library.read_bytes().replace(b'\t\tchar[64] magicTrace;\r\n', b'')
    trace = (b'\t\tmagicTrace = "[MAGIC_ATTACK] tactic=";\r\n'
             b'\t\tconcat(magicTrace, param_tactic);\r\n'
             b'\t\ttdebugstring(magicTrace);\r\n\r\n')
    assert hashlib.md5(source.replace(trace, b'')).hexdigest() == '64acd5a129ca67bd3a4862bee420e14d'
    assert not (ROOT / 'data/missions/corebrain.abx').exists()


def test_payload_rewrites_stock_archive_only(tmp_path, monkeypatch):
    sys.path.insert(0, str(ROOT / 'scripts'))
    import deploy_payload
    import tools.mc2x_import.fst as fst
    stock = b'coreAttack(0, 0);\r\n'
    monkeypatch.setattr(fst, 'fst_entries', lambda _: [
        (0, len(stock), len(stock), 'data/missions/warriors/enemy.abl', False, stock),
        (0, len(stock), len(stock), 'data/missions/mc2_01.abl', False, stock)])
    items = deploy_payload.magic_brain_payload(tmp_path, tmp_path)
    assert len(items) == 1
    path, destination, _ = items[0]
    assert destination == 'mods/magic-brain-1.45/data/missions/warriors/enemy.abl'
    assert Path(path).read_bytes() == b'magicAttack(0);\r\n'
    assert not (tmp_path / 'data/missions/warriors/enemy.abl').exists()
