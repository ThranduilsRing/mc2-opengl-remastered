//===========================================================================//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
//***************************************************************************
//
//								SCANNER.H
//
//***************************************************************************

#ifndef ABLSCAN_H
#define	ABLSCAN_H

#ifndef ABLGEN_H
#include"ablgen.h"
#endif

#ifndef ABLERR_H
#include"ablerr.h"
#endif

#include<cstddef>
#include<memory>
#include<string>
#include<stdint.h>

//***************************************************************************

#define	TAB_SIZE				4

//#define	MIN_INTEGER				-2147483648
#define	MAX_INTEGER				2147483647
#define	MAX_DIGIT_COUNT			20
#define	MAX_EXPONENT			20

//***************************************************************************

//------------
// TOKEN CODES

enum TokenCodeType {
	TKN_NONE,
	TKN_IDENTIFIER,
	TKN_NUMBER,
	TKN_TYPE,
	TKN_STRING,
	TKN_STAR,
	TKN_LPAREN,
	TKN_RPAREN,
	TKN_MINUS,
	TKN_PLUS,
	TKN_EQUAL,
	TKN_LBRACKET,
	TKN_RBRACKET,
	TKN_COLON,
	TKN_SEMICOLON,
	TKN_LT,
	TKN_GT,
	TKN_COMMA,
	TKN_PERIOD,
	TKN_FSLASH,
	TKN_EQUALEQUAL,
	TKN_LE,
	TKN_GE,
	TKN_NE,
	TKN_EOF,
	TKN_ERROR,
	TKN_CODE,
	TKN_ORDER,
	TKN_STATE,
	TKN_AND,
	TKN_SWITCH,
	TKN_CASE,
	TKN_CONST,
	TKN_DIV,
	TKN_DO,
	TKN_OF,
	TKN_ELSE,
	TKN_END_IF,
	TKN_END_WHILE,
	TKN_END_FOR,
	TKN_END_FUNCTION,
	TKN_END_ORDER,
	TKN_END_STATE,
	TKN_END_MODULE,
	TKN_END_FSM,
	TKN_END_LIBRARY,
	TKN_END_VAR,
	TKN_END_CODE,
	TKN_END_CASE,
	TKN_END_SWITCH,
	TKN_FOR,
	TKN_FUNCTION,
	TKN_IF,
	TKN_MOD,
	TKN_NOT,
	TKN_OR,
	TKN_REPEAT,
	TKN_THEN,
	TKN_TO,
	TKN_UNTIL,
	TKN_VAR,
	TKN_REF,
	TKN_WHILE,
	TKN_ELSIF,
	TKN_RETURN,
	TKN_MODULE,
	TKN_FSM,
	TKN_TRANS,
	TKN_TRANS_BACK,
	TKN_LIBRARY,
	TKN_ETERNAL,
	TKN_STATIC,
	TKN_POUND,
	TKN_UNEXPECTED_TOKEN,
	TKN_STATEMENT_MARKER,
	TKN_ADDRESS_MARKER,
	NUM_TOKENS
};

enum CharCodeType {
	CHR_LETTER,
	CHR_DIGIT,
	CHR_DQUOTE,
	CHR_SPECIAL,
	CHR_EOF
};

struct ReservedWord {
	const char*			string;
	TokenCodeType	tokenCode;
};

//***************************************************************************

//------------------
// LITERAL structure

enum LiteralType {
	LIT_INTEGER,
	LIT_REAL,
	LIT_STRING
};

struct Literal {
	LiteralType		type;
	struct {
		int         integer;
		float		real;
		char		string[MAXLEN_TOKENSTRING];
	} value;
};

//---------------------------------------------------------------------------

typedef struct CaseItem {
	int					labelValue;
	char*				branchLocation;
	struct CaseItem*	next;
} CaseItem;

using CaseItemPtr = CaseItem*;

//***************************************************************************

class ABLFile {

    public:

		std::string fileName;
		void*		file;

		static long (*createCB) (void** file, const char* fName);
		static long (*openCB) (void** file, const char* fName);
		static long (*closeCB) (void** file);
		static bool (*eofCB) (void* file);
		static long (*remainingCB) (void* file);
		static long (*readCB) (void* file, unsigned char* buffer, long length);
		static int32_t (*readIntCB) (void* file);
		static long (*readLongCB) (void* file);
		static long (*readStringCB) (void* file, unsigned char* buffer);
		static long (*readLineExCB) (void* file, unsigned char* buffer, long maxLength);
		static long (*writeCB) (void* file, unsigned char* buffer, long length);
		static long (*writeByteCB) (void* file, unsigned char byte);
		static long (*writeIntCB) (void* file, int32_t value);
		static long (*writeLongCB) (void* file, long value);
		static long (*writeStringCB) (void* file, const char* buffer);

	public:

		void* operator new (size_t ourSize);

		void operator delete (void *us);

		void init (void);
	
		ABLFile (void) {
			init();
		}

		void destroy (void) noexcept;

		~ABLFile (void) noexcept {
			destroy();
		}
		
		ABLFile(const ABLFile&) = delete;
		ABLFile& operator=(const ABLFile&) = delete;

		// Adopts the opaque callback handle; close releases it before the name.
		void set (void* fPtr) {
			close();
			file = fPtr;
		}

		void* get (void) {
			return(file);
		}

		long create (const char* fileName);

		long open (const char* fileName);

		long close (void);

		bool eof (void);

		// Bytes left to read, or -1 when the file or the host's remaining callback is absent.
		long remaining (void) {
			return (file && remainingCB) ? remainingCB(file) : -1;
		}

		long read (unsigned char* buffer, long length);

		int32_t readInt (void);

		long readLong (void);

		long readString (unsigned char* buffer, long capacity);

		long readLineEx (unsigned char* buffer, long maxLength);

		long write (unsigned char* buffer, long length);

		long writeByte (unsigned char val);

		long writeInt (int32_t val);

		long writeLong (long val);

		long writeString (const char* buffer);
};

//***************************************************************************

// The preprocessing call owns every include; the scanner globals only borrow them.
class ABLSourceFiles {
public:
	std::unique_ptr<ABLFile> files[MAX_INCLUDE_DEPTH];
	ABLSourceFiles();
	~ABLSourceFiles() noexcept;
	ABLSourceFiles(const ABLSourceFiles&) = delete;
	ABLSourceFiles& operator=(const ABLSourceFiles&) = delete;
};

//----------
// FUNCTIONS

inline CharCodeType calcCharCode (long ch);
long isReservedWord (void);

void skipComment (void);
void skipBlanks (void);
void getChar(void);
void crunchToken (void);
void downShiftWord (void);
void getToken(void);
void getWord (void);
void accumulateValue (float* valuePtr, SyntaxErrorType errCode);
void getNumber (void);
void getString (void);
void getSpecial (void);
bool tokenIn (TokenCodeType* tokenList);
void synchronize (TokenCodeType* tokenList1,
				  TokenCodeType* tokenList2,
				  TokenCodeType* tokenList3);
bool getSourceLine (void);
void printLine (char* line);
void initPageHeader (char* fileName);
void printPageHeader (void);

//----------
// VARIABLES

extern char			wordString[MAXLEN_TOKENSTRING];
// The ABL environment owns its memory: one pool resource per kind, alive from ABLi_init to the end of ABLi_close, which hands back
// every block still live (compiled symbols, types and code are never freed one by one). Blocks come back zero-filled: the eternal
// and static blocks are written verbatim into every save, so their bytes must be a function of the game state alone. Module
// instances, files and the debugger objects of other kinds use the C++ allocator (they are owned one by one).
enum class ABLPool : int { Symbol = 0, Code = 1, Stack = 2 };
void* ABL_alloc (ABLPool pool, size_t bytes);
void ABL_free (ABLPool pool, void* block);
// Live blocks across all pools, and whether a block is one of them (tests and the compile rollback use these).
size_t ABL_liveBlocks (void);
bool ABL_ownsBlock (const void* block);
extern void (*ABLDebugPrintCallback) (const char* s);
extern long (*ABLRandomCallback) (long range);
extern void (*ABLSeedRandomCallback) (unsigned long range);
extern unsigned long (*ABLGetTimeCallback) (void);
extern void (*ABLFatalCallback) (long code, const char* s);

//***************************************************************************
	
#endif


