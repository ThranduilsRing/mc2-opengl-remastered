"""Stock narrow MOVE packets (move.h, MOVE_saveData), rebuilt from a cell mask.

Normal areas are four-connected components within 30-cell sectors. Doors are
contiguous shared sector edges; links use actual within-area shortest paths.
No stock object's path graph is inherited. Special/destructible areas are not
supported yet: obstacles remain blocked after destruction.
"""
from collections import deque
import heapq
import struct

import numpy as np

AREA = struct.Struct('<hhIihi b??I')
DOOR = struct.Struct('<hhb?bhhhhbbhhII7i')
INFO = struct.Struct('<hb')
LINK = struct.Struct('<hbii')
DIRS = ((-1, 0), (0, 1), (1, 0), (0, -1))


def scalar(n):
    return struct.pack('<i', n)


def distances(area_map, start, area):
    """Stock calcLinkCost movement: 10 straight, 15 diagonal, one flank open."""
    found = {start: 0}
    queue = [(0, start)]
    h, w = area_map.shape
    while queue:
        cost, (r, c) = heapq.heappop(queue)
        if cost != found[r, c]:
            continue
        for dr, dc in (*DIRS, (-1, -1), (-1, 1), (1, -1), (1, 1)):
            p = r+dr, c+dc
            if not (0 <= p[0] < h and 0 <= p[1] < w and area_map[p] == area):
                continue
            diagonal = dr != 0 and dc != 0
            # MoveMap::calcPath rejects diagonals only when BOTH flanks block.
            if diagonal and area_map[r+dr, c] != area and area_map[r, c+dc] != area:
                continue
            candidate = cost + (15 if diagonal else 10)
            if candidate < found.get(p, float('inf')):
                found[p] = candidate
                heapq.heappush(queue, (candidate, p))
    return found


def global_packets(passable):
    h, w = passable.shape
    if h != w or h % 30:
        raise ValueError('MOVE requires square dimensions divisible by 30')
    amap = np.full((h, w), -1, dtype='<i2')
    sectors = []
    for sr in range(h//30):
        for sc in range(w//30):
            for r in range(sr*30, (sr+1)*30):
                for c in range(sc*30, (sc+1)*30):
                    if not passable[r, c] or amap[r, c] != -1:
                        continue
                    area = len(sectors)
                    if area >= 32767:
                        raise ValueError('too many MOVE areas')
                    sectors.append((sr, sc))
                    amap[r, c] = area
                    queue = deque([(r, c)])
                    while queue:
                        rr, cc = queue.popleft()
                        for dr, dc in DIRS:
                            nr, nc = rr+dr, cc+dc
                            if (sr*30 <= nr < (sr+1)*30 and sc*30 <= nc < (sc+1)*30
                                    and passable[nr, nc] and amap[nr, nc] == -1):
                                amap[nr, nc] = area
                                queue.append((nr, nc))
    doors = []
    # Same east/south orientation and midpoint convention as calcGlobalDoors.
    for direction in (1, 2):
        for boundary in range(29, w-1, 30):
            k = 0
            while k < h:
                r, c = (k, boundary) if direction == 1 else (boundary, k)
                dr, dc = DIRS[direction]
                pair = int(amap[r, c]), int(amap[r+dr, c+dc])
                if min(pair) < 0:
                    k += 1
                    continue
                start = k
                while k < h and k//30 == start//30:
                    rr, cc = (k, boundary) if direction == 1 else (boundary, k)
                    if (int(amap[rr, cc]), int(amap[rr+dr, cc+dc])) != pair:
                        break
                    k += 1
                doors.append((r, c, k-start, direction, pair))
    infos = [[] for _ in sectors]
    for d, (_, _, _, _, pair) in enumerate(doors):
        for side, a in enumerate(pair):
            infos[a].append((d, side))
    max_doors = max(map(len, infos), default=0)
    if max_doors > 30 or len(doors) >= 32765:
        raise ValueError('MOVE door capacity exceeded')

    def midpoint(d, side):
        r, c, length, direction, _ = doors[d]
        return ((r+length//2, c+side) if direction == 1
                else (r+side, c+length//2))

    door_bytes, link_packets = [], []
    for d, (r, c, length, direction, pair) in enumerate(doors):
        counts = [len(infos[a])-1 for a in pair]
        door_bytes.append(DOOR.pack(r, c, length, True, -1, *pair, 1, 1,
                                    direction, (direction+2)%4, *counts, 0, 0,
                                    1, 0, 0, 0, 0, 0, 0))
        for side, a in enumerate(pair):
            dist = distances(amap, midpoint(d, side), a)
            links = []
            for other, oside in infos[a]:
                if other != d:
                    cost = max(1, dist[midpoint(other, oside)])
                    links.append(LINK.pack(other, oside, cost, cost))
            link_packets.append(b''.join(links)+bytes(LINK.size*2))
    # Two scratch doors, with enough room for start/goal links and two extras.
    for _ in range(2):
        door_bytes.append(DOOR.pack(-1, -1, -1, True, -1, -1, -1, 1, 1,
                                    -1, -1, max_doors, 0, 0, 0,
                                    1, 0, 0, 0, 0, 0, 0))
        link_packets.extend((bytes(LINK.size*(max_doors+2)), bytes(LINK.size*2)))
    num_links = sum(len(p)//LINK.size for p in link_packets)
    headers = (0x022523, h, w, 30, h//30, w//30, len(sectors), len(doors),
               sum(map(len, infos)), num_links)
    areas = b''.join(AREA.pack(sr, sc, 0, 0, len(infos[a]), 0, -1, False, True, 0)
                     for a, (sr, sc) in enumerate(sectors))
    packets = [scalar(v) for v in headers]+[amap.tobytes(), areas]
    packets += [b''.join(INFO.pack(*v) for v in run) for run in infos if run]
    packets += [b''.join(door_bytes)]+link_packets
    return packets


def build_move(cells):
    passable = (cells & 0x400) != 0
    h, w = cells.shape
    normal = global_packets(passable)
    flying = global_packets(np.ones_like(passable))
    return ([scalar(h), scalar(w), scalar(0), cells.astype('<u4').tobytes()]
            + normal + normal + flying + [scalar(0), scalar(0)])


def validate_move(packets, side):
    """Read the emitted schema independently, including cursor/count relations."""
    def integer(i):
        if len(packets[i]) != 4:
            raise ValueError(f'MOVE scalar packet {i} has wrong size')
        return struct.unpack('<i', packets[i])[0]
    if (integer(0), integer(1)) != (side, side) or len(packets[3]) != side*side*4:
        raise ValueError('MOVE local dimensions mismatch')
    local = np.frombuffer(packets[3], '<u4').reshape(side, side)
    cursor = 4
    summaries = []
    for level in range(3):
        version, h, w, dim, sh, sw, na, nd, ni, nl = [integer(cursor+i) for i in range(10)]
        if (version, h, w, dim, sh, sw) != (0x022523, side, side, 30, side//30, side//30):
            raise ValueError('MOVE global header mismatch')
        amap = np.frombuffer(packets[cursor+10], '<i2').reshape(side, side)
        if np.any((amap < -1) | (amap >= na)):
            raise ValueError('MOVE invalid area index')
        if level < 2 and not np.array_equal(amap >= 0, (local & 0x400) != 0):
            raise ValueError('MOVE passability/area mismatch')
        areas = list(AREA.iter_unpack(packets[cursor+11]))
        if len(areas) != na or sum(a[4] for a in areas) != ni:
            raise ValueError('MOVE area/door counts mismatch')
        cursor += 12
        area_infos = []
        for a in areas:
            run = list(INFO.iter_unpack(packets[cursor])) if a[4] else []
            if a[4]:
                cursor += 1
            if len(run) != a[4]:
                raise ValueError('MOVE door info size mismatch')
            area_infos.append(run)
        doors = list(DOOR.iter_unpack(packets[cursor]))
        cursor += 1
        if len(doors) != nd+2:
            raise ValueError('MOVE door count mismatch')
        for a, run in enumerate(area_infos):
            for d, s in run:
                if not (0 <= d < nd and s in (0, 1)) or doors[d][5+s] != a:
                    raise ValueError('MOVE door/area reference mismatch')
        total = 0
        for d, door in enumerate(doors):
            for s in (0, 1):
                links = list(LINK.iter_unpack(packets[cursor]))
                cursor += 1
                count = door[11+s]
                if len(links) != count+2:
                    raise ValueError('MOVE link capacity mismatch')
                total += len(links)
                if d < nd:
                    for other, os, cost, _ in links[:count]:
                        if not (0 <= other < nd and os in (0, 1) and cost > 0):
                            raise ValueError('MOVE invalid link')
                        if doors[other][5+os] != door[5+s]:
                            raise ValueError('MOVE link leaves area')
        if total != nl:
            raise ValueError('MOVE link pool mismatch')
        summaries.append({'areas': na, 'doors': nd, 'links': nl})
    if [integer(cursor), integer(cursor+1)] != [0, 0] or cursor+2 != len(packets):
        raise ValueError('MOVE special-area tail mismatch')
    return summaries
