//***************************************************************************
//
//	fit_number.h -- the number readers of FIT text values, free of the file and heap classes so the mission editor can share them
//
//	MechCommander II
//
//***************************************************************************

#pragma once

#include <cctype>
#include <cstdlib>
#include <cstring>

// inifile.cpp keeps a verbatim copy until editor/CMakeLists.txt includes this header instead of extracting the bodies;
// test_fit_number.cpp holds the two in lockstep.
// The quirks are the game's contract with stock and mod data, so they are kept: hex is any text after the first lowercase "0x"
// (a comment "7 // 0xff" reads 255), "0X" is not hex (atol reads "0X16" as 0), and hex stops at the first non-hex character.
// The hex scan writes a terminator into the caller's text at that character, so the argument must be writable.
namespace mc2::fit
{

inline float textToFloat (const char *num)
{
	float result = atof(num);
	return(result);
}

inline long textToLong (const char *num)
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

inline unsigned long textToULong (const char *num)
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

} // namespace mc2::fit
