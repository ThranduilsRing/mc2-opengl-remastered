// FitIniFile: seekBlock and the scalar readId* readers, readIdString and getIdStringLength.
// Declared in the class headers; shared names in inifile_internal.h.
#include "inifile_internal.h"
#include "mc2fmt.h"


//---------------------------------------------------------------------------
long FitIniFile::seekBlock (const char *blockId)
{
	unsigned long blockNum = 0;
	
	while ((blockNum < totalBlocks) && (strcmp(m_fileBlocks[blockNum].blockId,blockId) != 0))
	{
		blockNum++;
	}
	
	if (blockNum == totalBlocks)
	{
		return(BLOCK_NOT_FOUND);
	}

	//----------------------------------------
	// Block was found, seek to that position
	seek(m_fileBlocks[blockNum].blockOffset);
	
	//----------------------------------------
	// Setup all current Block Info
	currentBlockId = m_fileBlocks[blockNum].blockId;
	currentBlockOffset = m_fileBlocks[blockNum].blockOffset;
	
	blockNum++;
	if (blockNum == totalBlocks)
	{
		currentBlockSize = getLength() - currentBlockOffset;
	}
	else
	{
		currentBlockSize = m_fileBlocks[blockNum].blockOffset - currentBlockOffset;
	}
		
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdFloat (const char *varName, float &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "f {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0.0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = textToFloat(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdDouble (const char *varName, double &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "f {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0.0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = textToDouble(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

long FitIniFile::readIdInt(const char *varName, int &value)
{
    long tmp = 0;
    long rv = readIdLong (varName, tmp);
    value = tmp;
    return rv;
}

//---------------------------------------------------------------------------
long FitIniFile::readIdLong (const char *varName, long &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "l {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = textToLong(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdBoolean (const char *varName, bool &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "b {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = booleanToLong(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdShort (const char *varName, short &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "s {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = textToShort(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdChar (const char *varName, char &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "c {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = textToChar(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

long FitIniFile::readIdULong (const char *varName, DWORD &value)
{
    uint64_t tmp = 0;
    long rv = readIdULong (varName, tmp);
    value = (DWORD)tmp;
    return rv;
}

//---------------------------------------------------------------------------
long FitIniFile::readIdULong (const char *varName, uint64_t &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "ul {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
			char* tc = &line[strlen(searchString)];
			
			while (isspace(*tc))
				tc++;
				
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = textToULong(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdUShort (const char *varName, unsigned short &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "us {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = textToUShort(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdUChar (const char *varName, unsigned char &value)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "uc {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		value = 0;
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;
		value = textToUChar(equalSign);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::copyString (char *dest, char *src, unsigned long bufLen)
{
	unsigned long offset = 0;
	//---------------------
	// Find starting Quote
	while (*src != '"')
	{
		src++;
	}
	
	src++;		//One more to get to first character past quote.
	
	//---------------------------------------
	// Copy each character until close quote
	while (*src != '"' && offset < bufLen)
	{
		dest[offset] = *src;
		src++;
		offset++;
	}

	//----------------------------------------------------
	// If this string is longer than buffer, let em know.
	if (offset == bufLen)
	{
		return(BUFFER_TOO_SMALL);
	}

	//---------------------------------
	// otherwise, NULL term and return	
	dest[offset] = '\0';
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdString (const char *varName, char *result, unsigned long bufferSize)
{
	char line[2048];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "st {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,2047);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strstr(line, "=");
	if (equalSign)
	{
		equalSign++;

		

		char* pFound = nullptr;
		char* pFirstEqual = strstr( equalSign, "\"" );
		// strings can span more than one line, make sure there is another equal sign
		if ( pFirstEqual && !strstr( pFirstEqual+1, "\"" ) ) 
		{
			long curLen = static_cast<long>(strlen( equalSign ));
			char tmpLine[2047];
			do
			{
				readLine((MemoryPtr)tmpLine,2047);
				pFound = strstr( tmpLine, "\"" );

				long addedLen = static_cast<long>(strlen( tmpLine ) + 1);
				if ( curLen + addedLen < 2048 )
				{
					strcat( equalSign, tmpLine );
					strcat( equalSign, "\n" );
					curLen += addedLen;
				}
				else
					break;

			}while( !pFound && (logicalPosition < endOfBlock) );
		}
		
		long errorCode = copyString(result,equalSign,bufferSize);
		if (errorCode != NO_ERR)
			return(errorCode);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
	
	
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::getIdStringLength (const char *varName)
{
	char line[255];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------
	// Put prefix on varName.
	mc2::fmt::to_buf(searchString, "st {}", mc2::fmt::safe(varName));
	
	//--------------------------------
	// Search line by line for varName
	int	testy;
	do
	{
		readLine((MemoryPtr)line,254);
		testy = S_strnicmp(line, searchString, strlen(searchString));
		if (testy == 0)
		{
		char* tc = &line[strlen(searchString)];
			while (isspace(*tc))
				tc++;
			if (*tc != '=')
				testy = 1;
		}
	}
	while((testy != 0) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}
	
	char *equalSign = strchr(line, '"');
	if (equalSign)
	{
		equalSign++;
		char* end = equalSign;
		while (*end != '"' && *end != 0)
			end++;
		if (*end)
			return (end - equalSign + 1);	// + for terminator
		else
			return(SYNTAX_ERROR);
	}

	return(SYNTAX_ERROR);
}
