#include <memory>
#include <array>
#define SIMPLECOMPONENTLISTBOX_CPP
/*************************************************************************************************\
SimpleComponentListBox.cpp			: Implementation of the SimpleComponentListBox component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include"simplecomponentlistbox.h"
#include"../resource.h"
#include"logisticsvariant.h"
#include"logisticscomponent.h"
#include"logisticsdata.h"
#include"cmponent.h"
#include"unitprofile.h"	// EQUIP-ADD-UI-NAME-1 (S3): equipmentAddUiTextForStringId
#include "mc2fmt.h"
#include "memfunc.h"

//-------------------------------------------------------------------------------------------------
ComponentListBox::ComponentListBox()
{
	clickSFX = -1;
	highlightSFX = -1;
	disabledSFX = -1;
	helpID = IDS_HELP_WEAPONS;
}

ComponentListBox::~ComponentListBox()
{
	aListBox::destroy();
}

void ComponentListBox::setMech( LogisticsVariant* pMech )
{
	removeAllItems( true );

	if ( pMech )
	{
		LogisticsComponent* components[256];
		long componentCount = 256;

		pMech->getComponents( componentCount, components );

		setComponents( componentCount, components );
		
		int sensor = pMech->getSensorID();
		int ECM = pMech->getECM();

		if ( sensor > 0 )
		{
			auto textItem = std::make_unique<aTextListItem>( IDS_SALVAGE_AREA_COMPONENTS );
			// EQUIP-ADD-UI-NAME-1 (S3): ADD-filled ids have no 32000-block
			// string; use the def DisplayName. Miss -> stock string id path.
			const char* addName = equipmentAddUiTextForStringId( 32000 + sensor );
			if ( addName )
				textItem->setText( addName );
			else
				textItem->setText( 32000 + sensor );
			textItem->setColor( 0xffc29b00 );
			textItem->setHelpID( IDS_HELP_COMP0 + sensor );
			AddItem( textItem.release() );
		}

		if ( ECM > 0 )
		{
			auto textItem = std::make_unique<aTextListItem>( IDS_SALVAGE_AREA_COMPONENTS );
			// EQUIP-ADD-UI-NAME-1 (S3): same DisplayName fallback as above.
			const char* addNameEcm = equipmentAddUiTextForStringId( 32000 + ECM );
			if ( addNameEcm )
				textItem->setText( addNameEcm );
			else
				textItem->setText( 32000 + ECM );
			textItem->setColor( 0xffc29b00 );
			textItem->setHelpID( IDS_HELP_COMP0 + ECM );
			AddItem( textItem.release() );
		}

	}
}

void ComponentListBox::setVehicle( LogisticsVehicle* pVeh )
{
	removeAllItems( true );

	if ( pVeh )
	{
		LogisticsComponent* components[256];
		long componentCount = 256;

		pVeh->getComponents( componentCount, components );

		setComponents( componentCount, components );	

	}
}


void ComponentListBox::setComponents( long componentCount, LogisticsComponent** components )
{
		LogisticsComponent* finalList[64];


		uint32_t finalListCount[64];
//		long stringIDs[4] = { IDS_SHORT, IDS_MEDIUM, IDS_LONG, IDS_COMPONENT};
		std::array<uint32_t, 4> colors = { 0xff6E7C00, 0xff005392, 0xffA21600, 0xffc29b00};
//		long headerColors[4] = { 0xFFC8E100, 0xff0091FF, 0xFFFF0000, 0xffFF8A00 };

		for ( int i = 0; i < 4; i++ ) // do short, medium long
		{
			MemSet(finalList, 0);
			MemSet(finalListCount, 0);
			for ( int j = 0; j < componentCount; j++ )
			{
				if ( components[j]->getRangeType() == static_cast<LogisticsComponent::WEAPON_RANGE>(i) ) // short, med, long
				{
					bool bFound = 0;
                    int k = 0;
					for ( ; k < 64; k++ )
					{
						if ( finalList[k] == components[j])
						{
							finalListCount[k]++;
							bFound = true;
							break;
						}
						else if ( !finalList[k] )
							break;
					}

					if ( !bFound )
					{
						finalList[k] = components[j];
						finalListCount[k]++;
					}
				}
			}

			if ( finalList[0] )
			{
				// add the header
				// no more headers, keeping code just in case
//				aTextListItem* textItem = new aTextListItem( IDS_SALVAGE_AREA_COMPONENTS );
//				textItem->setText( stringIDs[i] );
//				textItem->setColor( headerColors[i] );
//				AddItem( textItem );

				// add each componet
				for( int j = 0; j < 64; j++ )
				{
					if ( !finalList[j] )
						break;

					auto textItem = std::make_unique<aTextListItem>( IDS_SALVAGE_AREA_COMPONENTS );
					if ( finalList[j]->getType() != COMPONENT_FORM_JUMPJET )
					{
						textItem->setText( std::format( "{:d}/{:d} {}", mc2::fmt::as_i32( finalListCount[j] ), mc2::fmt::as_i32( finalListCount[j] ), mc2::fmt::safe( finalList[j]->getName() ) ) );
					}
					else
						textItem->setText( finalList[j]->getName()  );
					textItem->setColor( colors[i] );
					textItem->setHelpID( IDS_HELP_COMP0 + finalList[j]->getID() );
					AddItem( textItem.release() );
				
				}

			}
		}

}


//*************************************************************************************************
// end of file ( SimpleComponentListBox.cpp )
