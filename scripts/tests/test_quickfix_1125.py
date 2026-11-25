"""Offline wiring checks for speech exclusion and presentation-only fixes."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_briefing_excludes_tutorial_and_other_speech():
    sound = (ROOT / "mclib/soundsys.cpp").read_text()
    # Objective speech is queued; the queue drain (updateDigitalStreams) holds the exclusion guards.
    for name in ("updateDigitalStreams", "playBettySample", "playSupportSample"):
        body = re.split(rf"\n\S[^\n]*SoundSystem::{name}\s*\(", sound, maxsplit=1)[1].split("//---------------------------------------------------------------------------", 1)[0]
        assert "isPlayingVoiceOver()" in body
        assert "isChannelPlaying(RADIO_CHANNEL)" in body
    video = sound.split("bool SoundSystem::beginVideoPCMStream", 1)[1].split("void SoundSystem::pushVideo", 1)[0]
    assert "RADIO_CHANNEL, BETTY_CHANNEL, SUPPORT_CHANNEL, STREAM3CHANNEL" in video
    assert "gosAudio_SetChannelPlayMode(channel, gosAudio_Stop)" in video
    radio = (ROOT / "code/gamesound.cpp").read_text()
    assert "wholeMsgDone && !speechBusy" in radio
    assert "isPlayingVoiceOver() || isChannelPlaying(BETTY_CHANNEL)" in radio


def test_sensor_markers_use_existing_water_safe_hud_path():
    for file, shapes in (("gvactor.cpp", ("sensorTriangleShape", "sensorCircleShape")),
                         ("mech3d_init.cpp", ("sensorTriangleShape", "sensorSquareShape"))):
        source = (ROOT / "mclib" / file).read_text()
        for shape in shapes:
            assert f"{shape}->SetIsHudElement();" in source


def test_missile_terrain_floor_and_sink_do_not_write_sim_positions():
    bolt = (ROOT / "code/weaponbolt_update.cpp").read_text().split("void WeaponBolt::updatePresentation()", 1)[1]
    assert "TerrainRuntime::groundElevation(visualPosition) + 1.0f" in bolt
    assert "missilePresentationPosition(laserPosition" in bolt
    assert "setTargetPosition(" not in bolt
    assert "laserPosition =" not in bolt
    vehicle = (ROOT / "code/gvehicl_update.cpp").read_text().split("void GroundVehicle::applyAppearancePush", 1)[1]
    assert "position.z =" not in vehicle
    presentation = (ROOT / "code/objmgr_misc.cpp").read_text()
    assert "pos.z = std::min(pos.z, Terrain::waterElevation) - vehicles[i]->visualSinkDepth" in presentation
