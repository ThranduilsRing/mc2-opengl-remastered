// Editor: terrain tools, mission settings, team/player, texture dialogs, link and drop-zone commands.
// Declared in the class headers; shared names in EditorInterface_internal.h.
#include "EditorInterface_internal.h"
#include "mc2str.h"


int EditorInterface::NewHeightMap()
{
	CFileDialog	fileDlg( 1,  "tga", nullptr, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = terrainPath;

	bool endFlag = false;
	while (!endFlag)
	{
		endFlag = true;
		if (  IDOK == fileDlg.DoModal() )
		{
			const char* pFile = fileDlg.m_ofn.lpstrFile;
			
			if (pFile)
			{
				File tgaFile;

				gosASSERT( strstr( pFile, ".tga" ) || strstr( pFile, ".TGA" ) );

				long result = tgaFile.open(const_cast<char*>(pFile));
				gosASSERT(result == NO_ERR);

				struct TGAFileHeader theader;
				tgaFile.read((MemoryPtr)&theader,sizeof(TGAFileHeader));

				if ((theader.width != land->realVerticesMapSide) || (theader.height != land->realVerticesMapSide))
				{
					AfxMessageBox(IDS_WRONG_SIZE_HEIGHT_MAP);
					endFlag = false;
					continue;
				}
			}
			else
			{
				gosASSERT(false);
				endFlag = false;
				continue;
			}

			HeightDlg htDlg;
			long tileR, tileC;

			htDlg.SetMin( (int)land->getLowestVertex( tileR, tileC ) );
			htDlg.SetMax( (int)land->getHighestVertex( tileR, tileC ) );
			
			bool chkStatus = false;
			if ( IDOK == htDlg.DoModal() )
			{
				SetBusyMode();
				chkStatus = EditorData::reassignHeightsFromTGA( pFile, htDlg.GetMin(), htDlg.GetMax() );
				UnsetBusyMode();
			}

			if (chkStatus)
				tacMap.UpdateMap();
		}
	}

	return true;
}

constexpr int MAX_THRESHOLD = 5;
constexpr int MAX_NOISE = 5;
constexpr int MIN_THRESHOLD = 1;
constexpr int MIN_NOISE = 0;

int EditorInterface::RefractalizeTerrain(long Threshold)
{
	FractalDlg fDlg;
	fDlg.SetThreshold(Terrain::fractalThreshold);
	fDlg.SetNoise(Terrain::fractalNoise);
	
	if (IDOK == fDlg.DoModal())
	{
		SetBusyMode();
		Terrain::fractalThreshold = fDlg.GetThreshold();
		Terrain::fractalNoise = fDlg.GetNoise();

		if (Terrain::fractalThreshold > MAX_THRESHOLD)
			Terrain::fractalThreshold = MAX_THRESHOLD;
			
		if (Terrain::fractalThreshold < MIN_THRESHOLD)
			Terrain::fractalThreshold = MIN_THRESHOLD;
 			
		if (Terrain::fractalNoise > MAX_NOISE)
			Terrain::fractalNoise = MAX_NOISE;
			
		if (Terrain::fractalNoise < MIN_NOISE)
			Terrain::fractalNoise = MIN_NOISE;
			
 		if (Terrain::terrainTextures2)
			Terrain::terrainTextures2->refractalizeBaseMesh(Terrain::terrainName, Terrain::fractalThreshold, Terrain::fractalNoise);
		UnsetBusyMode();
	}
		
	return true;
}

int EditorInterface::SetSky (long skyId)
{
	EditorData::instance->TheSkyNumber(skyId);

	return true;
}

int EditorInterface::Select()
{
	if ( currentBrushID == IDS_SELECT )
	{
		land->unselectAll();
		EditorObjectMgr::instance()->unselectAll();
	}
	else
	{
		int radius = GetParent()->GetMenu()->GetMenuState( ID_DRAGNORMAL, MF_BYCOMMAND ) & MF_CHECKED? -1 : smoothRadius;
		setActiveBrush( new SelectionBrush( false, radius ), IDS_SELECT, ID_OTHER_SELECT );

		ChangeCursor( IDC_MC2ARROW );
	}

	selecting = true;

	return true;
}

int EditorInterface::Flatten()
{
	if ( selecting && ( land->hasSelection() ) )
	{
		FlattenBrush brush;
		Action* pAction = brush.applyToSelection();
		if ( pAction )
		{
			undoMgr.AddAction( pAction );
		}
	}
	else if (0 == dynamic_cast<FlattenBrush *>(curBrush))
	{
		setActiveBrush( new FlattenBrush(), IDS_FLATTEN, ID_OTHER_FLATTEN );
		ChangeCursor( IDC_FLATTEN );
	}

	return true;

}

int EditorInterface::SaveCameras()
{
	if ( !EditorData::getMapName() )
	{
		// message box that you must save here
		EMessageBox( IDS_SAVE_CAMERA_FAIL, IDS_ERROR, MB_OK );
	}
	else
	{
		char	base[256];

		strcpy( base, cameraPath );
		strcat( base, EditorData::getMapName() );
		strcat( base, "cam" );
		strcat( base, ".fit" );

		FitIniFile file;
		file.open( base );

		return eye->save( &file );

	}

	return true;
}

int EditorInterface::SelectSlopes()
{
	SelectSlopeDialog dlg;
	if ( dlg.DoModal() == IDOK )
	{
		float minAngle = dlg.m_MinEdit;
		float maxAngle = dlg.m_MaxEdit;

		float minHeight = float(fabs(float(tan( minAngle * float(PI)/180.f ) * land->worldUnitsPerVertex)));
		float maxHeight = float(fabs(float(tan( maxAngle * float(PI)/180.f ) * land->worldUnitsPerVertex)));

		float centerElv;
		float tmpElv;

		int left;
		int right;
		int top;
		int bottom;

		land->unselectAll();

		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			for ( int i = 0; i < land->realVerticesMapSide; ++i )
			{
				left = i > 0 ? i - 1 : 0;
				right = i < land->realVerticesMapSide ? i + 1 : i;
				top = j > 0 ? j - 1 : 0;
				bottom = j < land->realVerticesMapSide ? j + 1 : j;

				centerElv = land->getVertexHeight( j * land->realVerticesMapSide + i );
				tmpElv = land->getVertexHeight( top * land->realVerticesMapSide + left ); 
				if ( fabs(tmpElv - centerElv) >= minHeight && fabs(tmpElv - centerElv) <= maxHeight )
				{
					land->selectVertex( j, i );
				}
				else
				{
					tmpElv = land->getVertexHeight( top * land->realVerticesMapSide + right );
					if ( fabs(tmpElv - centerElv) >= minHeight && fabs(tmpElv - centerElv) <= maxHeight )
					{
						land->selectVertex( j, i );
					}
					else
					{
						tmpElv = land->getVertexHeight( bottom * land->realVerticesMapSide + right );
						if ( fabs(tmpElv - centerElv) >= minHeight && fabs(tmpElv - centerElv) <= maxHeight )
						{
							land->selectVertex( j, i );
						}
						else
						{
							tmpElv = land->getVertexHeight( bottom * land->realVerticesMapSide + left );
							if ( fabs(tmpElv - centerElv) >= minHeight && fabs(tmpElv - centerElv) <= maxHeight )
							{
								land->selectVertex( j, i );
							}
						}
					}
				}
			}
		}// done looping over vertices
	}

	selecting = true;
	return true;
}

int EditorInterface::SelectAltitude()
{
	HeightDlg dlg;
	if ( dlg.DoModal() == IDOK )
	{
		float minHeight = (float)dlg.GetMin();
		float maxHeight = (float)dlg.GetMax();

		float centerElv;

		land->unselectAll();

		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			for ( int i = 0; i < land->realVerticesMapSide; ++i )
			{
				centerElv = land->getVertexHeight( j * land->realVerticesMapSide + i );
				if ( (centerElv >= minHeight) && (centerElv <= maxHeight) )
				{
					land->selectVertex( j, i );
				}
			}
		}// done looping over vertices
	}

	selecting = true;
	return true;
}

int EditorInterface::SelectTerrainType()
{
	SelectTerrainTypeDlg dlg;
	dlg.SelectedTerrainType(-1);
	if (( dlg.DoModal() == IDOK ) && (ID_TERRAINS_BLUEWATER <= dlg.SelectedTerrainType()))
	{
		const int selectedTerrainType = dlg.SelectedTerrainType() - ID_TERRAINS_BLUEWATER;
		land->unselectAll();

		for ( int j = 0; j < land->realVerticesMapSide; ++j )
		{
			for ( int i = 0; i < land->realVerticesMapSide; ++i )
			{
				int tmp = land->getTerrain( j, i );
				if (land->getTerrain( j, i ) == selectedTerrainType)
				{
					land->selectVertex( j, i );
				}
			}
		}// done looping over vertices
	}

	selecting = true;
	return true;
}

int EditorInterface::PurgeTransitions()
{
	land->purgeTransitions();

	highlighted = false;
	Terrain::mapData->unhighlightAll();

	return true;
}

int EditorInterface::ShowTransitions()
{
	highlighted ^= true;
	if (highlighted)
		Terrain::mapData->highlightAllTransitionsOver2();
	else
		Terrain::mapData->unhighlightAll();

	return true;
}

int EditorInterface::Fog()
{
	FogDlg dlg;
	dlg.m_blue = (eye->dayFogColor) & 0xff;
	dlg.m_green = (eye->dayFogColor >> 8) & 0xff;
	dlg.m_red = (eye->dayFogColor >> 16) & 0xff;
	dlg.m_start = eye->fogStart;
	dlg.m_end = eye->fogFull;

	if ( IDOK == dlg.DoModal() )
	{
		eye->dayFogColor = ((DWORD)dlg.m_blue) + (((DWORD)dlg.m_green) << 8) + (((DWORD)dlg.m_red) << 16);
		eye->fogColor = eye->dayFogColor;
		eye->fogStart = dlg.m_start;
		eye->fogFull = dlg.m_end;

	}

	return true;
}

int EditorInterface::Light()
{
	SunDlg dlg;
	if ( IDOK == dlg.DoModal() )
	{
		/*
		SafeRunGameOSLogic();
		land->reCalcLight();
		SafeRunGameOSLogic();
		tacMap.UpdateMap();
		*/
	}
	return true;
}

int EditorInterface::Waves()
{
	WaterDlg dlg;

	dlg.alphaDeep = Terrain::alphaDeep;
	dlg.alphaElevation = Terrain::mapData->alphaDepth;
	dlg.alphaMiddle = Terrain::alphaMiddle;
	dlg.alphaShallow = Terrain::alphaEdge;

	dlg.amplitude = Terrain::waterAmplitude;
	dlg.elevation = Terrain::mapData->waterDepth;
	dlg.frequency = Terrain::waterFreq;
	dlg.shallowElevation = Terrain::mapData->shallowDepth;

	if ( IDOK == dlg.DoModal() )
	{
		Terrain::alphaDeep = dlg.alphaDeep;
		Terrain::alphaMiddle = dlg.alphaMiddle;
		Terrain::alphaEdge = dlg.alphaShallow;

		Terrain::waterAmplitude = dlg.amplitude;
		Terrain::waterElevation = dlg.elevation;
		Terrain::waterFreq = dlg.frequency;
		Terrain::mapData->shallowDepth = dlg.shallowElevation;
		Terrain::mapData->alphaDepth = dlg.alphaElevation;
		Terrain::mapData->waterDepth = dlg.elevation;

		land->recalcWater();

		tacMap.UpdateMap();

	}

	return true;	

}


int EditorInterface::DragSmooth()
{
	if ( IDS_SELECT == currentBrushID )
	{
		currentBrushID = -1;
		currentBrushMenuID = -1;
		GetParent()->GetMenu()->CheckMenuItem( ID_DRAGSMOOTH, MF_BYCOMMAND | MF_CHECKED );
		GetParent()->GetMenu()->CheckMenuItem( ID_DRAGNORMAL, MF_BYCOMMAND | MF_UNCHECKED );
		Select();
	}

	return true;

}
int EditorInterface::DragRough()
{
	if ( IDS_SELECT == currentBrushID )
	{
		currentBrushID = -1;
		currentBrushMenuID = -1;
		GetParent()->GetMenu()->CheckMenuItem( ID_DRAGSMOOTH, MF_BYCOMMAND | MF_UNCHECKED );
		GetParent()->GetMenu()->CheckMenuItem( ID_DRAGNORMAL, MF_BYCOMMAND | MF_CHECKED );
		Select();
	}

	return true;

}

int EditorInterface::AssignElevation()
{
	if ( !land->hasSelection() )
	{
		// nothing to assign heights to, let the user know
		char buffer[256];
		EditorSafeLoadString( IDS_NO_VERTEX_SEL, buffer, 256, gameResourceHandle );
		MessageBox( buffer );
	}
	else
	{
		FlattenBrush tmp;
		float val = tmp.getAverageHeightOfSelection( );
		SingleValueDlg dlg( IDS_ASSIGN_ELEVATION, IDS_ELEVATION, (int)val );
		dlg.SetVal( (int)val );
		if ( IDOK == dlg.DoModal() )
		{
			ActionPaintTile *pAction = new ActionPaintTile;
			for ( int  j = 0; j < land->realVerticesMapSide; ++j )
			{
				for ( int i = 0; i < land->realVerticesMapSide; ++i )
				{
					if ( land->isVertexSelected( j, i ) )
					{
						land->setVertexHeight( j * land->realVerticesMapSide + i, (float)dlg.GetVal() );
					}
				}
			}

			if ( pAction )
				undoMgr.AddAction( pAction );

			/*Designers say refreshing the tacmap takes too long. User can do it manually.*/
		}
	}


	return true;
}

int EditorInterface::SmoothRadius()
{
	SingleValueDlg dlg( IDS_SMOOTH_RADIUS, IDS_RADIUS, smoothRadius );
	if ( IDOK == dlg.DoModal() )
	{
		smoothRadius = dlg.GetVal();

		
		if ( IDS_SELECT == currentBrushID )
		{
			currentBrushID = -1;
			currentBrushMenuID = -1;
			Select();
		}
	}

	return true;
}

int EditorInterface::Alignment( int specific )
{
	
	for ( int i = 0; i < 9; ++i )
		GetParent()->GetMenu()->CheckMenuItem( ID_ALIGNMENT_TEAM1 + i, MF_BYCOMMAND | MF_UNCHECKED );
		
	
	GetParent()->GetMenu()->CheckMenuItem( specific, MF_BYCOMMAND | MF_CHECKED );
	
	if ( currentBrushID >= IDS_OBJECT_200 && currentBrushID <= 30800 )
	{
		int tmp = currentBrushID;
		currentBrushID = 0;
		paintBuildings( tmp );
	}

	return true;
}

int EditorInterface::SaveHeightMap()
{
	CreateDirectory(terrainPath, nullptr);
	
	CFileDialog	fileDlg( 0, "tga", nullptr, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = terrainPath;
	
	if (  IDOK == fileDlg.DoModal() )
	{
		const char* pFile = fileDlg.m_ofn.lpstrFile;
		
		File file;
		if ( NO_ERR != file.create( (char*)pFile ) )
		{
			EMessageBox( IDS_INVALID_FILE, IDS_CANT_SAVE, MB_OK );
			return false;
		}
		
		SetBusyMode();
		bool ret = EditorData::saveHeightMap( &file );
		UnsetBusyMode();

		return ret;
	}

	return true;

}

int EditorInterface::MissionSettings()
{
	MissionSettingsDlg dlg;
	dlg.m_MissionNameUnlocalizedText = EditorData::instance->MissionName().c_str();
	dlg.m_MissionNameUseResourceString = EditorData::instance->MissionNameUseResourceString();
	dlg.m_MissionNameResourceStringID = EditorData::instance->MissionNameResourceStringID();
	dlg.m_AuthorEdit = EditorData::instance->Author().c_str();
	dlg.m_BlurbUnlocalizedText = EditorData::instance->Blurb().c_str();
	dlg.m_BlurbUseResourceString = EditorData::instance->BlurbUseResourceString();
	dlg.m_BlurbResourceStringID = EditorData::instance->BlurbResourceStringID();
	dlg.m_Blurb2UnlocalizedText = EditorData::instance->Blurb2().c_str();
	dlg.m_Blurb2UseResourceString = EditorData::instance->Blurb2UseResourceString();
	dlg.m_Blurb2ResourceStringID = EditorData::instance->Blurb2ResourceStringID();
	dlg.m_TimeLimit = EditorData::instance->TimeLimit();
	dlg.m_DropWeightLimit = EditorData::instance->DropWeightLimit();
	dlg.m_InitialResourcePoints = EditorData::instance->InitialResourcePoints();
	dlg.m_SinglePlayerCheck = (BOOL)EditorData::instance->IsSinglePlayer();
	dlg.m_MaxTeams = EditorData::instance->MaxTeams();
	dlg.m_MaxPlayers = EditorData::instance->MaxPlayers();
	dlg.m_ScenarioTune.Empty();
	dlg.m_ScenarioTune.Format(_T("%d"), EditorData::instance->ScenarioTune());
	dlg.m_VideoFilename = EditorData::instance->VideoFilename().c_str();
	dlg.m_CBills = EditorData::instance->CBills();
	dlg.m_NumRPBuildings = EditorData::instance->NumRandomRPbuildings();
	dlg.m_DownloadUrlEdit = EditorData::instance->DownloadURL().c_str();
	dlg.m_MissionType = EditorData::instance->MissionType();
	dlg.m_AirStrikeCheck = EditorData::instance->AirStrikesEnabledDefault();
	dlg.m_MineLayerCheck = EditorData::instance->MineLayersEnabledDefault();
	dlg.m_ScoutCopterCheck = EditorData::instance->ScoutCoptersEnabledDefault();
	dlg.m_SensorProbeCheck = EditorData::instance->SensorProbesEnabledDefault();
	dlg.m_UnlimitedAmmoCheck = EditorData::instance->UnlimitedAmmoEnabledDefault();
	dlg.m_AllTech = EditorData::instance->AllTechEnabledDefault();
	dlg.m_RepairVehicleCheck = EditorData::instance->RepairVehicleEnabledDefault();
	dlg.m_SalvageCraftCheck = EditorData::instance->SalvageCraftEnabledDefault();
	dlg.m_ResourceBuildingCheck = EditorData::instance->ResourceBuildingsEnabledDefault();
	dlg.m_NoVariantsCheck = EditorData::instance->NoVariantsEnabledDefault();
	dlg.m_ArtilleryPieceCheck = EditorData::instance->ArtilleryPieceEnabledDefault();
	dlg.m_RPsForMechsCheck = EditorData::instance->RPsForMechsEnabledDefault();
	if ( IDOK == dlg.DoModal() )
	{
		EditorData::instance->MissionName(mc2::str::from_c(dlg.m_MissionNameUnlocalizedText.GetBuffer(0)));
		EditorData::instance->MissionNameUseResourceString(dlg.m_MissionNameUseResourceString);
		EditorData::instance->MissionNameResourceStringID(dlg.m_MissionNameResourceStringID);
		EditorData::instance->Author(mc2::str::from_c(dlg.m_AuthorEdit.GetBuffer(0)));
		EditorData::instance->Blurb(mc2::str::from_c(dlg.m_BlurbUnlocalizedText.GetBuffer(0)));
		EditorData::instance->BlurbUseResourceString(dlg.m_BlurbUseResourceString);
		EditorData::instance->BlurbResourceStringID(dlg.m_BlurbResourceStringID);
		EditorData::instance->Blurb2(mc2::str::from_c(dlg.m_Blurb2UnlocalizedText.GetBuffer(0)));
		EditorData::instance->Blurb2UseResourceString(dlg.m_Blurb2UseResourceString);
		EditorData::instance->Blurb2ResourceStringID(dlg.m_Blurb2ResourceStringID);
		EditorData::instance->TimeLimit(dlg.m_TimeLimit);
		EditorData::instance->DropWeightLimit(dlg.m_DropWeightLimit);
		EditorData::instance->InitialResourcePoints(dlg.m_InitialResourcePoints);
		EditorData::instance->IsSinglePlayer((bool)dlg.m_SinglePlayerCheck);
		EditorData::instance->MaxTeams(dlg.m_MaxTeams);
		EditorData::instance->MaxPlayers(dlg.m_MaxPlayers);
		EditorData::instance->ScenarioTune(atoi(dlg.m_ScenarioTune.GetBuffer(0)));
		EditorData::instance->VideoFilename(mc2::str::from_c(dlg.m_VideoFilename.GetBuffer(0)));
		EditorData::instance->CBills( dlg.m_CBills);
		EditorData::instance->NumRandomRPbuildings(dlg.m_NumRPBuildings);
		EditorData::instance->DownloadURL(mc2::str::from_c(dlg.m_DownloadUrlEdit.GetBuffer(0)));
		EditorData::instance->MissionType(dlg.m_MissionType);
		EditorData::instance->AirStrikesEnabledDefault(dlg.m_AirStrikeCheck);
		EditorData::instance->MineLayersEnabledDefault(dlg.m_MineLayerCheck);
		EditorData::instance->ScoutCoptersEnabledDefault(dlg.m_ScoutCopterCheck);
		EditorData::instance->SensorProbesEnabledDefault(dlg.m_SensorProbeCheck);
		EditorData::instance->UnlimitedAmmoEnabledDefault(dlg.m_UnlimitedAmmoCheck);
		EditorData::instance->AllTechEnabledDefault(dlg.m_AllTech);
		EditorData::instance->RepairVehicleEnabledDefault(dlg.m_RepairVehicleCheck);
		EditorData::instance->SalvageCraftEnabledDefault(dlg.m_SalvageCraftCheck);
		EditorData::instance->ResourceBuildingsEnabledDefault(dlg.m_ResourceBuildingCheck);
		EditorData::instance->NoVariantsEnabledDefault(dlg.m_NoVariantsCheck);
		EditorData::instance->ArtilleryPieceEnabledDefault(dlg.m_ArtilleryPieceCheck);
		EditorData::instance->RPsForMechsEnabledDefault(dlg.m_RPsForMechsCheck);
	}
	return true;
}

int EditorInterface::Team( int team )
{
	static CTeams originalTeams;
	if (!ObjectSelectOnlyMode()) {
		originalTeams = EditorData::instance->TeamsRef();
	}
	EditorData::instance->DoTeamDialog(team);
	if (!ObjectSelectOnlyMode()) {
		if (!(originalTeams == EditorData::instance->TeamsRef())) {
			TeamsAction *pTeamsAction = new TeamsAction(originalTeams);
			undoMgr.AddAction(pTeamsAction);
		}
		originalTeams.Clear();
	}
	return true;
}

int EditorInterface::Player( int player )
{
	PlayerSettingsDlg dlg;
	dlg.m_playerEdit = player + 1;
	dlg.m_oldDefaultTeam = EditorData::instance->PlayersRef().PlayerRef(player).DefaultTeam();
	dlg.m_numTeams = EditorData::instance->MaxTeams();
	if ((IDOK == dlg.DoModal()) && (0 <= dlg.m_newDefaultTeam))
	{
		EditorData::instance->PlayersRef().PlayerRef(player).DefaultTeam(dlg.m_newDefaultTeam);
	}
	return true;
}


int EditorInterface::Damage( bool bDamage )
{
	if ( selecting && EditorObjectMgr::instance()->hasSelection() )
	{
		DamageBrush tmp( bDamage );
		Action* pRetAction = tmp.applyToSelection( );
		if ( pRetAction )
			undoMgr.AddAction( pRetAction );
	}
	else
	{
		DamageBrush *pCurDamageBrush = dynamic_cast<DamageBrush *>(curBrush);
		if ((0 == pCurDamageBrush) || (pCurDamageBrush->damage != bDamage))
		{
			// Toolbar entry is the base ID_OTHER_DAMAGE; brushId keeps the +bDamage
			// variant so range-tests are unchanged, menuId is the toolbar base so the
			// highlight matches for both damage and repair variants.
			setActiveBrush( new DamageBrush( bDamage ), ID_OTHER_DAMAGE + bDamage, ID_OTHER_DAMAGE );
			ChangeCursor( IDC_HAMMER + !bDamage );
		}
	}

	return true;
}

int EditorInterface::LayMines()
{
	if ( selecting && ( land->hasSelection() ) )
	{
		MineBrush	brush;
		Action* pRetAction = brush.applyToSelection( );
		if ( pRetAction )
			undoMgr.AddAction( pRetAction );
	}
	else if (0 == dynamic_cast<MineBrush *>(curBrush))
	{
		setActiveBrush( new MineBrush(), ID_OTHER_LAYMINES, ID_OTHER_LAYMINES );
		ChangeCursor( IDC_HAMMER ); // need a minebrush cursor
	}
	return true;
}

int EditorInterface::SelectDetailTexture()
{
	CFileDialog fileDlg( TRUE,  "tga", nullptr, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = texturePath;

	if (  IDOK == fileDlg.DoModal() )
	{
		CString path = fileDlg.m_ofn.lpstrFile;
		path.MakeLower();
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			land->terrainTextures2->resetDetailTexture(path.GetBuffer(0));
			EditorData::instance->DetailTextureNeedsSaving(true);
		}
	}
	return true;
}

int EditorInterface::SelectWaterTexture()
{
	CFileDialog fileDlg( TRUE,  "tga", nullptr, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = texturePath;

	if (  IDOK == fileDlg.DoModal() )
	{
		CString path = fileDlg.m_ofn.lpstrFile;
		path.MakeLower();
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			land->terrainTextures2->resetWaterTexture(path.GetBuffer(0));
			EditorData::instance->WaterTextureNeedsSaving(true);
		}
	}
	return true;
}

inline bool colorMapIsOKFormat (const char *fileName)
{
	DWORD localColorMapSizeCheck = land->realVerticesMapSide * 12.8;

	File tgaFile;
	long result = tgaFile.open(fileName);
	if (result == NO_ERR)
	{
		struct TGAFileHeader tgaHeader;
		tgaFile.read((MemoryPtr)&tgaHeader,sizeof(TGAFileHeader));
		if ((tgaHeader.image_type == UNC_TRUE) &&
			(tgaHeader.width == tgaHeader.height) &&
			(tgaHeader.width == localColorMapSizeCheck))
			return true;

		tgaFile.close();
	}

	return false;
}

int EditorInterface::SetBaseTexture()
{
	CFileDialog fileDlg( TRUE,  "tga", nullptr, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = texturePath;

	if (  IDOK == fileDlg.DoModal() )
	{
		CString path = fileDlg.m_ofn.lpstrFile;
		path.MakeLower();

		//Check that this is a valid colormap of EXACTLY the same size!!!
		if (colorMapIsOKFormat(path))
		{
			char name[1024];
			_splitpath(path,nullptr,nullptr,name,nullptr);

			char *testLoc = strstr(name,".burnin");
			if (testLoc)
				testLoc[0] = 0;	//Prune off the burnin name.

			land->setColorMapName(name);

			if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
			{
				if (land->colorMapName)
					land->terrainTextures2->resetBaseTexture(land->colorMapName);
				else
					land->terrainTextures2->resetBaseTexture(land->terrainName);
			}
		}
		else
		{
			EMessageBox(IDS_INVALID_COLOR_MAP,IDS_ERROR,MB_OK);
		}
	}

	return true;
}

int EditorInterface::ReloadBaseTexture()
{
	if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
	{
		if (land->colorMapName)
			land->terrainTextures2->resetBaseTexture(land->colorMapName);
		else
			land->terrainTextures2->resetBaseTexture(land->terrainName);
	}

	return true;
}

int EditorInterface::SelectWaterDetailTexture()
{
	CFileDialog fileDlg( TRUE,  "tga", nullptr, OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR, szTGAFilter );
	fileDlg.m_ofn.lpstrInitialDir = texturePath;

	if (  IDOK == fileDlg.DoModal() )
	{
		CString path = fileDlg.m_ofn.lpstrFile;
		path.MakeLower();
		if ( land->terrainTextures2  && (land->terrainTextures2->colorMapStarted))
		{
			land->terrainTextures2->resetWaterDetailTextures(path.GetBuffer(0));
			EditorData::instance->WaterDetailTextureNeedsSaving(true);
		}
	}
	return true;
}

int EditorInterface::TextureTilingFactors()
{
	TilingFactorsDialog tilingFactorsDlg;
	tilingFactorsDlg.m_TerrainDetailTilingFactor = land->terrainTextures2->getDetailTilingFactor();
	tilingFactorsDlg.m_WaterTilingFactor = land->terrainTextures2->getWaterTextureTilingFactor();
	tilingFactorsDlg.m_WaterDetailTilingFactor = land->terrainTextures2->getWaterDetailTilingFactor();
	if (IDOK == tilingFactorsDlg.DoModal())
	{
		land->terrainTextures2->setDetailTilingFactor(tilingFactorsDlg.m_TerrainDetailTilingFactor);
		land->terrainTextures2->setWaterTextureTilingFactor(tilingFactorsDlg.m_WaterTilingFactor);
		land->terrainTextures2->setWaterDetailTilingFactor(tilingFactorsDlg.m_WaterDetailTilingFactor);
	}
	return true;
}

int EditorInterface::Link( bool bLink )
{
	LinkBrush *pCurLinkBrush = dynamic_cast<LinkBrush *>(curBrush);
	if ((0 == pCurLinkBrush) || (pCurLinkBrush->bLink != bLink))
	{
		// Toolbar entry is the base ID_OTHER_LINK; brushId keeps the +bLink variant.
		setActiveBrush( new LinkBrush( bLink ), ID_OTHER_LINK + bLink, ID_OTHER_LINK );
		ChangeCursor( IDC_LINK + !bLink );
	}

	return 1;
}

int EditorInterface::DropZone( bool bVTol )
{
	int alignment = EDITOR_TEAM1;
	
	/*for ( int j = 0; j < 9; ++j )
	{
		if ( GetParent()->GetMenu()->GetMenuState( ID_ALIGNMENT_TEAM1 + j, MF_BYCOMMAND ) & MF_CHECKED )
		{
			if (j != 8)
				alignment = EDITOR_TEAM1 + j;
			else
				alignment = EDITOR_TEAMNONE;
		}
	}*/
	
	// No toolbar entry -> menuId = brushId. Highlight never matches it.
	setActiveBrush( new DropZoneBrush( alignment, bVTol ), ID_DROPZONES_ADD + bVTol, ID_DROPZONES_ADD + bVTol );
	ChangeCursor( IDC_DROPZONE );

	return true;

}
