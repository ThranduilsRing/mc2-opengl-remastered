//******************************************************************************************
//	color.cpp - This file contains the code for the Color Tables
//
//	MechCommander 2
//
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//

#ifndef COLOR_H
#include"color.h"
#endif

#ifndef PATHS_H
#include"paths.h"
#endif

#ifndef CIDENT_H
#include"cident.h"
#endif

#ifndef INIFILE_H
#include"inifile.h"
#endif

#ifndef HEAP_H
#include"heap.h"
#endif

#include<gameos.hpp>
#include<format>
//----------------------------------------------------------------------------------
DWORD **colorRGBLookup = nullptr;
long numColorRGBTables = 0;

constexpr int MAX_COLOR_ENTRIES = 56;
//----------------------------------------------------------------------------------
void initColorTables (void)
{
	FullPathFileName colorPath;
	colorPath.init(cameraPath,"colors",".fit");

	FitIniFile colorFile;
	long result = colorFile.open(colorPath);
	gosASSERT(result == NO_ERR);

	result = colorFile.seekBlock("Main");
	gosASSERT(result == NO_ERR);

	result = colorFile.readIdLong("NumTables",numColorRGBTables);
	gosASSERT(result == NO_ERR);

	colorRGBLookup = static_cast<DWORD **>(std::malloc(sizeof(DWORD *) * numColorRGBTables));
	gosASSERT(colorRGBLookup != nullptr);

	memset(colorRGBLookup,0,sizeof(DWORD *) * numColorRGBTables);

	for (long i=0;i<numColorRGBTables;i++)
	{
		char tableBlock[256];
		auto tableBlockEnd = std::format_to_n(tableBlock, sizeof(tableBlock) - 1, "Table{}", i).out;
		*tableBlockEnd = '\0';

		result = colorFile.seekBlock(tableBlock);
		gosASSERT(result == NO_ERR);

		colorRGBLookup[i] = static_cast<DWORD *>(std::malloc(sizeof(DWORD) * MAX_COLOR_ENTRIES));
		gosASSERT(colorRGBLookup[i] != nullptr);

		DWORD *table = colorRGBLookup[i];

		for (long j=0;j<MAX_COLOR_ENTRIES;j++)
		{
			char colorBlock[256];
			auto colorBlockEnd = std::format_to_n(colorBlock, sizeof(colorBlock) - 1, "Color{}", j).out;
			*colorBlockEnd = '\0';

            // sebi
            long unsigned int tmp;
			result = colorFile.readIdULong(colorBlock, tmp);
            table[j] = static_cast<DWORD>(tmp);
			gosASSERT(result == NO_ERR);
		}
	}

	colorFile.close();
}	

//----------------------------------------------------------------------------------
void destroyColorTables (void)
{
	for (long i=0;i<numColorRGBTables;i++)
	{
		std::free(colorRGBLookup[i]);
		colorRGBLookup[i] = nullptr;
	}

	if ( colorRGBLookup )
		std::free(colorRGBLookup);
	colorRGBLookup = nullptr;
}	

//----------------------------------------------------------------------------------
DWORD *getColorTable (long tableNum)
{
	if ((tableNum >= 0) && (tableNum < numColorRGBTables))
	{
		return(colorRGBLookup[tableNum]);
	}

	return(nullptr);
}	

//----------------------------------------------------------------------------------
