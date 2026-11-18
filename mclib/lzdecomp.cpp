//--------------------------------------------------------------------------
// LZ Decompress Routine
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Static Globals

inline constexpr int HASH_CLEAR = 256; //clear hash table command code
inline constexpr int HASH_EOF = 257; //End Of Data command code
inline constexpr int HASH_FREE = 258; //First Hash Table Chain Offset Value

inline constexpr int BASE_BITS = 9;
inline constexpr int MAX_BIT_INDEX = (1 << BASE_BITS);
constexpr unsigned int NO_RAM_FOR_LZ_DECOMP = 0xCBCB0002;

#include <zlib.h>

using MemoryPtr = unsigned char*;


#include"gameos.hpp"
#include<string.h>

//-------------------------------------------------------------------------------
// zlib-inflate path (used by OUR writer since the LINUX_BUILD LZCompress is zlib deflate).
// Returns 0 on any failure so caller can fall back to classic-LZW.
// destMax bounds the total bytes written to dest; if the stream would exceed it,
// the function returns 0 (treated as failure so the 3-arg caller falls back to
// classic-LZW, and the 4-arg caller surfaces the truncation to its own caller).
static size_t LZDecompZlib_(MemoryPtr dest, MemoryPtr src, size_t srcLen, size_t destMax)
{
    const int CHUNK = 16384;
    int ret;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.avail_in = 0;
    strm.next_in = Z_NULL;
    ret = inflateInit(&strm);
    if (ret != Z_OK)
        return 0;

    size_t len_left = srcLen;
    MemoryPtr dataptr = src;
    size_t out_size = 0;
    MemoryPtr destStart = dest;
    do {
        strm.avail_in = static_cast<uInt>(len_left>CHUNK ? CHUNK : len_left);
        memcpy(in, dataptr, strm.avail_in);
        dataptr += strm.avail_in;
        len_left -= strm.avail_in;

        if (strm.avail_in == 0)
            break;
        strm.next_in = in;

        do {
            strm.avail_out = CHUNK;
            strm.next_out = out;

            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR) { inflateEnd(&strm); return 0; }
            switch (ret) {
                case Z_NEED_DICT:
                    ret = Z_DATA_ERROR;
                case Z_DATA_ERROR:
                case Z_MEM_ERROR:
                    inflateEnd(&strm);
                    return 0;
            }

            have = CHUNK - strm.avail_out;
            if (out_size + have > destMax) { inflateEnd(&strm); return 0; }
            memcpy(dest, out, have);
            dest += have;
            out_size += have;

        } while (strm.avail_out == 0);
    } while (ret != Z_STREAM_END);

    (void)inflateEnd(&strm);
    (void)destStart;
    return ret == Z_STREAM_END ? out_size : 0;
}

//-------------------------------------------------------------------------------
// Classic variable-width LZW decoder (9→12 bit codes, clear=256, eof=257, first=258).
// C port of the original Microsoft MC2 __asm routine at the top of this file.
// Needed because Wolfman's (and presumably other legacy) FSTs were packed with the
// original asm LZW compressor, NOT with our LINUX_BUILD zlib deflate.
// On detection: we try zlib first; if it returns 0, we fall back to this.
static size_t LZDecompClassicLZW_(MemoryPtr dest, MemoryPtr src, size_t srcLen, size_t destMax)
{
    // sebi 2026-04-21
    constexpr int HASH_CLEAR_C = 256;
    constexpr int HASH_EOF_C = 257;
    constexpr int HASH_FREE_C = 258;
    constexpr int BASE_BITS_C = 9;
    constexpr int MAX_BITS_C = 12;

    // Dictionary: each entry has 16-bit parent chain + 8-bit suffix. Max 4096 entries.
    static thread_local unsigned short parentTbl[4096];
    static thread_local unsigned char  suffixTbl[4096];

    if (srcLen < 2) return 0;
    MemoryPtr destStart = dest;

    // Bit-stream reader: pull next `bits` bits, LSB-first, across byte boundaries.
    size_t srcPos = 0;
    unsigned int bitBuf = 0;
    int bitsInBuf = 0;
    auto readCode = [&](int bits) -> int {
        while (bitsInBuf < bits) {
            if (srcPos >= srcLen) return -1;
            bitBuf |= (static_cast<unsigned int>(src[srcPos++])) << bitsInBuf;
            bitsInBuf += 8;
        }
        int code = static_cast<int>((bitBuf & ((1u << bits) - 1u)));
        bitBuf >>= bits;
        bitsInBuf -= bits;
        return code;
    };

    // State
    int codeWidth = BASE_BITS_C;
    int maxCode = 1 << codeWidth;              // 512 initially
    int freeIdx = HASH_FREE_C;                 // 258
    int prevCode = -1;
    unsigned char prevFirstByte = 0;

    // Stack for chain-emission (LIFO)
    unsigned char stk[4096];
    int sp = 0;

    for (;;) {
        int code = readCode(codeWidth);
        if (code < 0) break;
        if (code == HASH_EOF_C) break;

        if (code == HASH_CLEAR_C) {
            codeWidth = BASE_BITS_C;
            maxCode = 1 << codeWidth;
            freeIdx = HASH_FREE_C;
            prevCode = -1;
            continue;
        }

        // Expand code → byte sequence onto stack
        int c = code;
        unsigned char firstByte;
        if (c < freeIdx) {
            // In dictionary
            while (c >= HASH_FREE_C) {
                if (sp >= static_cast<int>(sizeof(stk))) return 0; // chain too deep, corrupt
                stk[sp++] = suffixTbl[c];
                c = parentTbl[c];
            }
            firstByte = static_cast<unsigned char>(c);
            stk[sp++] = firstByte;
        } else if (c == freeIdx && prevCode >= 0) {
            // KwKwK special case: code == freeIdx means "previous string + its first char"
            stk[sp++] = prevFirstByte;
            int pc = prevCode;
            while (pc >= HASH_FREE_C) {
                if (sp >= static_cast<int>(sizeof(stk))) return 0;
                stk[sp++] = suffixTbl[pc];
                pc = parentTbl[pc];
            }
            firstByte = static_cast<unsigned char>(pc);
            stk[sp++] = firstByte;
        } else {
            // Invalid code (> freeIdx or code==freeIdx without prev)
            return 0;
        }

        // Emit in reverse (stack top-down) — bounds-checked against destMax to prevent
        // heap overflow when input is slightly corrupt or size-of-dest under-allocated.
        while (sp > 0) {
            if (static_cast<size_t>((dest - destStart)) >= destMax) {
                return static_cast<size_t>((dest - destStart));  // truncate on overflow, don't corrupt heap
            }
            *dest++ = stk[--sp];
        }

        // Add new entry: prevCode + firstByte-of-current-string
        if (prevCode >= 0 && freeIdx < (1 << MAX_BITS_C)) {
            parentTbl[freeIdx] = static_cast<unsigned short>(prevCode);
            suffixTbl[freeIdx] = firstByte;
            ++freeIdx;

            // Grow code width when dictionary fills
            if (freeIdx >= maxCode && codeWidth < MAX_BITS_C) {
                ++codeWidth;
                maxCode = 1 << codeWidth;
            }
        }

        prevCode = code;
        prevFirstByte = firstByte;
    }

    return static_cast<size_t>((dest - destStart));
}

// LZ DeCompress: tries zlib first (covers FSTs we packed), falls back to classic LZW (covers original
// Microsoft-packed FSTs like Wolfman's MC2X). destMax is the allocated size of dest: it bounds both the zlib
// fast-path and the classic-LZW fallback, so a corrupt stream cannot overflow the caller's buffer.
size_t LZDecomp (MemoryPtr dest, MemoryPtr src, size_t srcLen, size_t destMax)
{
    if (!dest || !src || srcLen == 0) return 0;

    bool looksZlib = (srcLen >= 2)
                     && ((src[0] & 0x0F) == 0x08)
                     && ((static_cast<unsigned int>(src[0]) * 256u + static_cast<unsigned int>(src[1])) % 31u == 0u);
    if (looksZlib) {
        size_t n = LZDecompZlib_(dest, src, srcLen, destMax);
        if (n > 0) return n;
    }

    return LZDecompClassicLZW_(dest, src, srcLen, destMax);
}
