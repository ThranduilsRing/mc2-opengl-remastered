"""Shared offline codecs and their stock comparison with real native readers."""
import json
import os
import subprocess

import pytest

from tools.mission_codec import (decode_move, encode_move, encode_objects,
                                 fit_sections, move_fields, object_records, packet, parent_cell)
from tools.mc2data.stock_missions import resolve_missions_dir
from tools.pak_append import PacketRecord, make_zlib_payload, write_pak


def test_parent_sentinels_and_packed_cell():
    assert parent_cell(0) is None
    assert parent_cell(-1) is None
    assert parent_cell(0xFFFFFFFF) is None
    assert parent_cell(0x8123ABCD) == [0x8123, 0xABCD]


@pytest.mark.skipif(not os.environ.get('MC2_CODEC_READER'), reason='set MC2_CODEC_READER for native stock proof')
def test_stock_native_roundtrip(tmp_path):
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
    env = dict(os.environ, MC2_CODEC_GOLDEN=str(tmp_path))
    result = subprocess.run([os.environ['MC2_CODEC_READER'], '--test-case=mission-codec/*', '--no-breaks'],
                            env=env, capture_output=True, text=True)
    (tmp_path / 'native.log').write_text(result.stdout + result.stderr, encoding='utf8')
    assert result.returncode == 0, (result.stdout + result.stderr)[-12000:]
