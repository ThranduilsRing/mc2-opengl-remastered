#!/usr/bin/env python3
"""Terrain GPU evidence, without treating CPU zones or window means as frame tails.

An evidence JSON contains identity (commit, exe_sha256, shaders_sha256, config,
gpu, driver, resolution, settings, content_sha256, mission, camera), sampling
('frame' or 'window_mean'), and scopes {GPU zone: [milliseconds, ...]}.
Use --log with --identity to ingest VK_GPU_PASSES receipts. Missing passes stay
unmeasured; the terrain aggregate includes main and skirts, never water/overlays.
Different builds are refused unless --candidate-commit names the intended B build.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics

IDENTITY = ('commit', 'exe_sha256', 'shaders_sha256', 'config', 'gpu', 'driver',
            'resolution', 'settings', 'content_sha256', 'mission', 'camera')
BUILD = ('commit', 'exe_sha256', 'shaders_sha256')
TERRAIN = 'VKIsl.Terrain'
SCOPE_NOTE = ('VKIsl.Terrain includes main patches and skirts. Water, overlays and '
              'shadow casting are separate. Subdraw costs are unmeasured.')


def validate(evidence):
    identity = evidence.get('identity', {})
    for key in IDENTITY:
        if key not in identity or identity[key] in (None, '', {}, []):
            raise ValueError(f'missing capture identity: {key}')
    if identity['config'] != 'RelWithDebInfo':
        raise ValueError('config must be RelWithDebInfo')
    if not re.fullmatch(r'[0-9a-f]{40}', identity['commit']):
        raise ValueError('commit must be a full clean git SHA')
    for key in ('exe_sha256', 'shaders_sha256', 'content_sha256'):
        if not re.fullmatch(r'[0-9a-f]{64}', identity[key]):
            raise ValueError(f'invalid {key}')
    size = identity['resolution']
    if not isinstance(size, list) or len(size) != 2 or any(type(x) is not int or x <= 0 for x in size):
        raise ValueError('resolution must be [width, height]')
    if evidence.get('sampling') not in ('frame', 'window_mean'):
        raise ValueError('sampling must declare frame or window_mean')
    scopes = evidence.get('scopes', {})
    if not scopes.get(TERRAIN):
        raise ValueError('missing terrain GPU timings')
    for name, samples in scopes.items():
        if not name.startswith('VKIsl.') or not isinstance(samples, list) or not samples:
            raise ValueError(f'invalid GPU scope: {name}')
        if any(type(x) not in (int, float) or not math.isfinite(x) or x < 0 for x in samples):
            raise ValueError(f'invalid GPU timings: {name}')
    return evidence


def from_log(log, identity, after_frame):
    scopes = {}
    windows = []
    names = None
    for line in log.splitlines():
        if '[VK_GPU_PASSES v1] frame=' not in line:
            continue
        header, sep, tail = line.partition('|')
        fields = dict(re.findall(r'(\w+)=([\d.]+)', header))
        if int(fields['frame']) <= after_frame:
            continue
        if not sep or int(fields['dropped']) or int(fields['overflow']):
            raise ValueError('incomplete GPU timestamp window')
        count = int(fields['n'])
        if count <= 0:
            raise ValueError('empty GPU timestamp window')
        row = dict(re.findall(r'(VKIsl\.[\w.]+)=([\d.]+)\(x[\d.]+\)', tail))
        if names is not None and set(row) != names:
            raise ValueError('GPU scope set changed during capture')
        names = set(row)
        windows.append({'end_frame': int(fields['frame']), 'frames': count})
        for name, value in row.items():
            scopes.setdefault(name, []).append(float(value))
    # Unequal window lengths would bias a plain mean. Do not silently weight tails.
    if len({w['frames'] for w in windows}) > 1:
        raise ValueError('unequal GPU timestamp window lengths')
    return validate({'identity': identity, 'sampling': 'window_mean', 'scopes': scopes,
                     'windows': windows, 'source_log_sha256': hashlib.sha256(log.encode()).hexdigest()})


def summarize(evidence):
    validate(evidence)
    result = {'identity': evidence['identity'], 'sampling': evidence['sampling'],
              'scope_note': SCOPE_NOTE, 'scopes': {}}
    for name, values in sorted(evidence['scopes'].items()):
        row = {'count': len(values), 'mean_ms': statistics.mean(values)}
        if evidence['sampling'] == 'frame':
            ordered = sorted(values)
            for q in (50, 95, 99):
                row[f'p{q}_ms'] = ordered[math.ceil(q / 100 * len(ordered)) - 1]
        result['scopes'][name] = row
    return result


def compare(before, after, candidate_commit=None):
    a, b = summarize(before), summarize(after)
    if candidate_commit is not None and b['identity']['commit'] != candidate_commit:
        raise ValueError('candidate build does not match the declared commit')
    for key in IDENTITY:
        if a['identity'][key] != b['identity'][key] and not (candidate_commit and key in BUILD):
            raise ValueError(f'capture mismatch: {key}')
    if a['sampling'] != b['sampling'] or a['scopes'].keys() != b['scopes'].keys():
        raise ValueError('sampling or measured pass scopes differ')
    delta = {name: {'before_ms': row['mean_ms'], 'after_ms': b['scopes'][name]['mean_ms'],
                    'delta_ms': b['scopes'][name]['mean_ms'] - row['mean_ms']}
             for name, row in a['scopes'].items()}
    return {'before': a, 'after': b, 'delta': delta}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', nargs='?', type=Path, help='evidence JSON')
    parser.add_argument('after', nargs='?', type=Path, help='optional candidate evidence JSON')
    parser.add_argument('--candidate-commit', help='explicitly permit this B build in an A/B')
    parser.add_argument('--log', type=Path, help='import GPU timestamp windows')
    parser.add_argument('--identity', type=Path, help='JSON capture identity for --log')
    parser.add_argument('--after-frame', type=int, default=600, help='discard warmup windows ending at/before this frame')
    parser.add_argument('-o', '--output', type=Path)
    args = parser.parse_args(argv)
    try:
        read = lambda p: json.loads(p.read_text(encoding='utf-8'))
        if args.log:
            if not args.identity or args.before or args.after:
                raise ValueError('--log requires --identity and no positional evidence')
            result = from_log(args.log.read_text(encoding='utf-8'), read(args.identity), args.after_frame)
        else:
            if not args.before or args.identity:
                raise ValueError('supply evidence JSON or --log with --identity')
            before = read(args.before)
            result = compare(before, read(args.after), args.candidate_commit) if args.after else summarize(before)
        output = json.dumps(result, indent=2, allow_nan=False) + '\n'
        if args.output:
            args.output.write_text(output, encoding='utf-8')
        else:
            print(output, end='')
    except (ValueError, KeyError, TypeError, OSError) as exc:
        parser.exit(2, f'REFUSED: {exc}\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
