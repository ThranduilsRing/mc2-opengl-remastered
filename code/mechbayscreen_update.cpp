// MechBayScreen: the weight meter, the per-frame update and the message handler.
// Declared in the class headers; shared names in mechbayscreen_internal.h.
#include "mechbayscreen_internal.h"
#include "mc2fmt.h"


void MechBayScreen::drawWeightMeter(long xOffset, long yOffset)
{
		// there are 20 rays, each 9 degrees
	// they should be 50 pixels long
	float currentDropWeight = LogisticsData::instance->getCurrentDropWeight();
	float maxDropWeight = LogisticsData::instance->getMaxDropWeight();
	long numAddBars = 0;
	long numRemoveBars = 0;

	long addColor = 0;
	long removeColor = 0;		

	long numberOfBars = 20;
	if ( maxDropWeight != 0 )
	{
		float percent = currentDropWeight/maxDropWeight;
		percent *= 20.f;
		numberOfBars = percent + .5;
	}

	if ( addWeightAnim.isAnimating() && !addWeightAnim.isDone() )
	{
		addColor = addWeightAnim.getColor();
		float percent = (currentDropWeight - addWeightAmount) / maxDropWeight;
		percent *= 20.f;
		numAddBars = percent + .5;
		numAddBars = numberOfBars - numAddBars;
	}

	if ( removeWeightAnim.isAnimating() && !removeWeightAnim.isDone() )
	{
		addColor = removeWeightAnim.getColor();
		float percent = removeWeightAmount / maxDropWeight;
		percent *= 20.f;
		numRemoveBars = percent + .5;
	}

	gos_VERTEX v[3];
	for (auto & i : v)
	{
		i.u = 0.f;
		i.v = 0.f;
		i.rhw = .5;
		i.z = 0.f;
		i.argb = 0;
		i.frgb = 0;
		i.x = weightCenterX + xOffset;
		i.y = weightCenterY + yOffset;
	}

	// DEFS-METER-BRIDGE-1: raw gos_DrawTriangles is buried under the ImGui defs
	// page; when the caller has the GUI bridge active, draw the radial gauge
	// wedges on the ImGui layer at display scale instead.
	float bSx = 1.f, bSy = 1.f, bOx = 0.f, bOy = 0.f;
	const bool bBridged = aObject::getGuiBridgeTransform( bSx, bSy, bOx, bOy );
	 
	float curArc = 180.f;
	float curX = 50.f * cos( DEGREES_TO_RADS * curArc );
	float curY = 50.f * sin( DEGREES_TO_RADS * curArc );
	for (int i= 0; i < numberOfBars + numRemoveBars; i++ )
	{

		v[1].x = curX + weightCenterX + xOffset;
		v[1].y = curY + weightCenterY + yOffset;

		curArc += 9.f;
		curX = 50.f * cos( DEGREES_TO_RADS * curArc );
		curY = 50.f * sin( DEGREES_TO_RADS * curArc );

		v[2].x = curX + weightCenterX + xOffset;
		v[2].y = curY + weightCenterY + yOffset;

		if ( i >= numberOfBars - numAddBars )
			v[0].argb = v[1].argb = v[2].argb = addColor;
		else if ( i >= numberOfBars )
			v[0].argb = v[1].argb = v[2].argb = removeColor;
		else
			v[0].argb = v[1].argb = v[2].argb = interpolateColor( weightStartColor, weightEndColor, i * .05 );

		if ( bBridged )
			GuiRuntime::DrawUiTriangleVerts(
				v[0].x * bSx + bOx, v[0].y * bSy + bOy,
				v[1].x * bSx + bOx, v[1].y * bSy + bOy,
				v[2].x * bSx + bOx, v[2].y * bSy + bOy,
				v[0].argb );
		else
			gos_DrawTriangles( v, 3 );


	}


	dropWeightMeter.render(xOffset, yOffset);
}

void MechBayScreen::update()
{
	// MC2_BOOT_TO_MECHLAB harness: once a mech is selected in the bay (e.g. via
	// MC2_SOAK_AUTO_PURCHASE), auto-click "Modify Mech" to enter the mech lab so
	// its component palette loads/renders for capture + file-resolve tracing.
	{
		static const bool s_bootMechLab = mc2::gate::get<"MC2_BOOT_TO_MECHLAB">();
		static const bool s_autoPurchase = mc2::gate::get<"MC2_SOAK_AUTO_PURCHASE">();
		static int s_mechLabDelay = 0;
		static bool s_mechLabFired = false;
		LogisticsMech* captureMech = s_autoPurchase
			? LogisticsData::instance->getMechToModify() : pCurMech;
		if ( s_bootMechLab && !s_mechLabFired && captureMech && status == RUNNING )
		{
			if ( ++s_mechLabDelay > 30 )
			{
				s_mechLabFired = true;
				status = DOWN;
				// Auto-purchase already opened an editable copy of the bought variant.
				if ( LogisticsData::instance->getMechToModify() != captureMech )
					LogisticsData::instance->setMechToModify( captureMech );
				return;
			}
		}
	}

	mechListBox.disableItemsThatCanNotGoInFG();

	if ( !pIcons[0].getMech() )
		getButton( MB_MSG_REMOVE )->disable( true );

	// update current text
	char str[64];

	// RP
	mc2::fmt::to_buf(str, "{:d} ", mc2::fmt::as_i32(LogisticsData::instance->getCBills()));
	textObjects[1].setText( str );
	setDefsElementText( "game.mcl_mb_layout.text.cbills_text", str );

	long mouseX = userInput->getMouseX();
	long mouseY = userInput->getMouseY();
	if ( userInput->isLeftClick() && dropWeightMeter.pointInside( mouseX, mouseY ) )
	{
		soundSystem->playDigitalSample( LOG_WRONGBUTTON );
	}
	
	// current drop weight
	char tmpStr[128];
	cLoadString( IDS_MB_WEIGHT, tmpStr, 63 );
	long currentDropWeight = LogisticsData::instance->getCurrentDropWeight();
	long maxDropWeight = LogisticsData::instance->getMaxDropWeight();

	sprintf( str, tmpStr, currentDropWeight, maxDropWeight );
	textObjects[6].setText( str );
	setDefsElementText( "game.mcl_mb_layout.text.team_weight_readout", str );


	// update drag and drop
	// must be done before Icon's are updated, or we can get out of sync
	if ( pDragMech )
	{
		dragIcon.moveTo( userInput->getMouseX() - dragIcon.width() / 2, 
			userInput->getMouseY() - dragIcon.height() / 2 );

		bool bLeft = 0;
		bool bRight = 0;
		
		GUI_RECT testRect;
		testRect.left = mechListBox.globalX();
		testRect.right = mechListBox.globalX() + mechListBox.width();
		testRect.top = mechListBox.globalY();
		testRect.bottom = mechListBox.globalY() + mechListBox.height();


		if ( dragIcon.rectIntersect( testRect ) )
		{
			// set the focus
		}	bRight = true;

		testRect.left = pIcons[0].globalX();
		testRect.right = pIcons[ICON_COUNT - 1].globalX() + pIcons[0].width();
		testRect.top = pIcons[0].globalY();
		testRect.bottom = pIcons[ICON_COUNT-1].globalY() + pIcons[0].height();

		if ( dragIcon.rectIntersect( testRect ) )
		{
			bLeft = true;
		}
		else 
			bLeft = 0;

		if ( userInput->leftMouseReleased() || userInput->isLeftDoubleClick() )
		{
			if ( dragLeft )
			{
				if ( bLeft )
				{
					LogisticsData::instance->removeMechFromForceGroup( pDragMech, false );
					LogisticsData::instance->addMechToForceGroup( pDragMech, forceGroupCount+1 );
					forceGroupCount++;

					reinitMechs();
				}
				else
				{
					if ( 0 == LogisticsData::instance->getVariantsInInventory( pDragMech->getVariant(), 0 ) )
					{ 
						MechListBoxItem* item = new MechListBoxItem( pDragMech, 0 );
						int index = mechListBox.AddItem( item );
						mechListBox.SelectItem( index );
					}
					LogisticsData::instance->removeMechFromForceGroup( pDragMech, true );
					forceGroupCount--;
					reinitMechs();

					removeWeightAmount = pDragMech->getMaxWeight();
					removeWeightAnim.begin();
					addWeightAnim.end();
				}
			}
			else
			{
				if ( bLeft )
				{
					reinitMechs();
					addWeightAmount = pDragMech->getMaxWeight();
					addWeightAnim.begin();
					removeWeightAnim.end();
		
				}
				else
				{
					if ( 0 == LogisticsData::instance->getVariantsInInventory( pDragMech->getVariant(), 0 ) )
					{
						MechListBoxItem* item = new MechListBoxItem( pDragMech, 0 );
						int index = mechListBox.AddItem( item );
						mechListBox.SelectItem( index );
					}
					LogisticsData::instance->removeMechFromForceGroup( pDragMech, true );
					reinitMechs();

				}
			}
		
			pDragMech = nullptr;
		}

	}


	// update icon selection
	int newSel = -1;
	int oldSel = -1;
	int removeMech = -1;
	for ( int i = 0; i < ICON_COUNT; i++ )
	{
		pIcons[i].update();
		if ( pIcons[i].justSelected() )
			newSel = i;
		else if ( pIcons[i].isSelected() )
			oldSel = i;

		bool bInside = pIcons[i].pointInside( userInput->getMouseX(), userInput->getMouseY() );

		if ( bInside && userInput->isLeftDoubleClick() && pIcons[i].getMech() )
		{
			 removeMech = i;
		}
		else if ( userInput->isLeftDrag() 
			&& pIcons[i].pointInside( userInput->getMouseDragX(), userInput->getMouseDragY() ) )
		{
			beginDrag( pIcons[i].getMech() );
		}
	}

	if ( newSel != -1 )
	{
		if ( oldSel != -1 )
			pIcons[oldSel].select( 0 );
		mechListBox.SelectItem( -1 );
		
	}
	if ( removeMech != -1 )
	{
		 removeSelectedMech();
	}


	if ( !MPlayer || !ChatWindow::instance()->pointInside(userInput->getMouseX(), userInput->getMouseY()) )
		LogisticsScreen::update();
	
	if ( newSel == -1 && oldSel == -1 )
	{
		// disable the remove button
		getButton( MB_MSG_REMOVE )->disable(1);
	}
	else
		getButton( MB_MSG_REMOVE )->disable(0);

	
	// update list box, 
	oldSel = mechListBox.GetSelectedItem();
	mechListBox.update();
	newSel = mechListBox.GetSelectedItem( );

	// did anything change?
	if ( oldSel != newSel && newSel != -1)
	{
		unselectDeploymentTeam();
	}

	// component list box
	loadoutListBox.update();	

	addWeightAnim.update();
	removeWeightAnim.update();

	LogisticsMech* pTmp = mechListBox.getCurrentMech();
	if ( !pTmp )
	{
		pTmp = getFGSelMech();
	}

	if ( pTmp )
		setMech( pTmp, 0);

	// update buttons!
	if ( newSel == -1 || !LogisticsData::instance->canAddMechToForceGroup( mechListBox.getCurrentMech() ) )
	{
		// disable the add button
		getButton( MB_MSG_ADD )->disable(1);
	}
	else
		getButton( MB_MSG_ADD )->disable(0);

		

	if ( MPlayer && ( !MPlayer->missionSettings.variants || !pCurMech ) )
	{
		getButton( MB_MSG_CHANGE_LOADOUT )->disable( true );
	}
	else if ( LogisticsData::instance->skipPurchasing() )
	{
		getButton( MB_MSG_CHANGE_LOADOUT )->disable( true );
		getButton( MB_MSG_BUY )->disable( true );
	}
	else 
	{
		getButton( MB_MSG_CHANGE_LOADOUT )->disable( false );
		getButton( MB_MSG_BUY )->disable( false );

		if ( !pCurMech )
			getButton( MB_MSG_CHANGE_LOADOUT )->disable( true );
		else
		{
			getButton( MB_MSG_CHANGE_LOADOUT )->disable( false );
			mechCamera->update();
		}
	}





	if ( MPlayer && ChatWindow::instance() )
		ChatWindow::instance()->update();



}

int	MechBayScreen::handleMessage( unsigned long message, unsigned long who )
{
	if ( status != RUNNING )
		return 0;

	switch( who )
	{
	case MB_MSG_NEXT:
		status = NEXT;
		break;

	case MB_MSG_PREV:
		status = PREVIOUS;
		break;

	case MB_MSG_ADD:
		
		addSelectedMech();
		soundSystem->playDigitalSample( LOG_SELECT );		
		break;

	case MB_MSG_REMOVE:
		removeSelectedMech();
		break;

	case MB_MSG_BUY:
		status = UP;
		break;

	case MB_MSG_CHANGE_LOADOUT:
		status = DOWN;
		if ( pCurMech )
		{
			LogisticsData::instance->setMechToModify( pCurMech );
		}
		break;

	case MB_MSG_BUY_SELL:
		break;

	case MB_MSG_MAINMENU:
		status = MAINMENU;
		break;
	}


	return 0;
}
