"""
tools/mc2x_import/strings.py -- PE RT_STRING walker, ported from the proven legacy
script A:/tmp/extract_mc2xres.py (lines 9-69: PE parse + RT_STRING block loop).
Edits only vs the original: the hardcoded dll_path/fit_path and the base-fit
append/merge are dropped; the walk is wrapped in extract_strings(dll_path).
"""
import struct


def extract_strings(dll_path):
    """Parse a PE DLL's RT_STRING (type 6) resources. Return {string_id: text}."""
    with open(dll_path, 'rb') as f:
        data = f.read()

    def u16(o): return struct.unpack_from('<H', data, o)[0]
    def u32(o): return struct.unpack_from('<I', data, o)[0]

    pe_off = u32(0x3c)
    num_sections = u16(pe_off + 6)
    opt_hdr_size = u16(pe_off + 20)
    opt_hdr_off = pe_off + 24
    data_dir_off = opt_hdr_off + 96
    rsrc_rva = u32(data_dir_off + 2 * 8)

    sections = []
    sec_off = opt_hdr_off + opt_hdr_size
    for i in range(num_sections):
        vaddr = u32(sec_off + 12); raw_off = u32(sec_off + 20)
        raw_size = u32(sec_off + 16); vsize = u32(sec_off + 8)
        sections.append((vaddr, raw_off, raw_size, vsize))
        sec_off += 40

    def rva_to_off(rva):
        for vaddr, raw_off, raw_size, vsize in sections:
            if vaddr <= rva < vaddr + max(raw_size, vsize):
                return raw_off + (rva - vaddr)
        return None

    rsrc_off = rva_to_off(rsrc_rva)

    def parse_dir(off):
        named = u16(rsrc_off + off + 12)
        id_count = u16(rsrc_off + off + 14)
        entries = []
        eoff = off + 16
        for i in range(named + id_count):
            noff = u32(rsrc_off + eoff)
            doff = u32(rsrc_off + eoff + 4)
            is_dir = bool(doff & 0x80000000)
            name = noff & 0x7FFFFFFF
            child = doff & 0x7FFFFFFF
            entries.append((name, child, is_dir))
            eoff += 8
        return entries

    strings = {}
    for typ, toff, is_dir in parse_dir(0):
        if typ != 6: continue
        for block_id, boff, _ in parse_dir(toff):
            for lang, loff, _ in parse_dir(boff):
                data_rva = u32(rsrc_off + loff)
                data_size = u32(rsrc_off + loff + 4)
                raw = rva_to_off(data_rva)
                if raw is None: continue
                block_data = data[raw:raw + data_size]
                base_id = (block_id - 1) * 16
                pos = 0
                for i in range(16):
                    if pos + 2 > len(block_data): break
                    slen = struct.unpack_from('<H', block_data, pos)[0]
                    pos += 2
                    if slen > 0:
                        text = block_data[pos:pos + slen * 2].decode('utf-16-le', errors='replace')
                        strings[base_id + i] = text
                    pos += slen * 2
    return strings
