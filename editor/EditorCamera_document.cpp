#include "EditorInterface_internal.h"

extern float elevationAdjustFactor, zoomMax, zoomMin, FOVMax, FOVMin;

EditorCamera::MissionView EditorCamera::currentMissionView() const
{
    return {newScaleFactor, zoomMax};
}

bool EditorCamera::readMissionView(FitIniFile& file)
{
    MissionView authored;
    if (file.seekBlock("Cameras") != NO_ERR
        || !EditorFit::floatValue(file, "NewScale", authored.scale)
        || !EditorFit::floatValue(file, "ZoomMax", authored.zoomMaximum)
        || authored.scale <= 0 || authored.zoomMaximum <= 0)
        return EditorFit::reject("mission camera scale/zoom");
    missionView = authored;
    return true;
}

// The mission camera writer consumes authored zoom, independently of viewport
// zoom limits. Other camera controls retain their existing authoring surfaces.
bool EditorCamera::saveMissionView(FitIniFile* file)
{
	const auto authored = missionView.value_or(currentMissionView());
	file->writeBlock("Cameras");
	file->writeIdFloat( "ProjectionAngle", projectionAngle );
	file->writeIdFloat( "PositionX", position.x );
	file->writeIdFloat( "PositionY", position.y );
	file->writeIdFloat( "PositionZ", 0.0 );
	file->writeIdBoolean( "Ready", true );

	file->writeIdUChar( "LightRed", lightRed );
	file->writeIdUChar( "LightGreen", lightGreen );
	file->writeIdUChar( "LightBlue", lightBlue );
	file->writeIdUChar( "AmbientBlue", ambientBlue );
	file->writeIdUChar( "AmbientGreen", ambientGreen );
	file->writeIdUChar( "AmbientRed", ambientRed );
	if (terrainShadowColorEnabled)
	{
		file->writeIdUChar( "TerrainShadowColorEnabled", 1 );
	}
	else
	{
		file->writeIdUChar( "TerrainShadowColorEnabled", 0 );
	}
	file->writeIdUChar( "TerrainShadowBlue", terrainShadowBlue );
	file->writeIdUChar( "TerrainShadowGreen", terrainShadowGreen );
	file->writeIdUChar( "TerrainShadowRed", terrainShadowRed );

	
	file->writeIdUChar( "DayLightRed", dayLightRed );
	file->writeIdUChar( "DayLightGreen", dayLightGreen );
	file->writeIdUChar( "DayLightBlue", dayLightBlue );
	file->writeIdUChar( "DayAmbientBlue", dayAmbientBlue );
	file->writeIdUChar( "DayAmbientGreen", dayAmbientGreen );
	file->writeIdUChar( "DayAmbientRed", dayAmbientRed );
	file->writeIdUChar( "SunsetLightRed", sunsetLightRed );
	file->writeIdUChar( "SunsetLightGreen", sunsetLightGreen );
	file->writeIdUChar( "SunsetLightBlue", sunsetLightBlue );
	file->writeIdUChar( "NightLightRed", nightLightRed );
	file->writeIdUChar( "NightLightGreen", nightLightGreen );
	file->writeIdUChar( "NightLightBlue", nightLightBlue );
	file->writeIdUChar( "NightAmbientBlue", nightAmbientBlue );
	file->writeIdUChar( "NightAmbientGreen", nightAmbientGreen );
	file->writeIdUChar( "NightAmbientRed", nightAmbientRed );

	file->writeIdUChar( "SeenBlue", seenBlue );
	file->writeIdUChar( "SeenGreen", seenGreen );
	file->writeIdUChar( "SeenRed", seenRed );
	file->writeIdUChar( "BaseBlue", baseBlue );
	file->writeIdUChar( "BaseGreen", baseGreen );
	file->writeIdUChar( "BaseRed", baseRed );
	
	file->writeIdFloat( "LightDirPitch", lightPitch );
	file->writeIdFloat("DayLightPitch",dayLightPitch);

	file->writeIdFloat("DayToNightTime",day2NightTransitionTime);
	file->writeIdFloat( "LightDirYaw", lightYaw );
	
	file->writeIdFloat( "NewScale", authored.scale );
	file->writeIdFloat( "StartRotation", getCameraRotation() );
	file->writeIdFloatArray( "LODScales", zoomLevelLODScale, 3 );
	file->writeIdFloat( "ElevationAdjustFactor", elevationAdjustFactor );
	file->writeIdFloat( "ZoomMax", authored.zoomMaximum );
	file->writeIdFloat( "ZoomMin", zoomMin );
	file->writeIdFloat( "FOVMax",FOVMax);
	file->writeIdFloat( "FOVMin",FOVMin);
	file->writeIdFloat( "FogStart", fogStart );
	file->writeIdFloat( "FogFull", fogFull );
	file->writeIdULong( "FogColor", dayFogColor );
	file->writeIdFloat( "FogTransparency", fogTransparency );
	long userMin, userMax;
	int baseTerrain;
	land->getUserSettings( userMin, userMax, baseTerrain );
	file->writeIdLong( "UserMin", userMin );
	file->writeIdLong( "UserMax", userMax );
	file->writeIdLong( "BaseTerrain", baseTerrain );

	return true;

}

bool EditorReadMissionCamera(FitIniFile& file)
{
    return eye && static_cast<EditorCamera*>(eye)->readMissionView(file);
}
bool EditorSaveMissionCamera(FitIniFile& file)
{
    return eye && static_cast<EditorCamera*>(eye)->saveMissionView(&file);
}
void EditorResetMissionCamera()
{
    if (eye) static_cast<EditorCamera*>(eye)->missionView.reset();
}

