/*************************************************************************************************\
MechBayScreen.cpp			: Implementation of the MechBayScreen component.
//---------------------------------------------------------------------------//
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
//===========================================================================//
\*************************************************************************************************/

#include "mechbayscreen_internal.h"


MechBayScreen* MechBayScreen::s_instance = nullptr;



MechBayScreen::MechBayScreen() : mechListBox( 1, 0 )
{
	pCurMech = nullptr;
	pIcons = nullptr;
	status = LogisticsScreen::RUNNING;
	s_instance = this;
	forceGroupCount = 0;
	mechCamera = std::make_unique_for_overwrite<SimpleCamera>();

	addWeightAmount = 0;
	removeWeightAmount = 0;
	pDragMech = nullptr;
	dragLeft = 0;

	helpTextArrayID = 13;

}

MechBayScreen::~MechBayScreen()
{
	if ( mechCamera )
		mechCamera.reset();

	if ( pIcons )
		pIcons.reset();

	mechListBox.destroy();
}
void MechBayScreen::init(FitIniFile* file)
{
	// init button, texts, statics, rects
	LogisticsScreen::init( *file, "MechBayStatic", "MechBayTextEntry", "MechBayRect", "MechBayButton" );
	defsHelpTextKey = "game.mcl_mb_layout.text.help_text";

	// initialize the list box
	mechListBox.init();
	mechListBox.setHelpID( IDS_HELP_MECHSTORAGE );

	loadoutListBox.init( rects[3].left(), rects[3].top(), rects[3].width(), rects[3].height() );

	// fill the mech list box
	((aObject*)&mechListBox)->init( rects[0].left(), rects[0].top(), rects[0].width(), rects[0].height() );

	// initialize little icons
	FitIniFile iconFile;
	char path[256];
	strcpy( path, artPath );
	strcat( path, "mcl_gn_deploymentteams.fit" );
	S_strlwr( path );
	if ( NO_ERR != iconFile.open( path ) )
	{
		gosASSERT( "couldn't open the MCL_GN_DevploymentTeams file" );
		return;
	}
	LogisticsMechIcon::init(iconFile);
	pIcons = std::make_unique_for_overwrite<LogisticsMechIcon[]>(ICON_COUNT);

	int count = 0;

	
	long x = 0;
	long y = 0;

	for ( int j = 0; j < ICON_COUNT_Y; j++ )
	{
        int i = 0;
		for (; i < ICON_COUNT_X; i++ )
		{
			pIcons[count].setMech( nullptr );
			pIcons[count].move( x, y );
			x+= pIcons[count].width();
			x+=4;

			count++;
		}

		y+= pIcons[i].height();
		y += 5;
		x = 0;
	}
	
	mechListBox.drawCBills( 0 );
	mechListBox.setScrollBarOrange();


	// initialize the attribute meeters
	attributeMeters[0].init( file, "AttributeMeter0" );
	attributeMeters[1].init( file, "AttributeMeter1" );
	attributeMeters[2].init( file, "AttributeMeter2" );


	dropWeightMeter.init( file, "DropWeightMeter" );
	file->readIdLong( "RayCenterX", weightCenterX );
	file->readIdLong( "RayCenterY", weightCenterY );
	file->readIdLong( "RayStartColor ", weightStartColor );
	file->readIdLong( "RayEndColor ", weightEndColor );

	file->seekBlock( "weightAddAnimation" );
	addWeightAnim.init( file, "" );

	file->seekBlock( "weightRemoveAnimation" );
	removeWeightAnim.init( file, "" );

	file->seekBlock( "FRectAnim" );

	mechCamera->init( rects[1].left(), rects[1].top(), rects[2].right(), rects[1].bottom() );

	
	
	for ( int i= 0; i < buttonCount; i++ )
		buttons[i].setMessageOnRelease();


	textObjects[7].setText( "" );
	textObjects[8].setText( "" );
	textObjects[9].setText( "" );
	textObjects[10].setText( "" );
	textObjects[11].setText( "" );
	textObjects[12].setText( "" );
	textObjects[14].setText( "" );
	for (int i = 0; i < 3; i++ )
	{
		attributeMeters[i].setValue(0);
	}
	setMech( nullptr, 0 );

}

void MechBayScreen::begin()
{
	status = RUNNING;

	pDragMech = nullptr;

	mechListBox.removeAllItems( true );
	reinitMechs();	

	int mechCount[256];
	memset( mechCount, 0, sizeof ( int ) * 256 );

	bool bCurMechIsValid = 0;

	// initialize both the inventory and icon lists
	auto mechList = LogisticsData::instance->getInventory();
	
	for ( auto iter = mechList.begin(); iter != mechList.end(); ++iter )
		{
			if ( *iter == pCurMech )
				bCurMechIsValid = true;

			if ( !(*iter)->getForceGroup() )
			{
				bool bFound = 0;

				for ( int i = 0; i < mechListBox.GetItemCount(); i++ )
				{
					if ( ((MechListBoxItem*)mechListBox.GetItem(i))->getMech()->getVariant()
						== (*iter)->getVariant() )
						bFound = true;
				}

				if ( !bFound )
				{
					MechListBoxItem* item = new MechListBoxItem( (*iter), 1 );
					mechListBox.AddItem( item );
				}

			}
		}

	
	
	// reset the old mech to NULL to make sure everything gets set
	
	mechListBox.drawCBills( 0 );
	mechListBox.setOrange( true );

	if ( !pCurMech || !bCurMechIsValid )
	{
		if ( !selectFirstFGItem() )
		{
			if ( !selectFirstViableLBMech() )
				setMech( nullptr );
		}
	}
	else
	{
		LogisticsMech* pMech = pCurMech;
		setMech( nullptr ); // need to make sure the guy is still running
		setMech( pMech );

		bool bFound = 0;
		for ( int i = 0; i < mechListBox.GetItemCount(); i++ )
		{
			MechListBoxItem* pItem = (MechListBoxItem*)mechListBox.GetItem( i );
			if ( pItem && pItem->getMech() == pCurMech )
			{
				mechListBox.SelectItem( i );
				bFound = true;
			}
		}

		if ( !bFound )
		{
			for ( int i = 0; i < ICON_COUNT; i++ )
			{
				if ( pIcons[i].getMech() == pMech )
				{
					pIcons[i].select( true );
					break;
				}
			}
		}
	}
	
}
void MechBayScreen::render(int xOffset, int yOffset)
{
	// Legacy->display scale for the text bridge (crisp TTF labels on the mech-
	// storage list + deployment icons, matching the data/defs UI layer).
	// UI-ASPECT-ANCHOR-1: canvas-aware transform (scale + pad origin) so
	// bridged icons/text and preview composites line up with the defs page
	// at every aspect; full-stretch fallback when no canvas is active.
	float tbSx = 1.f, tbSy = 1.f, tbOx = 0.f, tbOy = 0.f;
	aObject::getCanvasTransform( tbSx, tbSy, tbOx, tbOy );

	// DEFS-LIST-ICONS-1: full GUI bridge (not just text). With only the text
	// bridge active the row TEXT reached the ImGui layer but every quad in the
	// MechListBoxItem widget tree — the mech silhouette icons, cost icon, row
	// outline/divider, scrollbar — went through raw gos_DrawQuads and was buried
	// under the ImGui defs page (text-only 'MECH STORAGE rows). Same fix as
	// MECH-ICON-BLANK-1 below for the deployment-slot icons. aListBox::render
	// also clips bridged edge rows to the box (DEFS-LIST-CLIP-1).
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	aObject::beginTextBridge( tbSx, tbSy, 1.0f, tbOx, tbOy );
	mechListBox.move( xOffset, yOffset );
	mechListBox.render();
	mechListBox.move( -xOffset, -yOffset );
	aObject::endTextBridge();
	aObject::endGuiBridge();

	loadoutListBox.move( xOffset, yOffset );
	loadoutListBox.render();
	loadoutListBox.move( -xOffset, -yOffset );

	if ( !xOffset && !yOffset && pCurMech)
	{
		// PREVIEW-FBO-FIXED-800x600-1: Mech Bay has no defs/ImGui Gui3DView panel
		// (no hasDefsUiPage() branch, no such block in mcl_mb_layout.fit) -- but
		// GuiRuntime::Render() still composites every frame regardless, so we can
		// draw the preview via the same offscreen-FBO + ImGui-image technique used
		// for Mechlopedia, just computing the real-screen destination rect
		// ourselves (mechCamera->bounds[] is the panel rect in raw, unscaled
		// 800x600 legacy space -- mechbayscreen.cpp's mechCamera->init() call,
		// unlike mechlopedia.cpp's defs branch, never multiplies it by sx/sy) by
		// scaling with the SAME tbSx/tbSy ratio already used for the text bridge
		// above.
		mechCamera->setPreviewOffscreen( true );
		mechCamera->render();
		mechCamera->drawPreviewToPanel(
			mechCamera->bounds[0] * tbSx + tbOx, mechCamera->bounds[1] * tbSy + tbOy,
			(mechCamera->bounds[2] - mechCamera->bounds[0]) * tbSx,
			(mechCamera->bounds[3] - mechCamera->bounds[1]) * tbSy );
	}



	// DEFS-METER-BRIDGE-1: bridge the stat bars + the TEAM WEIGHT LIMIT gauge
	// onto the ImGui layer — their raw gos draws were buried under the defs page
	// (empty weight-limit box / empty bars).
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	for ( int i = 0; i < 3; i++ )
	{
		attributeMeters[i].render(xOffset, yOffset);
	}



	drawWeightMeter(xOffset, yOffset);
	aObject::endGuiBridge();

	LogisticsScreen::render(xOffset, yOffset);

	// MECH-ICON-BLANK-1: full GUI bridge, not just the text bridge. With the
	// data/defs replacement page active, legacy gos_DrawQuads output is buried
	// under the ImGui page — the deployment-slot icon/pilot/connector QUADS
	// were drawn but invisible while the bridged TEXT (names) showed. Routing
	// the whole icon widget tree through the gui bridge draws it on the ImGui
	// HUD layer at display scale, same as the names.
	aObject::beginGuiBridge( tbSx, tbSy, tbOx, tbOy );
	aObject::beginTextBridge( tbSx, tbSy, 1.0f, tbOx, tbOy );
	for (int i = 0; i < ICON_COUNT; i++ )
	{
		pIcons[i].render( xOffset, yOffset );
	}
	aObject::endTextBridge();
	aObject::endGuiBridge();

	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->render(xOffset, yOffset);




	if ( pDragMech )
		dragIcon.render();


}
