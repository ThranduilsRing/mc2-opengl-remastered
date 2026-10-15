//---------------------------------------------------------------------------
//
// inifile.cpp - This file contains the class declaration for the FitIni Files
//
//				The FitIni file is a ini style file.
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

//---------------------------------------------------------------------------
// Include files

#include "inifile_internal.h"
#include "mc2fmt.h"

//#else
//#include<assert.h>
//#define gosASSERT assert
//#define gos_Malloc malloc
//#define gos_Free free
//#endif

//---------------------------------------------------------------------------
// Static Globals
char fitIniHeader[7] = "FITini";
char fitIniFooter[7] = "FITend";

//---------------------------------------------------------------------------
// class FitIniFile
FitIniFile::FitIniFile (void) : File()
{
	totalBlocks = 0;
	
	currentBlockId = nullptr;
	currentBlockOffset = 0;
	currentBlockSize = 0;
}

//---------------------------------------------------------------------------
FitIniFile::~FitIniFile (void)
{
	close();
}

//---------------------------------------------------------------------------
long FitIniFile::findNextBlockStart (char *line, unsigned long lineLen)
{
	char thisLine[255];
	char *common = nullptr;
	
	do
	{
		if (line)
		{
			readLine((MemoryPtr)line,lineLen);
			common = line;
		}
		else
		{
			readLine((MemoryPtr)thisLine,254);
			common = thisLine;
		}
	}
	while(!eof() && (common[0] != '['));
	
	if (eof())
	{
		return(NO_MORE_BLOCKS);
	}

	return(NO_ERR);
}	

//---------------------------------------------------------------------------
long FitIniFile::countBlocks (void)
{
	long count = 0;
	long oldPosition = logicalPosition;
	
	while (findNextBlockStart() != NO_MORE_BLOCKS)
	{
		count++;
	}
	
	//----------------------------------
	// Move back to where we were.
	seek(oldPosition);
	
	return(count);
}

//---------------------------------------------------------------------------
long FitIniFile::getNextWord (char *&line, char *buffer, unsigned long bufLen)
{
	//--------------------------------------------------
	// Check to see if we are at end of line
	if (*line == '\0')
		return(GET_NEXT_LINE);
		
	//--------------------------------------------------
	// Check to see if the rest of the line is comments
	if (*line == '/')
		return(GET_NEXT_LINE);
		
	//------------------------------------------
	// Find start of word from current location
	while ((*line != '\0') && ((*line == ' ') || (*line == '\t') || (*line == ',')))
	{
		line++;
	}
	
	//--------------------------------------------------
	// Check to see if we are at end of line
	if (*line == '\0')
		return(GET_NEXT_LINE);
		
	//--------------------------------------------------
	// Check to see if the rest of the line is comments
	if (*line == '/')
		return(GET_NEXT_LINE);
		
	//-------------------------------------------
	// Find length of word from current location
	char *startOfWord = line;
	unsigned long wordLength = 0;
	while ((*line != '\0') && ((*line != ' ') && (*line != '\t') && (*line != ',')))
	{
		line++;
		wordLength++;
	}
	
	if (wordLength > bufLen)
		return(BUFFER_TOO_SMALL);
		
	strncpy(buffer, startOfWord, wordLength);
	buffer[wordLength] = '\0';
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::afterOpen (void)
{
	char line[255];
	
	//-------------------------------------------------------
	// Check if we opened this with CREATE and write the
	// FITini Header and position to Write Start.
	if (fileMode == CREATE && parent == nullptr)
	{
		mc2::fmt::to_buf(line, "{} \r\n", mc2::fmt::safe(fitIniHeader));
		write((MemoryPtr)line,strlen(line));

		totalBlocks = 0;
	}
	else
	{
		//------------------------------------------------------
		// Check if this is a FitIniFile by looking for header.
		char chkHeader[12];
	
		readLine((MemoryPtr)chkHeader,11);
		if (strstr(chkHeader,fitIniHeader) == nullptr)
			return(NOT_A_FITINIFILE);

				//------------------------------------------------------
		// Find out how many blocks we have	
		totalBlocks = countBlocks();
		
		//--------------------------------------------------------------------------
		// Allocate RAM for the BlockInfoNodes.  Check if system Heap is available
		m_fileBlocks.assign(totalBlocks, IniBlockNode{});
		
		//--------------------------------------------------------------------------
		// Put Info into fileBlocks.
		
		unsigned long currentBlockNum = 0;
		while (findNextBlockStart(line,254) != NO_MORE_BLOCKS)
		{
			//----------------------------------------------------
			// If we write too many fileBlocks, we will trash RAM
			// Shouldn't be able to happen but...
			if (currentBlockNum == totalBlocks)
				return(TOO_MANY_BLOCKS);

			// blockId is char[50]: the terminator goes at blockId[count-1], so copy up to index 49 (count 50) and no
			// further, and stop at the end of the string, so a long or unterminated [block] header in a mod .fit
			// cannot write past the node or read past the line buffer.
			long count = 1;
			while (line[count] != ']' && line[count] != '\n' && line[count] != '\0' && count < 50)
			{
				m_fileBlocks[currentBlockNum].blockId[count-1] = line[count];
				count++;
			}
			if (count >= 49)
				STOP(("BlockId To large in Fit File %s",m_fileName.c_str()));

			if (line[count] == '\n' || line[count] == '\0')
			{
				char error[256];
				mc2::fmt::to_buf(error, "couldn't resolve block {} in file {}", mc2::fmt::safe(line), mc2::fmt::safe(getFilename()));
				Assert( 0, 0, error );
				return SYNTAX_ERROR;
			}
			
			m_fileBlocks[currentBlockNum].blockId[count-1] = '\0';
			
			//----------------------------------------------------------------------
			// Since we just read all of last line, we now point to start of data
			m_fileBlocks[currentBlockNum].blockOffset = logicalPosition;
			currentBlockNum++;
		}

		//------------------------------------------------------
		// If we didn't read in enough, CD-ROM error?
		if (currentBlockNum != totalBlocks)
			return(NOT_ENOUGH_BLOCKS);
	}

	return(NO_ERR);
}

//---------------------------------------------------------------------------
void FitIniFile::atClose (void)
{
	//------------------------------------------------------------
	// Check if we are in create mode and if so, write the footer
	char line[200];
	if (fileMode == CREATE)
	{
		seek(0,SEEK_END);
		mc2::fmt::to_buf(line, "{} \r\n", mc2::fmt::safe(fitIniFooter));
		write((MemoryPtr)line,strlen(line));
	}

	//-----------------------------
	// Free up the fileBlocks; currentBlockId pointed into them.
	m_fileBlocks.clear();
	totalBlocks = 0;   // a seekBlock after close must see an empty table, not a count that indexes past it
	currentBlockId = nullptr;
}

//---------------------------------------------------------------------------
float FitIniFile::textToFloat (const char *num)
{
	return mc2::fit::textToFloat(num);
}

//---------------------------------------------------------------------------
double FitIniFile::textToDouble (const char *num)
{
	double result = atof(num);
	return(result);
}

// sebi: this is baaaad I know just dumb copy paste, maybe later do template version
//---------------------------------------------------------------------------
int FitIniFile::textToInt (const char *num)
{
	int result = 0;
	
	//------------------------------------
	// Check if Hex Number
	char *hexOffset = (char *)strstr(num,"0x");
	if (hexOffset == nullptr)
	{
		result = atoi(num);
	}
	else
	{
		hexOffset += 2;
		int numDigits = static_cast<int>(strlen(hexOffset)-1);
		for (int i=0; i<=numDigits; i++)
		{
			if (!isalnum(hexOffset[i]) || (isalpha(hexOffset[i]) && toupper(hexOffset[i]) > 'F'))
			{
				hexOffset[i] = 0;	// we've reach a "wrong" character. Either start of a comment or something illegal. Either way, stop evaluation here.
				break;
			}
		}
		numDigits = static_cast<int>(strlen(hexOffset)-1);
		int power = 0;
		for (int count = numDigits;count >= 0;count--,power++)
		{
			unsigned char currentDigit = toupper(hexOffset[count]);
			
			if (currentDigit >= 'A' && currentDigit <= 'F')
			{
				result += (currentDigit - 'A' + 10)<<(4*power);
			}
			else if (currentDigit >= '0' && currentDigit <= '9')
			{
				result += (currentDigit - '0')<<(4*power);
			}
			else
			{
				//---------------------------------------------------------
				// There is a digit in here I don't understand.  Return 0.
				result = 0;
				break;
			}
		}
	}

	
	return(result);
}
//---------------------------------------------------------------------------
long FitIniFile::textToLong (const char *num)
{
	return mc2::fit::textToLong(num);
}


//---------------------------------------------------------------------------
short FitIniFile::textToShort (const char *num)
{
	short result = 0;
	
	//------------------------------------
	// Check if Hex Number
	char *hexOffset = (char *)strstr(num,"0x");
	if (hexOffset == nullptr)
	{
		result = atol(num);
	}
	else
	{
		hexOffset += 2;
		long numDigits = static_cast<long>(strlen(hexOffset)-1);
		for (int i=0; i<=numDigits; i++)
		{
			if (!isalnum(hexOffset[i]) || (isalpha(hexOffset[i]) && toupper(hexOffset[i]) > 'F'))
			{
				hexOffset[i] = 0;	// we've reach a "wrong" character. Either start of a comment or something illegal. Either way, stop evaluation here.
				break;
			}
		}
		numDigits = static_cast<long>(strlen(hexOffset)-1);
		long power = 0;
		for (long count = numDigits;count >= 0;count--,power++)
		{
			unsigned char currentDigit = toupper(hexOffset[count]);
			
			if (currentDigit >= 'A' && currentDigit <= 'F')
			{
				result += (currentDigit - 'A' + 10)<<(4*power);
			}
			else if (currentDigit >= '0' && currentDigit <= '9')
			{
				result += (currentDigit - '0')<<(4*power);
			}
			else
			{
				//---------------------------------------------------------
				// There is a digit in here I don't understand.  Return 0.
				result = 0;
				break;
			}
		}
	}
	
	return(result);
}


//---------------------------------------------------------------------------
char FitIniFile::textToChar (const char *num)
{
	char result = 0;
	
	//------------------------------------
	// Check if Hex Number
	char *hexOffset = (char *)strstr(num,"0x");
	if (hexOffset == nullptr)
	{
		result = atol(num);
	}
	else
	{
		hexOffset += 2;
		long numDigits = static_cast<long>(strlen(hexOffset)-1);
		for (int i=0; i<=numDigits; i++)
		{
			if (!isalnum(hexOffset[i]) || (isalpha(hexOffset[i]) && toupper(hexOffset[i]) > 'F'))
			{
				hexOffset[i] = 0;	// we've reach a "wrong" character. Either start of a comment or something illegal. Either way, stop evaluation here.
				break;
			}
		}
		numDigits = static_cast<long>(strlen(hexOffset)-1);
		long power = 0;
		for (long count = numDigits;count >= 0;count--,power++)
		{
			unsigned char currentDigit = toupper(hexOffset[count]);
			
			if (currentDigit >= 'A' && currentDigit <= 'F')
			{
				result += (currentDigit - 'A' + 10)<<(4*power);
			}
			else if (currentDigit >= '0' && currentDigit <= '9')
			{
				result += (currentDigit - '0')<<(4*power);
			}
			else
			{
				//---------------------------------------------------------
				// There is a digit in here I don't understand.  Return 0.
				result = 0;
				break;
			}
		}
	}
	
	return(result);
}

//---------------------------------------------------------------------------
unsigned long FitIniFile::textToULong (const char *num)
{
	return mc2::fit::textToULong(num);
}


//---------------------------------------------------------------------------
unsigned short FitIniFile::textToUShort (const char *num)
{
	unsigned short result = 0;
	
	//------------------------------------
	// Check if Hex Number
	char *hexOffset = (char *)strstr(num,"0x");
	if (hexOffset == nullptr)
	{
		result = atol(num);
	}
	else
	{
		hexOffset += 2;
		long numDigits = static_cast<long>(strlen(hexOffset)-1);
		for (int i=0; i<=numDigits; i++)
		{
			if (!isalnum(hexOffset[i]) || (isalpha(hexOffset[i]) && toupper(hexOffset[i]) > 'F'))
			{
				hexOffset[i] = 0;	// we've reach a "wrong" character. Either start of a comment or something illegal. Either way, stop evaluation here.
				break;
			}
		}
		numDigits = static_cast<long>(strlen(hexOffset)-1);
		long power = 0;
		for (long count = numDigits;count >= 0;count--,power++)
		{
			unsigned char currentDigit = toupper(hexOffset[count]);
			
			if (currentDigit >= 'A' && currentDigit <= 'F')
			{
				result += (currentDigit - 'A' + 10)<<(4*power);
			}
			else if (currentDigit >= '0' && currentDigit <= '9')
			{
				result += (currentDigit - '0')<<(4*power);
			}
			else
			{
				//---------------------------------------------------------
				// There is a digit in here I don't understand.  Return 0.
				result = 0;
				break;
			}
		}
	}
	
	return(result);
}


//---------------------------------------------------------------------------
unsigned char FitIniFile::textToUChar (const char *num)
{
	unsigned char result = 0;
	
	//------------------------------------
	// Check if Hex Number
	char *hexOffset = (char *)strstr(num,"0x");
	if (hexOffset == nullptr)
	{
		result = atol(num);
	}
	else
	{
		hexOffset += 2;
		long numDigits = static_cast<long>(strlen(hexOffset)-1);
		for (int i=0; i<=numDigits; i++)
		{
			if (!isalnum(hexOffset[i]) || (isalpha(hexOffset[i]) && toupper(hexOffset[i]) > 'F'))
			{
				hexOffset[i] = 0;	// we've reach a "wrong" character. Either start of a comment or something illegal. Either way, stop evaluation here.
				break;
			}
		}
		numDigits = static_cast<long>(strlen(hexOffset)-1);
		long power = 0;
		for (long count = numDigits;count >= 0;count--,power++)
		{
			unsigned char currentDigit = toupper(hexOffset[count]);
			
			if (currentDigit >= 'A' && currentDigit <= 'F')
			{
				result += (currentDigit - 'A' + 10)<<(4*power);
			}
			else if (currentDigit >= '0' && currentDigit <= '9')
			{
				result += (currentDigit - '0')<<(4*power);
			}
			else
			{
				//---------------------------------------------------------
				// There is a digit in here I don't understand.  Return 0.
				result = 0;
				break;
			}
		}
	}
	
	return(result);
}

//---------------------------------------------------------------------------
bool FitIniFile::booleanToLong (const char *num)
{
char testChar = 0;
	while (num[testChar] && isspace(num[testChar]))
		testChar++;

	if ((toupper(num[testChar]) == 'F') || (toupper(num[testChar]) == '0'))
		return FALSE;
	else
		return(TRUE);
}	

//---------------------------------------------------------------------------
long FitIniFile::floatToText (char *result, float num, unsigned long bufLen)
{
	char temp[250];
	mc2::fmt::to_buf(temp, "{:.4f}", num);

	unsigned long numLength = static_cast<unsigned long>(strlen(temp));
	if (numLength >= bufLen)
		return(BUFFER_TOO_SMALL);

	strncpy(result,temp,numLength);
	result[numLength] = '\0';
	
	return(NO_ERR);
}			

//---------------------------------------------------------------------------
long FitIniFile::longToTextDec (char *result, long num, unsigned long bufLen)
{
	char temp[250];
	mc2::fmt::to_buf(temp, "{:d}", mc2::fmt::as_i32(num));

	unsigned long numLength = static_cast<unsigned long>(strlen(temp));
	if (numLength >= bufLen)
		return(BUFFER_TOO_SMALL);

	strncpy(result,temp,numLength);
	result[numLength] = '\0';
	
	return(NO_ERR);
}	

//---------------------------------------------------------------------------
long FitIniFile::longToTextHex (char *result, long num, unsigned long bufLen)
{
	char temp[250];
	mc2::fmt::to_buf(temp, "0x{:x}", mc2::fmt::as_u32(num));

	unsigned long numLength = static_cast<unsigned long>(strlen(temp));
	if (numLength >= bufLen)
		return(BUFFER_TOO_SMALL);

	strncpy(result,temp,numLength);
	result[numLength] = '\0';
	
	return(NO_ERR);
}	

//---------------------------------------------------------------------------
long FitIniFile::shortToTextDec (char *result, short num, unsigned long bufLen)
{
	char temp[250];
	mc2::fmt::to_buf(temp, "{:d}", mc2::fmt::as_i32(num));

	unsigned long numLength = static_cast<unsigned long>(strlen(temp));
	if (numLength >= bufLen)
		return(BUFFER_TOO_SMALL);

	strncpy(result,temp,numLength);
	result[numLength] = '\0';
	
	return(NO_ERR);
}	

//---------------------------------------------------------------------------
long FitIniFile::shortToTextHex (char *result, short num, unsigned long bufLen)
{
	char temp[250];
	mc2::fmt::to_buf(temp, "0x{:x}", mc2::fmt::as_u32(num));

	unsigned long numLength = static_cast<unsigned long>(strlen(temp));
	if (numLength >= bufLen)
		return(BUFFER_TOO_SMALL);

	strncpy(result,temp,numLength);
	result[numLength] = '\0';
	
	return(NO_ERR);
}	

//---------------------------------------------------------------------------
long FitIniFile::byteToTextDec (char *result, byte num, unsigned long bufLen)
{
	char temp[250];
	mc2::fmt::to_buf(temp, "{:d}", mc2::fmt::as_i32(num));

	unsigned long numLength = static_cast<unsigned long>(strlen(temp));
	if (numLength >= bufLen)
		return(BUFFER_TOO_SMALL);

	strncpy(result,temp,numLength);
	result[numLength] = '\0';
	
	return(NO_ERR);
}	

//---------------------------------------------------------------------------
long FitIniFile::byteToTextHex (char *result, byte num, unsigned long bufLen)
{
	char temp[250];
	mc2::fmt::to_buf(temp, "0x{:x}", mc2::fmt::as_u32(num));

	unsigned long numLength = static_cast<unsigned long>(strlen(temp));
	if (numLength >= bufLen)
		return(BUFFER_TOO_SMALL);

	strncpy(result,temp,numLength);
	result[numLength] = '\0';
	
	return(NO_ERR);
}	

//---------------------------------------------------------------------------
long FitIniFile::open (const char* fName, FileMode _mode, long numChild, bool doNotLower)
{
	long result = File::open(fName,_mode,numChild, doNotLower);
	if (result != NO_ERR)
		return(result);
		
	seek(0);

	result = afterOpen();
	
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::open (FilePtr _parent, unsigned long fileSize, long numChild)
{
	numChild = -1;		//Force all parented FitINIs to load from RAM.
	long result = File::open(_parent,fileSize,numChild);
	if (result != NO_ERR)
		return(result);
		
	result = afterOpen();

	return(result);
}

//---------------------------------------------------------------------------
// FITINI-INMEM-OPEN-1: parse a .fit from an in-memory buffer (caller-owned, not
// copied). Mirrors the fName open: base in-RAM File::open(buffer,len), seek(0),
// afterOpen(). No disk, no FST. Used by tools/fitini_inmem_harness to parse
// .fit text game-free.
long FitIniFile::open (const char* buffer, int bufferLength)
{
	long result = File::open(buffer, bufferLength);
	if (result != NO_ERR)
		return(result);

	seek(0);

	result = afterOpen();

	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::create (const char* fName)
{
	long result = File::create(fName);
	afterOpen();
	return(result);
}

long FitIniFile::createWithCase(const char* fName )
{
	long result = File::createWithCase( fName );
	afterOpen();
	return result;
}

//---------------------------------------------------------------------------
void FitIniFile::close (void)
{
	if (isOpen())
	{
		atClose();
		File::close();
	}
}
	
//---------------------------------------------------------------------------
long FitIniFile::writeBlock (const char *blockId)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "\r\n[{}]\r\n", mc2::fmt::safe(blockId));
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdFloat (const char *varName, float value)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "f {} = {:f}\r\n", mc2::fmt::safe(varName), value);
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdBoolean (const char *varName, bool value)
{
	char thisLine[255];
	if (value)
		mc2::fmt::to_buf(thisLine, "b {} = {}\r\n", mc2::fmt::safe(varName), "TRUE");
	else
		mc2::fmt::to_buf(thisLine, "b {} = {}\r\n", mc2::fmt::safe(varName), "FALSE");
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdLong (const char *varName, long value)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "l {} = {:d}\r\n", mc2::fmt::safe(varName), mc2::fmt::as_i32(value));
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdShort (const char *varName, short value)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "s {} = {:d}\r\n", mc2::fmt::safe(varName), mc2::fmt::as_i32(value));
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdChar (const char *varName, char value)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "c {} = {:d}\r\n", mc2::fmt::safe(varName), mc2::fmt::as_i32(value));
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdULong (const char *varName, unsigned long value)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "ul {} = {:d}\r\n", mc2::fmt::safe(varName), mc2::fmt::as_i32(value));
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdUShort (const char *varName, unsigned short value)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "us {} = {:d}\r\n", mc2::fmt::safe(varName), mc2::fmt::as_i32(value));
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdUChar (const char *varName, unsigned char value)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "uc {} = {:d}\r\n", mc2::fmt::safe(varName), mc2::fmt::as_i32(value));
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdString (const char *varName, const char *result)
{
	char thisLine[4096];
	char tmpString[4000];
	memset(tmpString,0,4000);
	strncpy(tmpString,result,3999);

	if (strlen(result) >= 4000)
		PAUSE(("String passed to WriteIdString is longer then 4000 characters"));

	mc2::fmt::to_buf(thisLine, "st {} = \"{}\"\r\n", mc2::fmt::safe(varName), mc2::fmt::safe(result));
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));
	return(bytesWritten);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdUShortArray (const char *varName, unsigned short *array, unsigned long numElements)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "us[{:d}] {} = {:d},", mc2::fmt::as_i32(numElements), mc2::fmt::safe(varName), mc2::fmt::as_i32(array[0]));
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));

	for (long i=1;i<(long)numElements;i++)
	{
		mc2::fmt::to_buf(thisLine, "{:d},", mc2::fmt::as_i32(array[i]));
		bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));
	}
	mc2::fmt::to_buf(thisLine, "\r\n");
	bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));

	return (bytesWritten);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdLongArray (const char *varName, long *array, unsigned long numElements)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "l[{:d}] {} = {:d},", mc2::fmt::as_i32(numElements), mc2::fmt::safe(varName), mc2::fmt::as_i32(array[0]));
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));

	for (long i=1;i<(long)numElements;i++)
	{
		mc2::fmt::to_buf(thisLine, "{:d},", mc2::fmt::as_i32(array[i]));
		bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));
	}
	mc2::fmt::to_buf(thisLine, "\r\n");
	bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));

	return (bytesWritten);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdFloatArray (const char *varName, float *array, unsigned long numElements)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "f[{:d}] {} = {:.2f},", mc2::fmt::as_i32(numElements), mc2::fmt::safe(varName), array[0]);
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));

	for (long i=1;i<(long)numElements;i++)
	{
		mc2::fmt::to_buf(thisLine, " {:.2f},", array[i]);
		bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));
	}
	mc2::fmt::to_buf(thisLine, "\r\n");
	bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));

	return (bytesWritten);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdUCharArray (const char *varName, unsigned char *array, unsigned long numElements)
{
	char thisLine[255];
	mc2::fmt::to_buf(thisLine, "uc[{:d}] {} = {:d},", mc2::fmt::as_i32(numElements), mc2::fmt::safe(varName), mc2::fmt::as_i32(array[0]));
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));

	for (long i=1;i<(long)numElements;i++)
	{
		mc2::fmt::to_buf(thisLine, " {:d},", mc2::fmt::as_i32(array[i]));
		bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));
	}
	mc2::fmt::to_buf(thisLine, "\r\n");
	bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));

	return (bytesWritten);
}
