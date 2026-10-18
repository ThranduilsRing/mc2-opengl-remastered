"""
tools/mc2x_import/fst.py -- FST/pak read helpers, ported verbatim from the proven
legacy scripts A:/tmp/cveg_min_files.py (FST/pak helpers, appearance parsing) and
A:/tmp/fst_extract.py (lzw_decomp). Two edits only vs the originals:
  (a) lzw_decomp is inlined here (legacy imported it from fst_extract via sys.path);
  (b) module-level hardcoded install-path constants are dropped (paths now passed in).
Everything else is byte-identical proven code.
"""
import struct, os, re, sys, shutil, zlib, argparse


# ---- Classic variable-width LZW decompressor ---------------------------------
# Faithful port of the ORIGINAL Microsoft __asm LZDecomp in mclib/lzdecomp.cpp
# (the non-LINUX_BUILD path — the decoder the shipped Windows engine actually
# runs, and the one whose semantics the stock FST packer targets).
#
# The earlier version of this function ported the LINUX_BUILD C fallback
# (LZDecompClassicLZW_) instead, which deviates from the asm in two ways and
# failed with "invalid code 301" on stock misc.fst entries:
#   1. The asm adds a dictionary entry for EVERY code, including the very
#      first one of the stream (using initial old_chain=0 / old_suffix=0),
#      so free_idx runs one ahead of the C-fallback's — every later
#      dictionary code is off by one.
#   2. After a CLEAR (256), the asm reads the next code, emits ONLY its low
#      byte and records it as old_chain WITHOUT adding a dictionary entry.
#   3. Codes >= free_idx are not an error in the asm: the entry at
#      table[code] is written from (old_chain, old_suffix) and the walk
#      continues from old_chain (generalized KwKwK).
#   4. free_idx keeps incrementing past 4096 (table is 16384/3 = 5461
#      entries); only code-width growth stops at 12 bits.
#
# Bit stream: LSB-first across byte boundaries, 9 -> 12 bit codes,
# clear=256, eof=257, first dictionary code=258.

HASH_CLEAR = 256
HASH_EOF   = 257
HASH_FREE  = 258
MAX_BITS   = 12
BASE_BITS  = 9
_TABLE_LEN = 16384 // 3   # 5461 entries, same as the asm's LZHashBuffer[16384]


def lzw_decomp(src: bytes, dest_max: int) -> bytes:
    """Decompress Microsoft MC2 LZW-packed data (asm-faithful).

    Returns the decompressed bytes (truncated at dest_max). Never raises on
    stream oddities the asm tolerates; stops at EOF code / end of input.
    """
    parent = [0] * _TABLE_LEN   # 16-bit parent code
    suffix = [0] * _TABLE_LEN   # 8-bit terminal byte

    src_len = len(src)
    src_pos = 0
    bit_buf = 0
    bits_in = 0
    dest    = bytearray()

    def read_code(nbits):
        nonlocal src_pos, bit_buf, bits_in
        while bits_in < nbits:
            if src_pos >= src_len:
                return -1
            bit_buf |= src[src_pos] << bits_in
            bits_in += 8
            src_pos += 1
        code = bit_buf & ((1 << nbits) - 1)
        bit_buf >>= nbits
        bits_in -= nbits
        return code

    code_width = BASE_BITS
    max_code   = 1 << code_width   # 512 initially
    free_idx   = HASH_FREE         # 258
    old_chain  = 0                 # asm: LZOldChain starts 0 (NOT "no prev")
    old_suffix = 0                 # asm: LZOldSuffix starts 0

    while True:
        code = read_code(code_width)
        if code < 0 or code == HASH_EOF:
            break

        if code == HASH_CLEAR:
            # asm ClearHash: reset, then consume the next code as a literal —
            # emit its low byte, record it as old_chain, add NO entry.
            code_width = BASE_BITS
            max_code   = 1 << code_width
            free_idx   = HASH_FREE
            code = read_code(code_width)
            if code < 0:
                break
            old_chain  = code
            old_suffix = code & 0xFF
            if len(dest) >= dest_max:
                return bytes(dest)
            dest.append(code & 0xFF)
            continue

        chain = code
        stk = []
        c = code
        if c >= free_idx:
            # Not-in-table (KwKwK generalized): plant old_suffix, write the
            # entry at table[code], continue the walk from old_chain.
            stk.append(old_suffix)
            if c < _TABLE_LEN:
                suffix[c] = old_suffix
                parent[c] = old_chain
            c = old_chain
        depth = 0
        while c > 255:
            stk.append(suffix[c])
            c = parent[c]
            depth += 1
            if depth > _TABLE_LEN:
                # Parent-chain cycle — cannot happen on a well-formed stream;
                # means this data is NOT classic LZW (the asm would run off
                # into garbage here; we fail loudly instead of hanging).
                raise ValueError("LZW: parent-chain cycle (not an LZW stream?)")
        old_suffix = c & 0xFF      # terminal char = first byte of the string
        stk.append(old_suffix)

        # Emit in reverse (stack is LIFO -> actual order)
        while stk:
            if len(dest) >= dest_max:
                return bytes(dest)
            dest.append(stk.pop())

        # Add new dictionary entry — UNCONDITIONALLY (even for the first code
        # of the stream). free_idx keeps counting past 4096 like the asm.
        if free_idx < _TABLE_LEN:
            parent[free_idx] = old_chain
            suffix[free_idx] = old_suffix
        free_idx += 1
        old_chain = chain

        # Grow code width when dictionary index reaches the current max
        if free_idx >= max_code and code_width < MAX_BITS:
            code_width += 1
            max_code = 1 << code_width

    return bytes(dest)


# -- helpers (from A:/tmp/cveg_min_files.py) ----------------------------------

def read_pak_packet(pak_data, idx):
    magic, count = struct.unpack_from('<II', pak_data, 0)
    assert magic == 0xFEEDFACE, f"bad pak magic {magic:#010x}"
    entry = struct.unpack_from('<I', pak_data, 8 + idx*4)[0]
    ptype  = (entry >> 29) & 0x7
    offset = entry & 0x1FFFFFFF
    nxt    = struct.unpack_from('<I', pak_data, 8 + (idx+1)*4)[0] & 0x1FFFFFFF
    return pak_data[offset:nxt].decode('ascii', errors='replace')

def pak_count(pak_data):
    return struct.unpack_from('<I', pak_data, 4)[0]

def fst_entries(fst_path):
    """Return list of (offset, comp_sz, real_sz, name) for a FST."""
    with open(fst_path, 'rb') as f:
        data = f.read()
    magic, count = struct.unpack_from('<II', data, 0)
    is_lz = (magic == 0xFADDECAF)   # else 0xCADDECAF = zlib
    entries = []
    for i in range(count):
        base = 8 + i*266
        off, comp, real, h = struct.unpack_from('<IIII', data, base)
        name_raw = data[base+16:base+266]
        null = name_raw.find(b'\x00')
        name = name_raw[:null].decode('latin-1','replace') if null>=0 else ''
        entries.append((off, comp, real, name, is_lz, data))
    return entries

def decompress_entry(off, comp, real, is_lz, data):
    """Decompress one FST entry, mirroring the engine's LZDecomp dispatch.

    The FST magic (0xFADDECAF = "LZ") does NOT guarantee classic LZW per
    entry: FSTs repacked by our LINUX_BUILD writer keep the LZ magic but
    deflate each entry with zlib. The engine's LZDecomp sniffs the zlib
    header per entry ((b0 & 0x0F) == 8 and (b0*256+b1) % 31 == 0) and tries
    inflate first, falling back to classic LZW. Do the same here — feeding a
    zlib entry to the LZW decoder is what produced "invalid code 301".
    """
    raw = data[off:off+comp]
    if comp == real:
        return raw   # stored uncompressed
    if not is_lz:
        return zlib.decompress(raw)
    looks_zlib = (len(raw) >= 2 and (raw[0] & 0x0F) == 0x08
                  and (raw[0] * 256 + raw[1]) % 31 == 0)
    if looks_zlib:
        try:
            return zlib.decompress(raw)
        except zlib.error:
            pass  # false-positive header — fall back to classic LZW
    return lzw_decomp(raw, real)

def extract_fst_file(fst_path, target_rel, out_path):
    """Extract a single file from an FST into out_path."""
    target_lower = target_rel.lower().replace('\\','/')
    for off, comp, real, name, is_lz, data in fst_entries(fst_path):
        if name.lower().replace('\\','/') == target_lower:
            content = decompress_entry(off, comp, real, is_lz, data)
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, 'wb') as f:
                f.write(content)
            return True
    return False

def fst_has(fst_path, target_rel):
    target_lower = target_rel.lower().replace('\\','/')
    for off, comp, real, name, is_lz, data in fst_entries(fst_path):
        if name.lower().replace('\\','/') == target_lower:
            return True
    return False

# -- Find all CVE-G-exclusive mech/vehicle types from object2.pak --------------

def get_cveg_types(pak_path):
    """Return list of dicts: {obj_num, obj_type, appearance, profile}"""
    with open(pak_path, 'rb') as f:
        pak_data = f.read()
    count = pak_count(pak_data)
    types = []
    for i in range(count):
        text = read_pak_packet(pak_data, i)
        obj_type = appearance = profile = None
        for line in text.split('\n'):
            l = line.strip()
            if 'ObjectTypeNum' in l:
                m = re.search(r'=\s*(\d+)', l)
                if m: obj_type = int(m.group(1))
            if 'AppearanceName' in l:
                m = re.search(r'=\s*"([^"]+)"', l)
                if m: appearance = m.group(1).strip()
            if 'ProfileName' in l:
                m = re.search(r'=\s*"([^"]+)"', l)
                if m: profile = m.group(1).strip()
        if appearance:
            types.append({'idx': i, 'type': obj_type, 'appearance': appearance, 'profile': profile or appearance})
    return types

# -- For each mech appearance, compute required tgl/ files --------------------

MECH_ANIM_NAMES = [
    'StandToPark','ParkToStand','','STtoWK','Walk','StandToPark',
    'WKtoRN','Run','RNToWK','Walk','WKtoST','LimpLeft','LimpRight',
    'Idle','FallBackward','FallForward','HitFront','HitBack','HitLeft',
    'HitRight','Jump','GetupBack','GetupFront','FallForward','FallBackward',
    'FallBackwardDam','FallForwardDam',
]

def mech_tgl_files(appearance):
    """All data/tgl/ files needed for a mech appearance."""
    base = appearance.lower()
    needed = set()
    # Main ini
    needed.add(f'data/tgl/{base}.ini')
    # Left/right arm ini (may be 151-154 bytes stub with just bounds -- still needed)
    needed.add(f'data/tgl/{base}leftarm.ini')
    needed.add(f'data/tgl/{base}rightarm.ini')
    # Main AGL (even if empty header)
    needed.add(f'data/tgl/{base}.agl')
    # Animations
    for anim in MECH_ANIM_NAMES:
        if anim:
            needed.add(f'data/tgl/{base}{anim.lower()}.agl')
    # TGL shapes (main + LODs + shadow + arms + damage)
    for suffix in ['', 'l1', 'x', 'leftarm', 'rightarm', 'fallforwarddam', 'fallbackwarddam']:
        needed.add(f'data/tgl/{base}{suffix}.tgl')
    return needed

def vehicle_tgl_files(appearance):
    """Vehicles need fewer files -- just ini and tgl."""
    base = appearance.lower()
    return {f'data/tgl/{base}.ini', f'data/tgl/{base}.tgl'}

# -- CSV files for mech stats -------------------------------------------------

def mech_csv_files(profile):
    return {f'data/objects/{profile.lower()}.csv'}
