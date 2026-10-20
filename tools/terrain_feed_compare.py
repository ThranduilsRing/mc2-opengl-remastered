#!/usr/bin/env python3
"""Compare consumed terrain receipts from MC2_VK_FEED_DUMP, bit for bit.

Use the existing frozen bookmark capture. This checks CPU resource receipts,
staged GPU uniforms, patch order and protected activity; external GPU image
contents still require paired viewed captures. Exit 0 equal, 1 changed, 2 invalid.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re

FIELDS = {'height', 'cement', 'visual_height', 'colormap', 'cement_atlas',
          'shore_mask', 'shore_field', 'light_pool', 'light_header', 'light_indices'}
REQUIRED = {'island', 'schema', 'render_frame', 'activity_frame', 'frozen', 'resolution',
            'mapSide', 'patchCount', 'patches', 'ubo_hex', 'fields', 'versions',
            'imported_versions', 'resource_shapes', 'material_handles',
            'external_image_contents', 'activity'}


def integer(value, minimum=0):
    return type(value) is int and value >= minimum


def hex_bytes(value, size=None):
    return (isinstance(value, str) and len(value) % 2 == 0
            and re.fullmatch('[0-9a-f]*', value) is not None
            and (size is None or len(value) == size * 2))


def validate(record):
    if not isinstance(record, dict) or not REQUIRED <= record.keys():
        raise ValueError('incomplete terrain receipt')
    if type(record['schema']) is not int or record['schema'] != 1 or record['island'] != 'terrain':
        raise ValueError('unsupported terrain receipt schema')
    if record['frozen'] is not True:
        raise ValueError('terrain feed must use a frozen capture')
    if (not integer(record['render_frame']) or not integer(record['activity_frame'])
            or record['activity_frame'] != record['render_frame']):
        raise ValueError('stale activity frame epoch')
    size = record['resolution']
    if not isinstance(size, list) or len(size) != 2 or not all(integer(x, 1) for x in size):
        raise ValueError('invalid resolution')
    if not integer(record['mapSide'], 1) or not integer(record['patchCount'], 1):
        raise ValueError('invalid map or patch count')
    patches = record['patches']
    if not isinstance(patches, list) or len(patches) != record['patchCount']:
        raise ValueError('incomplete patches')
    if any(not isinstance(p, list) or len(p) != 9 or any(type(x) is not int for x in p) for p in patches):
        raise ValueError('invalid patch fields')
    if not hex_bytes(record['ubo_hex'], record['patchCount'] * 1024):
        raise ValueError('incomplete staged uniforms')
    fields = record['fields']
    if not isinstance(fields, dict) or fields.keys() != FIELDS:
        raise ValueError('incomplete field receipts')
    for field in fields.values():
        if (not isinstance(field, dict) or set(field) != {'bytes', 'fnv1a64'}
                or not integer(field['bytes'])
                or (field['fnv1a64'] is not None and
                    (not integer(field['fnv1a64']) or field['fnv1a64'] >= 2**64))):
            raise ValueError('invalid field receipt')
    for key, count in [('versions', 9), ('imported_versions', 8),
                       ('resource_shapes', 11), ('material_handles', 9)]:
        values = record[key]
        if not isinstance(values, list) or len(values) != count or not all(integer(x) for x in values):
            raise ValueError(f'incomplete {key}')
    height = fields['height']
    if height['bytes'] != record['mapSide'] ** 2 * 4 or height['fnv1a64'] is None:
        raise ValueError('missing authoritative height field receipt')
    if record['imported_versions'][0] != record['versions'][0]:
        raise ValueError('stale consumed height version')
    if record['external_image_contents'] != 'not_read_back':
        raise ValueError('external GPU image contents are not measured by this boundary')
    activity = record['activity']
    if not isinstance(activity, dict) or set(activity) != {'blocks', 'vertices', 'solid_enabled', 'solid_count', 'solid_hex'}:
        raise ValueError('missing protected activity')
    if (not hex_bytes(activity['blocks']) or not activity['blocks']
            or not hex_bytes(activity['vertices'], record['mapSide'] ** 2)
            or any(activity[k][i:i+2] not in ('00', '01')
                   for k in ('blocks', 'vertices') for i in range(0, len(activity[k]), 2))
            or type(activity['solid_enabled']) is not bool or not integer(activity['solid_count'])
            or not hex_bytes(activity['solid_hex'], activity['solid_count'] * 4)):
        raise ValueError('invalid protected activity')
    return record


def load(path):
    records = [json.loads(line) for line in Path(path).read_text(encoding='utf-8').splitlines() if line.strip()]
    if any(not isinstance(r, dict) for r in records):
        raise ValueError('feed records must be JSON objects')
    terrain = [r for r in records if r.get('island') == 'terrain']
    if len(terrain) != 1:
        raise ValueError('expected exactly one terrain receipt')
    return validate(terrain[0])


def compare(before, after):
    validate(before)
    validate(after)
    # Preserve ordering and unknown future fields. No epsilon, ignored epochs,
    # sorted patch lists, or activity exceptions can conceal a changed input.
    return [key for key in sorted(before.keys() | after.keys()) if before.get(key) != after.get(key)]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args(argv)
    try:
        changed = compare(load(args.before), load(args.after))
    except (ValueError, TypeError, KeyError, OSError) as exc:
        parser.exit(2, f'REFUSED: {exc}\n')
    print('CHANGED: ' + ', '.join(changed) if changed else 'EQUAL: consumed terrain and protected activity')
    return int(bool(changed))


if __name__ == '__main__':
    raise SystemExit(main())
