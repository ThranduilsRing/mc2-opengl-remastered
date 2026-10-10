//--------------------------------------------------------------------------
// LZ Compress Routine
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef _MBCS
#include<gameos.hpp>

#ifndef HEAP_H
#include"heap.h"
#endif

#else
#include<assert.h>
#include<malloc.h>
#define gosASSERT assert
#define gos_Malloc malloc
#define gos_Free free
#endif

#include <zlib.h>
#include "memfunc.h"

// 128K and more cold be faster
#define CHUNK 16384

//---------------------------------------------------------------------------
// Static Globals

#ifndef NULL
#define NULL			0
#endif

typedef unsigned char* MemoryPtr;


// LZ Compress Routine
// Takes a pointer to dest buffer, a pointer to source buffer and len of source.
// returns length of compressed image.
size_t LZCompress (MemoryPtr dest, MemoryPtr src, size_t srcLen)
{
    int ret, flush;
    unsigned have;
    z_stream strm;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];
    int compression_level = -1; // tradeoff between speed/memeory used = to 6, available 0 - 9

    /* allocate deflate state */
    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    ret = deflateInit(&strm, compression_level);
    if (ret != Z_OK)
        return 0;

    MemoryPtr dataptr = src;
    size_t len_left = srcLen;
    size_t out_size = 0;
    do {

        strm.avail_in = static_cast<uInt>(len_left >= CHUNK ? CHUNK : len_left);
        flush = len_left > CHUNK ? Z_NO_FLUSH : Z_FINISH ;
        MemCpy(in, dataptr, strm.avail_in);
        strm.next_in = in;

        dataptr += strm.avail_in;
        len_left -= strm.avail_in;

        do {
            strm.avail_out = CHUNK;
            strm.next_out = out;

            ret = deflate(&strm, flush);    /* no bad return value */
            gosASSERT(ret != Z_STREAM_ERROR);  /* state not clobbered */

            have = CHUNK - strm.avail_out;
            MemCpy(dest, out, have);
            dest += have;
            out_size += have;

        } while(strm.avail_out == 0);
        gosASSERT(strm.avail_in == 0);     /* all input will be used */

    /* done when last data in file processed */
    } while (flush != Z_FINISH);
    gosASSERT(ret == Z_STREAM_END);        /* stream will be complete */

    /* clean up and return */
    (void)deflateEnd(&strm);
    return out_size;
}

