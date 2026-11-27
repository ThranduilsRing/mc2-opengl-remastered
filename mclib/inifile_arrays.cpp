// FitIniFile: the readId*Array readers and the getId*ArrayElements counters.
// Declared in the class headers; shared names in inifile_internal.h.
#include "inifile_internal.h"
#include "mc2fmt.h"


//---------------------------------------------------------------------------
long FitIniFile::readIdFloatArray (std::string_view varName, float *result, unsigned long numElements)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "f[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	

	if (actualElements > numElements)
		return(USER_ARRAY_TOO_SMALL);
		
	//------------------------------
	// Parse out the elements here.
	char *equalSign = strstr(line, "=");
	unsigned long elementsRead = 0;
	if (equalSign)
	{
		equalSign++; //Move to char past equal sign.

		//--------------------------------------------------------------------------------
		// Now, loop until we reach the end of block or we've read in all of the elements
		while ((logicalPosition < endOfBlock) && (elementsRead < actualElements))
		{
			long errorCode = getNextWord(equalSign,elementString,9);
			if (errorCode == GET_NEXT_LINE)
			{
				readLine(reinterpret_cast<MemoryPtr>(line),254);
				equalSign = line;
				continue;
			}

			if (errorCode != NO_ERR)
			{
				return(errorCode);
			}
			
			result[elementsRead] = textToFloat(elementString);
			elementsRead++;
		}
		
		if (logicalPosition >= endOfBlock && elementsRead < actualElements)
			return(NOT_ENOUGH_ELEMENTS_FOR_ARRAY);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
		
	return(NO_ERR);
}

// sebi: this is baaaad I know just dumb copy paste, maybe later do template version
//---------------------------------------------------------------------------
long FitIniFile::readIdIntArray (std::string_view varName, int *result, unsigned int numElements)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "l[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	

	if (actualElements > numElements)
		return(USER_ARRAY_TOO_SMALL);
		
	//------------------------------
	// Parse out the elements here.
	char *equalSign = strstr(line, "=");
	unsigned long elementsRead = 0;
	if (equalSign)
	{
		equalSign++; //Move to char past equal sign.

		//--------------------------------------------------------------------------------
		// Now, loop until we reach the end of block or we've read in all of the elements
		while ((logicalPosition < endOfBlock) && (elementsRead < actualElements))
		{
			long errorCode = getNextWord(equalSign,elementString,9);
			if (errorCode == GET_NEXT_LINE)
			{
				readLine(reinterpret_cast<MemoryPtr>(line),254);
				equalSign = line;
				continue;
			}

			if (errorCode != NO_ERR)
			{
				return(errorCode);
			}
			
			result[elementsRead] = textToLong(elementString);
			elementsRead++;
		}
		
		if (logicalPosition >= endOfBlock && elementsRead < actualElements)
			return(NOT_ENOUGH_ELEMENTS_FOR_ARRAY);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
		
	return(NO_ERR);
}
		
//---------------------------------------------------------------------------
long FitIniFile::readIdLongArray (std::string_view varName, long *result, unsigned long numElements)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "l[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	

	if (actualElements > numElements)
		return(USER_ARRAY_TOO_SMALL);
		
	//------------------------------
	// Parse out the elements here.
	char *equalSign = strstr(line, "=");
	unsigned long elementsRead = 0;
	if (equalSign)
	{
		equalSign++; //Move to char past equal sign.

		//--------------------------------------------------------------------------------
		// Now, loop until we reach the end of block or we've read in all of the elements
		while ((logicalPosition < endOfBlock) && (elementsRead < actualElements))
		{
			long errorCode = getNextWord(equalSign,elementString,9);
			if (errorCode == GET_NEXT_LINE)
			{
				readLine(reinterpret_cast<MemoryPtr>(line),254);
				equalSign = line;
				continue;
			}

			if (errorCode != NO_ERR)
			{
				return(errorCode);
			}
			
			result[elementsRead] = textToLong(elementString);
			elementsRead++;
		}
		
		if (logicalPosition >= endOfBlock && elementsRead < actualElements)
			return(NOT_ENOUGH_ELEMENTS_FOR_ARRAY);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
		
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdULongArray (std::string_view varName, unsigned long *result, unsigned long numElements)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "ul[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 3;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	

	if (actualElements > numElements)
		return(USER_ARRAY_TOO_SMALL);
		
	//------------------------------
	// Parse out the elements here.
	char *equalSign = strstr(line, "=");
	unsigned long elementsRead = 0;
	if (equalSign)
	{
		equalSign++; //Move to char past equal sign.

		//--------------------------------------------------------------------------------
		// Now, loop until we reach the end of block or we've read in all of the elements
		while ((logicalPosition < endOfBlock) && (elementsRead < actualElements))
		{
			long errorCode = getNextWord(equalSign,elementString,9);
			if (errorCode == GET_NEXT_LINE)
			{
				readLine(reinterpret_cast<MemoryPtr>(line),254);
				equalSign = line;
				continue;
			}

			if (errorCode != NO_ERR)
			{
				return(errorCode);
			}
			
			result[elementsRead] = textToULong(elementString);
			elementsRead++;
		}
		
		if (logicalPosition >= endOfBlock && elementsRead < actualElements)
			return(NOT_ENOUGH_ELEMENTS_FOR_ARRAY);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
		
	return(NO_ERR);
}
		
//---------------------------------------------------------------------------
long FitIniFile::readIdShortArray (std::string_view varName, short *result, unsigned long numElements)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "s[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	

	if (actualElements > numElements)
		return(USER_ARRAY_TOO_SMALL);
		
	//------------------------------
	// Parse out the elements here.
	char *equalSign = strstr(line, "=");
	unsigned long elementsRead = 0;
	if (equalSign)
	{
		equalSign++; //Move to char past equal sign.

		//--------------------------------------------------------------------------------
		// Now, loop until we reach the end of block or we've read in all of the elements
		while ((logicalPosition < endOfBlock) && (elementsRead < actualElements))
		{
			long errorCode = getNextWord(equalSign,elementString,9);
			if (errorCode == GET_NEXT_LINE)
			{
				readLine(reinterpret_cast<MemoryPtr>(line),254);
				equalSign = line;
				continue;
			}

			if (errorCode != NO_ERR)
			{
				return(errorCode);
			}
			
			result[elementsRead] = textToShort(elementString);
			elementsRead++;
		}
		
		if (logicalPosition >= endOfBlock && elementsRead < actualElements)
			return(NOT_ENOUGH_ELEMENTS_FOR_ARRAY);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
		
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdUShortArray (std::string_view varName, unsigned short *result, unsigned long numElements)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "us[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 3;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	

	if (actualElements > numElements)
		return(USER_ARRAY_TOO_SMALL);
		
	//------------------------------
	// Parse out the elements here.
	char *equalSign = strstr(line, "=");
	unsigned long elementsRead = 0;
	if (equalSign)
	{
		equalSign++; //Move to char past equal sign.

		//--------------------------------------------------------------------------------
		// Now, loop until we reach the end of block or we've read in all of the elements
		while ((logicalPosition < endOfBlock) && (elementsRead < actualElements))
		{
			long errorCode = getNextWord(equalSign,elementString,9);
			if (errorCode == GET_NEXT_LINE)
			{
				readLine(reinterpret_cast<MemoryPtr>(line),254);
				equalSign = line;
				continue;
			}

			if (errorCode != NO_ERR)
			{
				return(errorCode);
			}
			
			result[elementsRead] = textToUShort(elementString);
			elementsRead++;
		}
		
		if (logicalPosition >= endOfBlock && elementsRead < actualElements)
			return(NOT_ENOUGH_ELEMENTS_FOR_ARRAY);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
		
	return(NO_ERR);
}
		
//---------------------------------------------------------------------------
long FitIniFile::readIdCharArray (std::string_view varName, char *result, unsigned long numElements)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "c[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	

	if (actualElements > numElements)
		return(USER_ARRAY_TOO_SMALL);
		
	//------------------------------
	// Parse out the elements here.
	char *equalSign = strstr(line, "=");
	unsigned long elementsRead = 0;
	if (equalSign)
	{
		equalSign++; //Move to char past equal sign.

		//--------------------------------------------------------------------------------
		// Now, loop until we reach the end of block or we've read in all of the elements
		while ((logicalPosition < endOfBlock) && (elementsRead < actualElements))
		{
			long errorCode = getNextWord(equalSign,elementString,9);
			if (errorCode == GET_NEXT_LINE)
			{
				readLine(reinterpret_cast<MemoryPtr>(line),254);
				equalSign = line;
				continue;
			}

			if (errorCode != NO_ERR)
			{
				return(errorCode);
			}
			
			result[elementsRead] = textToChar(elementString);
			elementsRead++;
		}
		
		if (logicalPosition >= endOfBlock && elementsRead < actualElements)
			return(NOT_ENOUGH_ELEMENTS_FOR_ARRAY);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
		
	return(NO_ERR);
}

//---------------------------------------------------------------------------
long FitIniFile::readIdUCharArray (std::string_view varName, unsigned char *result, unsigned long numElements)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "uc[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 3;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	

	if (actualElements > numElements)
		return(USER_ARRAY_TOO_SMALL);
		
	//------------------------------
	// Parse out the elements here.
	char *equalSign = strstr(line, "=");
	unsigned long elementsRead = 0;
	if (equalSign)
	{
		equalSign++; //Move to char past equal sign.

		//--------------------------------------------------------------------------------
		// Now, loop until we reach the end of block or we've read in all of the elements
		while ((logicalPosition < endOfBlock) && (elementsRead < actualElements))
		{
			long errorCode = getNextWord(equalSign,elementString,9);
			if (errorCode == GET_NEXT_LINE)
			{
				readLine(reinterpret_cast<MemoryPtr>(line),254);
				equalSign = line;
				continue;
			}

			if (errorCode != NO_ERR)
			{
				return(errorCode);
			}
			
			result[elementsRead] = textToUChar(elementString);
			elementsRead++;
		}
		
		if (logicalPosition >= endOfBlock && elementsRead < actualElements)
			return(NOT_ENOUGH_ELEMENTS_FOR_ARRAY);
	}
	else
	{
		return(SYNTAX_ERROR);
	}
		
	return(NO_ERR);
}
		
//---------------------------------------------------------------------------
unsigned long FitIniFile::getIdFloatArrayElements (std::string_view varName)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "f[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	
	
	return(actualElements);
}	
	
//---------------------------------------------------------------------------
unsigned long FitIniFile::getIdLongArrayElements (std::string_view varName)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "l[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	
	
	return(actualElements);
}

//---------------------------------------------------------------------------
unsigned long FitIniFile::getIdULongArrayElements (std::string_view varName)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "ul[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 3;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	
	
	return(actualElements);
}
											 
//---------------------------------------------------------------------------
unsigned long FitIniFile::getIdShortArrayElements (std::string_view varName)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "s[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	
	
	return(actualElements);
}

//---------------------------------------------------------------------------
unsigned long FitIniFile::getIdUShortArrayElements (std::string_view varName)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "us[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 3;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	
	
	return(actualElements);
}

//---------------------------------------------------------------------------
unsigned long FitIniFile::getIdCharArrayElements (std::string_view varName)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "c[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 2;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	
	
	return(actualElements);
}

//---------------------------------------------------------------------------
unsigned long FitIniFile::getIdUCharArrayElements (std::string_view varName)
{
	char line[255];
	char frontSearch[10];
	char searchString[255];
	
	//--------------------------------
	// Always read from top of Block.
	seek(currentBlockOffset);
	unsigned long endOfBlock = currentBlockOffset+currentBlockSize;
	
	//------------------------------------------------------------------
	// Create two search strings so that we can match any number in []
	mc2::fmt::to_buf(frontSearch, "uc[");
	mc2::fmt::to_buf(searchString, "] {}", varName);
	
	//--------------------------------
	// Search line by line for varName
	char *fSearch = nullptr;
	char *bSearch = nullptr;
	
	do
	{
		readLine(reinterpret_cast<MemoryPtr>(line),254);
		
		fSearch = strstr(line,frontSearch);
		bSearch = strstr(line,searchString);
	}
	while(((fSearch == nullptr) || (bSearch == nullptr)) && (logicalPosition < endOfBlock));
	
	if (logicalPosition >= endOfBlock)
	{
		return(VARIABLE_NOT_FOUND);
	}

	//--------------------------------------
	// Get number of elements in array.
	char elementString[10];
	unsigned long actualElements;
	
	fSearch += 3;												//Move pointer to first number in brackets.
	long numDigits = bSearch - fSearch;

	if (numDigits <= 0 || numDigits > 9)
		return(TOO_MANY_ELEMENTS);

	strncpy(elementString,fSearch,numDigits);
	elementString[numDigits] = '\0';
	
	actualElements = textToULong(elementString);	
	
	return(actualElements);
}
