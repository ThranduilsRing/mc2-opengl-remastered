//---------------------------------------------------------------------------
//
// inifile.h - This file contains the class declaration for the FitIni Files
//
//				The FitIni file is a ini style file.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef INIFILE_H
#define INIFILE_H
//---------------------------------------------------------------------------
// Include files

#ifndef DSTD_H
#include"dstd.h"
#endif

#ifndef DINIFILE_H
#include"dinifile.h"
#endif

#ifndef FILE_H
#include"file.h"
#endif

#include <string_view>

//---------------------------------------------------------------------------
// Macro Definitions

inline constexpr unsigned int BLOCK_NOT_FOUND = 0xFADA0000;
inline constexpr unsigned int ID_NOT_FOUND = 0xFADA0001;
inline constexpr unsigned int DATA_NOT_CORRECT_TYPE = 0xFADA0002;
inline constexpr unsigned int BUFFER_TOO_SMALL = 0xFADA0003;
inline constexpr unsigned int NOT_A_FITINIFILE = 0xFADA0004;
inline constexpr unsigned int NO_RAM_FOR_INI_BLOCKS = 0xFADA0005;
inline constexpr unsigned int NO_MORE_BLOCKS = 0xFADA0006;
inline constexpr unsigned int TOO_MANY_BLOCKS = 0xFADA0007;
inline constexpr unsigned int NOT_ENOUGH_BLOCKS = 0xFADA0008;
inline constexpr unsigned int VARIABLE_NOT_FOUND = 0xFADA0009;
inline constexpr unsigned int SYNTAX_ERROR = 0xFADA000A;
inline constexpr unsigned int NOT_ENOUGH_ELEMENTS_FOR_ARRAY = 0xFADA000B;
inline constexpr unsigned int GET_NEXT_LINE = 0xFADA000C;
inline constexpr unsigned int USER_ARRAY_TOO_SMALL = 0xFADA000D;
inline constexpr unsigned int TOO_MANY_ELEMENTS = 0xFADA000E;

//---------------------------------------------------------------------------
// Enums

//---------------------------------------------------------------------------
// Structs
struct IniBlockNode
{
	char blockId[50];
	unsigned long blockOffset;
};

//---------------------------------------------------------------------------
//									FitIniFile
class FitIniFile : public File
{
	// Data Members
	//--------------
	protected:
		unsigned long 	totalBlocks;					//Total number of blocks in file
		std::vector<IniBlockNode> m_fileBlocks;		//Data for blocks to speed up file (one node per [block]; currentBlockId points into it)
		char 				*currentBlockId;				//Id of current block
		unsigned long 	currentBlockOffset;			//Offset into file of block start
		unsigned long 	currentBlockSize;				//Length of current block

	// Member Functions
	//------------------
	protected:
		long afterOpen (void);
		void atClose (void);
		
		long findNextBlockStart (char *line = nullptr, unsigned long lineLen = 0);
		long countBlocks (void);
		
		long getNextWord (char *&line, char *buffer, unsigned long bufLen);

		float textToFloat (mc2::czstring num);
		double textToDouble (mc2::czstring num);
		
		long textToLong (mc2::czstring num);
		int textToInt(mc2::czstring num);
		unsigned long textToULong (mc2::czstring num);
		
		short textToShort (mc2::czstring num);
		unsigned short textToUShort (mc2::czstring num);
		
		char textToChar (mc2::czstring num);
		unsigned char textToUChar (mc2::czstring num);

		bool booleanToLong (mc2::czstring num);

		float mathToFloat (mc2::czstring num);
		
		long mathToLong (mc2::czstring num);
		unsigned long mathToULong (mc2::czstring num);
		
		short mathToShort (mc2::czstring num);
		unsigned short mathToUShort (mc2::czstring num);
		
		char mathToChar (mc2::czstring num);
		unsigned char mathToUChar (mc2::czstring num);

		long floatToText (char *result, float num, unsigned long bufLen);
		
		long longToTextDec (char *result, long num, unsigned long bufLen);
		long longToTextHex (char *result, long num, unsigned long bufLen);

		long shortToTextDec (char *result, short num, unsigned long bufLen);
		long shortToTextHex (char *result, short num, unsigned long bufLen);

		long byteToTextDec (char *result, byte num, unsigned long bufLen);	
		long byteToTextHex (char *result, byte num, unsigned long bufLen);
		
		long copyString (char* dest, char *src, unsigned long bufLen);

	public:
		FitIniFile (void);
		~FitIniFile (void);

		virtual long open (mc2::czstring fName, FileMode _mode = READ, long numChildren = 50, bool doNotLower = false);
		virtual long open (FilePtr _parent, unsigned long fileSize, long numChildren = 50);
		// FITINI-INMEM-OPEN-1: parse a .fit directly from an in-memory buffer
		// (caller-owned, not copied) -- reuses the base in-RAM File::open(buffer,len)
		// then runs afterOpen(). Enables game-free parsing without disk/FST. The
		// explicit override is required: the two open() decls above otherwise
		// name-hide the base File::open(const char*, int) buffer overload.
		virtual long open (const char* buffer, int bufferLength);

		virtual long create (mc2::czstring fName);
		virtual long createWithCase(mc2::czstring fName );


		virtual void close (void);

		virtual FileClass getFileClass (void)
		{
			return INIFILE;
		}

		long seekBlock (mc2::czstring blockId);

		long readIdFloat (std::string_view varName, float &value);
		long readIdDouble (std::string_view varName, double &value);
		
		long readIdBoolean (std::string_view varName, bool &value);
		long readIdInt (std::string_view varName, int &value);
		long readIdLong (std::string_view varName, long &value);
		long readIdULong (std::string_view varName, uint64_t &value);
        // sebi
		long readIdULong (std::string_view varName, DWORD &value);
		
		long readIdShort (std::string_view varName, short &value);
		long readIdUShort (std::string_view varName, unsigned short &value);
		
		long readIdChar (std::string_view varName, char &value);
		long readIdUChar (std::string_view varName, unsigned char &value);
		
		long readIdString (std::string_view varName, char *result, unsigned long bufferSize);

		long getIdStringLength (std::string_view varName);
		
		long readIdFloatArray (std::string_view varName, float *result, unsigned long numElements);
		
		long readIdIntArray (std::string_view varName, int *result, unsigned int numElements);

		long readIdLongArray (std::string_view varName, long *result, unsigned long numElements);
		long readIdULongArray (std::string_view varName, unsigned long *result, unsigned long numElements);
		
		long readIdShortArray (std::string_view varName, short *result, unsigned long numElements);
		long readIdUShortArray (std::string_view varName, unsigned short *result, unsigned long numElements);
		
		long readIdCharArray (std::string_view varName, char *result, unsigned long numElements);
		long readIdUCharArray (std::string_view varName, unsigned char *result, unsigned long numElements);
		
		unsigned long getIdFloatArrayElements (std::string_view varName);
		
		unsigned long getIdLongArrayElements (std::string_view varName);
		unsigned long getIdULongArrayElements (std::string_view varName);
											 
		unsigned long getIdShortArrayElements (std::string_view varName);
		unsigned long getIdUShortArrayElements (std::string_view varName);
		
		unsigned long getIdCharArrayElements (std::string_view varName);
		unsigned long getIdUCharArrayElements (std::string_view varName);
		
		long writeBlock (std::string_view blockId);

		long writeIdFloat (std::string_view varName, float value);
		
		long writeIdBoolean (std::string_view varName, bool value);
		long writeIdLong (std::string_view varName, long value);
		long writeIdULong (std::string_view varName, unsigned long value);
		
		long writeIdShort (std::string_view varName, short value);
		long writeIdUShort (std::string_view varName, unsigned short value);
		
		long writeIdChar (std::string_view varName, char value);
		long writeIdUChar (std::string_view varName, unsigned char value);
		
		long writeIdString (mc2::czstring varName, mc2::czstring result);

		long writeIdFloatArray (std::string_view varName, float *array, unsigned long numElements);
		long writeIdLongArray (std::string_view varName, long *array, unsigned long numElements);
		long writeIdUShortArray (std::string_view varName, unsigned short *array, unsigned long numElements);
		long writeIdUCharArray (std::string_view varName, unsigned char *array, unsigned long numElements);
};

//---------------------------------------------------------------------------
#endif

//---------------------------------------------------------------------------
//
// Edit Log
//
//
//---------------------------------------------------------------------------
