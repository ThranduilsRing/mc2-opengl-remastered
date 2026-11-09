#define LOGISTICSMECH_CPP
/*************************************************************************************************\
LogisticsMech.cpp			: Implementation of the LogisticsMech component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"logisticsmech.h"
#include"logisticsdata.h"
#include"inifile.h"
#include "mc2fmt.h"
#include "logistics_deployment.h"

LogisticsMech::LogisticsMech( LogisticsVariant* pVar, int Count ) : pVariant( pVar )
{
	ID = (Count << 24 | pVar->getID() );
	pilot = nullptr;
	forceGroup = 0;
}

LogisticsMech::~LogisticsMech()
{
	if ( pilot )
		pilot->setUsed( 0 );

	pilot = nullptr;
}

void LogisticsMech::setPilot( LogisticsPilot* pPilot )
{
	if ( pilot )
		pilot->setUsed( 0 );

	pilot = pPilot;
	if ( pilot )
		pilot->setUsed( 1 );
}

void LogisticsMech::setVariant( LogisticsVariant* pVar )
{
	pVariant = pVar;
	ID = ( LogisticsData::instance->createInstanceID( pVar ) << 24 | pVar->getID() );

}


long LogisticsMech::save( FitIniFile& file, long counter )
{
	logistics_deployment::MechRecord record;
	record.chassis = pVariant->getFileName();
	record.variant = pVariant->getName();
	if (pilot)
		record.pilot = pilot->getFileName();
	record.designer = pVariant->isDesignerMech();
	record.slot = forceGroup;
	logistics_deployment::writeMech(file, counter, record);

	return 0;
}

void	LogisticsMech::setColors( unsigned long base, unsigned long high1, unsigned long high2 )
{
	baseColor = base;
	highlightColor1 = high1;
	highlightColor2 = high2;
}

void	LogisticsMech::getColors( unsigned long& base, unsigned long& high1, unsigned long& high2 ) const
{
	base = baseColor;
	high1 = highlightColor1;
	high2 = highlightColor2;
}

//*************************************************************************************************
// end of file ( LogisticsMech.cpp )
