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
	fileBlocks = nullptr;
	
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
		sprintf(line,"%s \r\n",fitIniHeader);
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
		fileBlocks = (IniBlockNode *)systemHeap->Malloc(sizeof(IniBlockNode) * totalBlocks);
		
		gosASSERT(fileBlocks != nullptr);

		memset(fileBlocks,0,sizeof(IniBlockNode) * totalBlocks);
		
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
				fileBlocks[currentBlockNum].blockId[count-1] = line[count];
				count++;
			}
			if (count >= 49)
				STOP(("BlockId To large in Fit File %s",fileName));

			if (line[count] == '\n' || line[count] == '\0')
			{
				char error[256];
				snprintf( error, sizeof(error), "couldn't resolve block %s in file %s", line, getFilename() );
				Assert( 0, 0, error );
				return SYNTAX_ERROR;
			}
			
			fileBlocks[currentBlockNum].blockId[count-1] = '\0';
			
			//----------------------------------------------------------------------
			// Since we just read all of last line, we now point to start of data
			fileBlocks[currentBlockNum].blockOffset = logicalPosition;
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
		sprintf(line,"%s \r\n",fitIniFooter);
		write((MemoryPtr)line,strlen(line));
	}

	//-----------------------------
	// Free up the fileBlocks
	systemHeap->Free(fileBlocks);
	fileBlocks = nullptr;
}

//---------------------------------------------------------------------------
float FitIniFile::textToFloat (const char *num)
{
	float result = atof(num);
	return(result);
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
	long result = 0;
	
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
	unsigned long result = 0;
	
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
	sprintf(temp,"%.4f",num);

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
	sprintf(temp,"%d",num);

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
	sprintf(temp,"0x%x",num);

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
	sprintf(temp,"%d",num);

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
	sprintf(temp,"0x%x",num);

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
	sprintf(temp,"%d",num);

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
	sprintf(temp,"0x%x",num);

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
	sprintf(thisLine,"\r\n[%s]\r\n",blockId);
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdFloat (const char *varName, float value)
{
	char thisLine[255];
	sprintf(thisLine,"f %s = %f\r\n",varName,value);
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdBoolean (const char *varName, bool value)
{
	char thisLine[255];
	if (value)
		sprintf(thisLine,"b %s = %s\r\n",varName,"TRUE");
	else
		sprintf(thisLine,"b %s = %s\r\n",varName,"FALSE");
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdLong (const char *varName, long value)
{
	char thisLine[255];
	sprintf(thisLine,"l %s = %d\r\n",varName,value);
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdShort (const char *varName, short value)
{
	char thisLine[255];
	sprintf(thisLine,"s %s = %d\r\n",varName,value);
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdChar (const char *varName, char value)
{
	char thisLine[255];
	sprintf(thisLine,"c %s = %d\r\n",varName,value);
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdULong (const char *varName, unsigned long value)
{
	char thisLine[255];
	sprintf(thisLine,"ul %s = %d\r\n",varName,value);
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdUShort (const char *varName, unsigned short value)
{
	char thisLine[255];
	sprintf(thisLine,"us %s = %d\r\n",varName,value);
	
	long result = write((MemoryPtr)thisLine,strlen(thisLine));
	return(result);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdUChar (const char *varName, unsigned char value)
{
	char thisLine[255];
	sprintf(thisLine,"uc %s = %d\r\n",varName,value);
	
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

	sprintf(thisLine,"st %s = \"%s\"\r\n",varName,result);
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));
	return(bytesWritten);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdUShortArray (const char *varName, unsigned short *array, unsigned long numElements)
{
	char thisLine[255];
	sprintf(thisLine,"us[%d] %s = %d,",numElements,varName,array[0]);
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));

	for (long i=1;i<(long)numElements;i++)
	{
		sprintf(thisLine,"%d,",array[i]);
		bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));
	}
	sprintf(thisLine,"\r\n");
	bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));

	return (bytesWritten);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdLongArray (const char *varName, long *array, unsigned long numElements)
{
	char thisLine[255];
	sprintf(thisLine,"l[%d] %s = %d,",numElements,varName,array[0]);
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));

	for (long i=1;i<(long)numElements;i++)
	{
		sprintf(thisLine,"%d,",array[i]);
		bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));
	}
	sprintf(thisLine,"\r\n");
	bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));

	return (bytesWritten);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdFloatArray (const char *varName, float *array, unsigned long numElements)
{
	char thisLine[255];
	sprintf(thisLine,"f[%d] %s = %.2f,",numElements,varName,array[0]);
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));

	for (long i=1;i<(long)numElements;i++)
	{
		sprintf(thisLine," %.2f,",array[i]);
		bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));
	}
	sprintf(thisLine,"\r\n");
	bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));

	return (bytesWritten);
}

//---------------------------------------------------------------------------
long FitIniFile::writeIdUCharArray (const char *varName, unsigned char *array, unsigned long numElements)
{
	char thisLine[255];
	sprintf(thisLine,"uc[%d] %s = %d,",numElements,varName,array[0]);
	
	long bytesWritten = write((MemoryPtr)thisLine,strlen(thisLine));

	for (long i=1;i<(long)numElements;i++)
	{
		sprintf(thisLine," %d,",array[i]);
		bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));
	}
	sprintf(thisLine,"\r\n");
	bytesWritten += write((MemoryPtr)thisLine,strlen(thisLine));

	return (bytesWritten);
}
