//--------------------------------------------------------------------------
// LZ Decompress Routine
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Static Globals

#define HASH_CLEAR		256               //clear hash table command code
#define HASH_EOF        257               //End Of Data command code
#define HASH_FREE       258               //First Hash Table Chain Offset Value

#define BASE_BITS       9
#define MAX_BIT_INDEX   (1 << BASE_BITS)
#define NO_RAM_FOR_LZ_DECOMP	0xCBCB0002

#ifndef NULL
#define NULL			0
#endif

#include <zlib.h>

typedef unsigned char* MemoryPtr;

#ifndef LINUX_BUILD

struct HashStruct
{
	unsigned short Chain;
	unsigned char Suffix;
};

typedef HashStruct *HashStructPtr;
	
HashStructPtr	LZOldChain = NULL;			//Old Chain Value Found
HashStructPtr	LZChain = NULL;				//Current Chain Value Found 
unsigned long	LZMaxIndex = 0;				//Max index value in Hash Table
unsigned long	LZCodeMask = 0;
unsigned long	LZFreeIndex = 0;			//Current Free index into Hash Table
MemoryPtr		LZSrcBufEnd = NULL;			//ptr to 3rd from last byte in src buffer
MemoryPtr		LZOrigDOSBuf = NULL;		//original offset to start of src buffer
char			LZHashBuffer[16384];
char			LZOldSuffix = 0;			//Current Suffix Value found


//-----------------------------

//-------------------------------------------------------------------------------
// LZ DeCompress Routine
// Takes a pointer to dest buffer, a pointer to source buffer and len of source.
// returns length of decompressed image.
long LZDecomp (MemoryPtr dest, MemoryPtr src, unsigned long srcLen)
{
	long result = 0;
	
	__asm
	{
		mov		esi,src
		mov		ebx,srcLen

		mov		edi,dest
		xor		eax,eax

		xor		ecx,ecx							// CH and CL used
		lea		ebx,[ebx+esi-3]

		mov		LZOldChain,eax
		mov		LZSrcBufEnd,ebx

		mov		LZChain,eax
		mov		LZMaxIndex,eax

		mov		LZCodeMask,eax
		mov		LZFreeIndex,eax
	
		mov		LZCodeMask,MAX_BIT_INDEX-1
		mov		LZMaxIndex,MAX_BIT_INDEX		//max index for 9 bits == 512

		mov		LZFreeIndex,HASH_FREE			//set index to 258
		mov		LZOldSuffix,al

		mov		ch,BASE_BITS
		jmp		GetCode

//--------------------------------------------------------------------------
//                                                                          
// ClearHash restarts decompression assuming that it is starting from the   
//           beginning                                                      
//                                                                          
//--------------------------------------------------------------------------

ClearHash:
		mov		ch,BASE_BITS		        	//set up for nine bit codes
		mov		LZCodeMask,MAX_BIT_INDEX-1

		mov		LZMaxIndex,MAX_BIT_INDEX    	//max index for 9 bits == 512
		mov		LZFreeIndex,HASH_FREE       	//set index to 258
		
		cmp		esi,LZSrcBufEnd
	   	ja		error

		mov		eax,[esi+0]
		xor		ebx,ebx

		shr		eax,cl

		add		cl,ch
		mov		edx,LZCodeMask

		mov		bl,cl
		and   	cl,07h

		shr   	ebx,3
		and		eax,edx

		add		esi,ebx
		nop

		mov		LZOldChain,eax      	//previous Chain Offset.
		mov		LZOldSuffix,al

		mov		[edi],al
		inc		edi
//-------------------------------------------------------------------------
// ReadCode gets the next hash code (9 BITS) from LZDOSBuff
//         this WILL ReadFile more data if the buffer is empty
//-------------------------------------------------------------------------
GetCode:
		cmp		esi,LZSrcBufEnd
    	ja		error						// Read Passed End?

		mov		eax,[esi+0]
		xor		ebx,ebx

		shr		eax,cl

		add		cl,ch
		mov		edx,LZCodeMask

		mov		bl,cl
		and   	cl,07h

		shr   	ebx,3
		and		eax,edx

		add		esi,ebx
		nop

		cmp		eax,HASH_EOF
		je		eof

		cmp		eax,HASH_CLEAR         		//are we to clear out hash table?
		je		ClearHash
//---------------------------------------------------------------------------
//                                                                           
// Handle Chain acts on two types of Codes, A previously tabled one and a new
// one. On a previously tabled one, the chain value and suffix for that code 
// are preserved into OldSuffix and OldChain. The block operates on searching
// backward in the chains until a chain offset of 0-255 is found (meaning the
// terminal character has been reached.) Each character in the chain is saved
// on the stack.                                                             
//                                                                           
//---------------------------------------------------------------------------

//HandleChain:
		mov		edx,esp 		
		dec		esp

		mov		LZChain,eax        				//Save new chain as well
		lea		ebx, [LZHashBuffer+eax+eax*2]
		
		cmp		eax,LZFreeIndex					//is code in HASH TABLE already?
		jl		InTable      					//if yes, then process chain
	
		mov		al,LZOldSuffix					//get back the old suffix and plant it
		mov		[esp],al                   		//onto the stack for processing later
		dec		esp
		mov		[ebx+2],al				
		mov		eax,LZOldChain     				//get Old chain for creation of Old Chain
		mov		[ebx],ax
		lea		ebx, [LZHashBuffer+eax+eax*2]

InTable:
		test	ah,ah							//(ax<255) is current chain a character?  
		jz		ChainEnd   						//if yes, then begin Print out

		mov		al,[ebx+2]		 				//push suffix onto stack
		mov		[esp],al                   		//onto the stack for processing later

		dec		esp
		movzx	eax,word ptr [ebx]				//get chain to this code
		lea		ebx, [LZHashBuffer+eax+eax*2]
		jmp   InTable            				//and keep filling up
		
ChainEnd:
		mov		LZOldSuffix,al				//save last character in chain
		mov		[esp],al                   		//onto the stack for processing later

		sub		edx,esp 	
		mov		ebx,LZFreeIndex    				//get new code number index

send_bytes:
		mov		al,[esp]
		inc		esp

		mov		[edi],al
		inc		edi

		dec		edx
		jnz		send_bytes

//---------------------------------------------------------------------------
//                                                                           
// Here we add another chain to the hash table so that we continually use it 
// for more decompressions.                                                  
//                                                                           
//---------------------------------------------------------------------------

		mov		al,LZOldSuffix
		mov		edx,LZOldChain

		mov		byte ptr [LZHashBuffer+ebx+ebx*2+2],al
		mov		word ptr [LZHashBuffer+ebx+ebx*2],dx

		inc		ebx
		mov		eax,LZChain

		mov		LZFreeIndex,ebx
		mov		edx,LZMaxIndex

		mov		LZOldChain,eax
		cmp		ebx,edx

		jl    	GetCode

		cmp		ch,12
		je		GetCode

		inc		ch
		mov		ebx,LZCodeMask

		mov		eax,LZMaxIndex
		add		ebx,ebx

		add		eax,eax
		or		ebx,1

		mov		LZMaxIndex,eax
		mov		LZCodeMask,ebx

		jmp   	GetCode


error:
eof:
		sub		edi,dest
		mov		result,edi
	}
	return(result);
}
#else // LINUX_BUILD

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
        strm.avail_in = len_left>CHUNK ? CHUNK : len_left;
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
    enum { HASH_CLEAR_C = 256, HASH_EOF_C = 257, HASH_FREE_C = 258 };
    enum { BASE_BITS_C = 9, MAX_BITS_C = 12 };

    // Dictionary: each entry has 16-bit parent chain + 8-bit suffix. Max 4096 entries.
    static unsigned short parentTbl[4096];
    static unsigned char  suffixTbl[4096];

    if (srcLen < 2) return 0;
    MemoryPtr destStart = dest;

    // Bit-stream reader: pull next `bits` bits, LSB-first, across byte boundaries.
    size_t srcPos = 0;
    unsigned int bitBuf = 0;
    int bitsInBuf = 0;
    auto readCode = [&](int bits) -> int {
        while (bitsInBuf < bits) {
            if (srcPos >= srcLen) return -1;
            bitBuf |= ((unsigned int)src[srcPos++]) << bitsInBuf;
            bitsInBuf += 8;
        }
        int code = (int)(bitBuf & ((1u << bits) - 1u));
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
                if (sp >= (int)sizeof(stk)) return 0; // chain too deep, corrupt
                stk[sp++] = suffixTbl[c];
                c = parentTbl[c];
            }
            firstByte = (unsigned char)c;
            stk[sp++] = firstByte;
        } else if (c == freeIdx && prevCode >= 0) {
            // KwKwK special case: code == freeIdx means "previous string + its first char"
            stk[sp++] = prevFirstByte;
            int pc = prevCode;
            while (pc >= HASH_FREE_C) {
                if (sp >= (int)sizeof(stk)) return 0;
                stk[sp++] = suffixTbl[pc];
                pc = parentTbl[pc];
            }
            firstByte = (unsigned char)pc;
            stk[sp++] = firstByte;
        } else {
            // Invalid code (> freeIdx or code==freeIdx without prev)
            return 0;
        }

        // Emit in reverse (stack top-down) — bounds-checked against destMax to prevent
        // heap overflow when input is slightly corrupt or size-of-dest under-allocated.
        while (sp > 0) {
            if ((size_t)(dest - destStart) >= destMax) {
                return (size_t)(dest - destStart);  // truncate on overflow, don't corrupt heap
            }
            *dest++ = stk[--sp];
        }

        // Add new entry: prevCode + firstByte-of-current-string
        if (prevCode >= 0 && freeIdx < (1 << MAX_BITS_C)) {
            parentTbl[freeIdx] = (unsigned short)prevCode;
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

    return (size_t)(dest - destStart);
}

//-------------------------------------------------------------------------------
// LZ DeCompress Routine — tries zlib first (covers FSTs we packed), falls back
// to classic LZW (covers original Microsoft-packed FSTs like Wolfman's MC2X).
size_t LZDecomp (MemoryPtr dest, MemoryPtr src, size_t srcLen)
{
    if (!dest || !src || srcLen == 0) return 0;

    // Fast-path: zlib. First 2 bytes of a zlib stream satisfy (src[0]*256+src[1]) % 31 == 0
    // and the low-nybble of src[0] is 8 (deflate method). This is cheap to check and
    // avoids uselessly invoking inflate() on LZW-framed data.
    bool looksZlib = (srcLen >= 2)
                     && ((src[0] & 0x0F) == 0x08)
                     && (((unsigned int)src[0] * 256u + (unsigned int)src[1]) % 31u == 0u);
    // No true dest bound known in the 3-arg overload — fall back to the same
    // 8x expansion heuristic used by the classic-LZW path. Callers that know
    // the real allocated dest size should use the 4-arg overload below.
    size_t destGuess = srcLen * 16 + 4096;
    if (looksZlib) {
        size_t n = LZDecompZlib_(dest, src, srcLen, destGuess);
        if (n > 0) return n;
    }

    return LZDecompClassicLZW_(dest, src, srcLen, destGuess);
}

// 4-arg overload — prefer this when the caller knows the allocated dest size; prevents
// heap overflow on both the zlib fast-path and the classic-LZW fallback.
size_t LZDecomp (MemoryPtr dest, MemoryPtr src, size_t srcLen, size_t destMax)
{
    if (!dest || !src || srcLen == 0) return 0;

    bool looksZlib = (srcLen >= 2)
                     && ((src[0] & 0x0F) == 0x08)
                     && (((unsigned int)src[0] * 256u + (unsigned int)src[1]) % 31u == 0u);
    if (looksZlib) {
        size_t n = LZDecompZlib_(dest, src, srcLen, destMax);
        if (n > 0) return n;
    }

    return LZDecompClassicLZW_(dest, src, srcLen, destMax);
}
#endif // LINUX_BUILD
