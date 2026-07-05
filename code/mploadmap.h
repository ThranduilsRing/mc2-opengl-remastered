#ifndef MPLOADMAP_H
#define MPLOADMAP_H
/*************************************************************************************************\
MPLoadMap.h			: Interface for the MPLoadMap component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

//*************************************************************************************************

#ifndef LOGISTICSDIALOG_H
#include"logisticsdialog.h"
#endif

#include"asystem.h"
#include"alistbox.h"
#include"attributemeter.h"
#include"simplecamera.h"

#ifndef AANIM_H
#include"aanim.h"
#endif

class aButton;

namespace UiDefs { class GameOSPage; }


class MPLoadMap : public LogisticsDialog
{
public:
	
	MPLoadMap();
	virtual ~MPLoadMap();
	
	void init(FitIniFile* file);
	bool isDone();
	virtual void		begin();
	virtual void		end();
 	virtual void		render( int xOffset, int yOffset );
	virtual void		render();
	virtual void		update();
	virtual int			handleMessage( unsigned long, unsigned long );

	void				beginSingleMission();

	const char* getMapFileName(){ return selMapName; }

	static void			getMapNameFromFile( const char* pFileName, char* pBuffer, long bufferLength );




private:
	int indexOfButtonWithID(int id);
	void seedDialog( bool bSeedSingle );
	void seedFromCampaign();



	aListBox				mapList;
	aLocalizedListItem	templateItem;

	// New-pipeline mirror of mapList for the ImGui defs renderer (see
	// UiDefs::GameOSPage::setListItems). Read-only mirror for now: mapList
	// remains the source of truth for selection/clicks; this just makes
	// the same items+selection visible through the new List element.
	UiDefs::GameOSPage*	mapListPage;
	void					syncMapListPage();

	EString					selMapName;

	bool					bIsSingle;



	void	updateMapInfo();
	void	seedFromFile( const char* pFileName );
	void	addFile( const char* pFileName, bool bSeedSingle );



};



//*************************************************************************************************
#endif  // end of file ( MPLoadMap.h )
