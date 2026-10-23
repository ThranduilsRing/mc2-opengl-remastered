"""Shared offline codecs and their stock comparison with real native readers."""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

from scripts import console_child

from tools.mission_codec import (decode_move, encode_move, encode_objects,
                                 fit_sections, move_fields, object_records, packet, parent_cell)
from tools.mc2data.stock_missions import resolve_missions_dir
from tools.pak_append import PacketRecord, make_zlib_payload, write_pak


def test_parent_sentinels_and_packed_cell():
    assert parent_cell(0) is None
    assert parent_cell(-1) is None
    assert parent_cell(0xFFFFFFFF) is None
    assert parent_cell(0x8123ABCD) == [0x8123, 0xABCD]


ROOT = Path(__file__).resolve().parents[2]
READER_CANDIDATES = ('build64-ninja/out/editor/editor_objective_reader_tests.exe',
                     'build64/out/editor/RelWithDebInfo/editor_objective_reader_tests.exe')
DEFAULT_INSTALL = 'A:/Games/mc2-opengl/mc2-win64-v0.4'


def _required():
    return os.environ.get('MC2_CODEC_REQUIRED') == '1'


def _skip_or_fail(msg):
    """MC2_CODEC_REQUIRED=1 (the mission_codec_golden contract row) turns every would-be skip into a failure."""
    pytest.fail(msg) if _required() else pytest.skip(msg)


def _reader():
    """The built native reader: $MC2_CODEC_READER, else a known build output; the contract row builds it when missing."""
    found = os.environ.get('MC2_CODEC_READER') or next((str(ROOT / c) for c in READER_CANDIDATES if (ROOT / c).exists()), None)
    if not found and _required():
        subprocess.run([sys.executable, str(ROOT / 'scripts/ninja_build.py'), 'editor_objective_reader_tests'], check=True,
                       **console_child.child_kwargs())
        found = next((str(ROOT / c) for c in READER_CANDIDATES if (ROOT / c).exists()), None)
    if not found:
        _skip_or_fail('editor_objective_reader_tests.exe not built (py -3 scripts/ninja_build.py editor_objective_reader_tests)')
    return found


def test_stock_native_roundtrip(tmp_path):
    reader = _reader()
    install = os.environ.get('MC2_CODEC_INSTALL') or (DEFAULT_INSTALL if Path(DEFAULT_INSTALL).is_dir() else '')
    if not install:
        _skip_or_fail('no install dir for the native reader (set MC2_CODEC_INSTALL)')
    names = ('mc2_01', 'mc2_03', 'mc2_17', 'mc2_24')
    stock = resolve_missions_dir(os.environ.get('MC2_CODEC_STOCK'), missions=names, allow_fst_extract=False).dir
    for name in names:
        pak = (stock / f'{name}.pak').read_bytes()
        fit = (stock / f'{name}.fit').read_bytes()
        local = [packet(pak, i) for i in range(8)]
        objects = object_records(local[1])
        h, w, planet, cells = decode_move(local, 4)
        rebuilt = [local[0], encode_objects(objects), *local[2:4], *encode_move(cells, planet)]
        assert rebuilt == local
        expected = {'objects': objects, 'parents': [parent_cell(r[7]) for r in objects],
                    'move': {'dimensions': [h, w, planet],
                             **{k: v.ravel().tolist() for k, v in move_fields(cells).items()}},
                    'fit': fit_sections(fit)}
        (tmp_path / f'{name}.pak').write_bytes(pak)
        (tmp_path / f'{name}.fit').write_bytes(fit)
        (tmp_path / f'{name}.json').write_text(json.dumps(expected, separators=(',', ':')), encoding='utf8')
        write_pak([PacketRecord(i, 4, make_zlib_payload(p)) for i, p in enumerate(rebuilt)],
                  tmp_path / f'{name}_roundtrip.pak')
    (tmp_path / 'codec_case.txt').write_text('\n'.join([str(tmp_path), install, '']), encoding='utf8')
    result = subprocess.run([reader, '--test-case=mission-codec/*', '--no-breaks'],
                            cwd=tmp_path, capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    (tmp_path / 'native.log').write_text(result.stdout + result.stderr, encoding='utf8')
    assert result.returncode == 0, (result.stdout + result.stderr)[-12000:]
    # The C++ case returns early (and doctest counts a pass) when it does not see codec_case.txt: require real assertions.
    assert re.search(r'assertions:\s+[1-9]\d*\s+\|', result.stdout), 'native codec case ran no assertions: ' + result.stdout[-4000:]
