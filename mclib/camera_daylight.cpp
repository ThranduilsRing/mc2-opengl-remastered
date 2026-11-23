// Camera: daylight fraction and sun pitch and the (uncalled) per-frame light update; the colour maths is lighting_legacy.h.
// Declared in the class headers; shared names in camera_internal.h.
#include <algorithm>

#include "camera_internal.h"
#include "lighting_legacy.h"
#include "mc2fmt.h"


//---------------------------------------------------------------------------
// GAMEPLAY-DRIVER-2: runtime sun control for the MC2_UI_COMMAND_FILE channel.
// See the contract comment on the declarations in camera.h.
float Camera::setDaylightFraction (float f)
{
	// Wrap rather than clamp: 0.25 == noon, 0.5 == sunset, 0.75 == midnight.
	f = f - floorf(f);
	if (todCurve)
	{
		// T3 curves author sun pitch independently of time: f is their normalized t,
		// not an angle to invert. Noon at 0.25 applies only to the legacy sweep below.
		todT = std::clamp(f, todCurve->keys.front().t, todCurve->keys.back().t);
		lightMods.sunPitch.reset(); lightMods.sunYaw.reset();
		publishLight();
		return lightPitch;
	}

	// updateDaylight()'s entire body is inside `if (day2NightTransitionTime >
	// 0.0f)`, and missions without a DayToNightTime park it at -1 (camera.cpp
	// readIdFloat fallback). Seed a nominal transition so the recompute runs.
	if (day2NightTransitionTime <= 0.0f)
		day2NightTransitionTime = 3600.0f/*seconds*/;

	dayLightTime = f * day2NightTransitionTime;

	// The geometric SUN altitude this fraction corresponds to. updateDaylight()
	// derives the same value internally, then -- on its night branch -- REPLACES
	// lightPitch with the MOON's pitch (`moonPitch = 180 - |180 - lightPitch|`).
	// That is fine for updateDaylight's own colour maths, but Camera::update()
	// re-derives isNight / nightFactor / lightDirection from lightPitch EVERY
	// frame and assumes it is the SUN. A midnight moon at zenith therefore
	// leaves lightPitch == 90, which update() reads back one frame later as high
	// noon -- silently undoing the night verdict. Measured before this line
	// existed: `time_of_day 0.75` acked isNight=1 and the very next
	// `light_state` read isNight=0. So restore the sun angle after the colour
	// recompute and let the per-frame derivation agree with the fraction asked
	// for. Night is then produced the same way a mission that SHIPS as night
	// produces it: a low/negative sun altitude, not a pitch past 180.
	const float sunPitch = f * 360.0f;
	// With a time-of-day curve the curve owns the colours: this verb only overrides the sun angle (a modulator); the legacy colour
	// recompute would write members the curve then overrides.
	if (!todCurve)
		updateDaylight(false);        // ambient / sun / sky / fog for this time
	setSunPitch(sunPitch);            // ... but hand update() the SUN angle
	return lightPitch;
}

float Camera::setSunPitch (float pitchDeg, bool setYaw, float yawDeg)
{
	if (todCurve)
	{
		lightMods.sunPitch = pitchDeg;
		if (setYaw) lightMods.sunYaw = yawDeg;
		publishLight();
		return lightPitch;
	}
	while (pitchDeg < 0.0f)     pitchDeg += 360.0f;
	while (pitchDeg >= 360.0f)  pitchDeg -= 360.0f;
	lightPitch = pitchDeg;
	if (setYaw)
		lightYaw = yawDeg;

	// Camera::update() recomputes lightDirection + isNight + nightFactor from
	// lightPitch every frame, but the GameOS world light's TRANSFORM is only
	// rebuilt where updateDaylight() and init do it -- so re-sync it here, or the
	// directional light keeps pointing at the old sun while every other consumer
	// has moved. Same expression as updateDaylight / Camera::init.
	if (worldLights && worldLights[0]) {
		glm::mat4 lightToWorldMatrix;
		mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0f, 0.0f, 0.0f));
		mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(
			lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));
		worldLights[0]->SetLightToWorld(&lightToWorldMatrix);
	}
	return lightPitch;
}

void Camera::updateDaylight (bool bInitialize)
{
	float day2NightTransitionTime = this->day2NightTransitionTime;
	if (bInitialize) {
		if (day2NightTransitionTime <= 0.0f) {
			day2NightTransitionTime = 3600.0f/*seconds*/;
		}
		dayLightTime = dayLightPitch / 360.0f/*degrees*/ * day2NightTransitionTime;
	}

	if (day2NightTransitionTime > 0.0f)
	{
		while (dayLightTime > day2NightTransitionTime) {
			dayLightTime -= day2NightTransitionTime;
		}
		
		float oneOverDay2NightTransitionTime = 1.0f / day2NightTransitionTime;

		lightPitch = dayLightTime * oneOverDay2NightTransitionTime * 360.0/*degrees*/;

		// The colour maths lives in lighting_legacy.h (the legacy curve generator); this is its only per-frame-capable caller.
		mc2::lighting::legacy::Params params;
		params.day[0] = dayLightRed; params.day[1] = dayLightGreen; params.day[2] = dayLightBlue;
		params.sunset[0] = sunsetLightRed; params.sunset[1] = sunsetLightGreen; params.sunset[2] = sunsetLightBlue;
		params.night[0] = nightLightRed; params.night[1] = nightLightGreen; params.night[2] = nightLightBlue;
		params.dayAmb[0] = dayAmbientRed; params.dayAmb[1] = dayAmbientGreen; params.dayAmb[2] = dayAmbientBlue;
		params.nightAmb[0] = nightAmbientRed; params.nightAmb[1] = nightAmbientGreen; params.nightAmb[2] = nightAmbientBlue;
		params.dayFogColor = dayFogColor;
		params.fogTransparency = fogTransparency;
		const auto sampled = mc2::lighting::legacy::sample(params, lightPitch, lightYaw, static_cast<float>(getCameraRotation()));
		lightPitch = sampled.pitch;
		isNight = sampled.isNight;
		nightFactor = sampled.nightFactor;
		ambientRed = sampled.amb[0]; ambientGreen = sampled.amb[1]; ambientBlue = sampled.amb[2];
		lightRed = sampled.sun[0]; lightGreen = sampled.sun[1]; lightBlue = sampled.sun[2];

		glm::mat4 lightToWorldMatrix;
		mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0f,0.0f,0.0f));
		mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));
		worldLights[0]->SetLightToWorld(&lightToWorldMatrix);

		fogColor = sampled.fogColor;

		dayLightTime += frameLength;
	}
}

//---------------------------------------------------------------------------
void Camera::updateLetterboxAndFade (void)
{
	//OK, if we are inMovieMode and we haven't finished letterboxing, finish it!
	if (!startEnding && inMovieMode && (letterBoxTime != MaxLetterBoxTime))
	{
		letterBoxTime += frameLength;
		float letterBoxPercent = letterBoxTime / MaxLetterBoxTime;
		if (letterBoxTime > MaxLetterBoxTime)
		{
			letterBoxPercent = 1.0f;
			letterBoxTime = MaxLetterBoxTime;
		}
		
		letterBoxPos = letterBoxPercent * MaxLetterBoxPos;
		letterBoxAlpha = (letterBoxPercent * 255.0f);
	}
	
	//Now, if we are in startEnding and we haven't finished DE-Letterboxing, finish it.
	if (startEnding && inMovieMode && (letterBoxPos != 0.0f))
	{
		letterBoxTime -= frameLength;
		float letterBoxPercent = letterBoxTime / MaxLetterBoxTime;
		//sebi: if letterBoxTime = 0 then letterBoxPercent also = 0 and then letterBoxPos = 0
		// see (*) and that means that on next iteration this condition will be false
		// but inMovieMode and startEnding will not be updated, causeing infinit stay in movie mode 
		if (letterBoxTime <= 0.0f)
		//if (letterBoxTime < 0.0f)
		{
			letterBoxPercent = 0.0f;
			inMovieMode = false;
			startEnding = false;
		}
		
		letterBoxPos = letterBoxPercent * MaxLetterBoxPos; // (*)
		letterBoxAlpha = (letterBoxPercent * 255.0f);
	}
	
	//If we are fading toward a color, continue the fade.
	// We only fades during movie Mode at present!!
	if (inMovieMode && inFadeMode)
	{
		timeLeftToFade += frameLength;
		float fadePercent = timeLeftToFade / startFadeTime;
		if (timeLeftToFade > startFadeTime)
		{
			fadePercent = 1.0f;
			inFadeMode = false;
		}
		
		fadeAlpha = (fadeStart >> 24) + (fadePercent * (static_cast<float>(fadeColor >> 24) - static_cast<float>(fadeStart >> 24)));
	}
}

//---------------------------------------------------------------------------
// T1.15 [SPOT_DIAG v1] file-scope state for the camera-overwrite probe.
// First-seen-per-slot is always-on (one stderr line per unique slot index seen
// here); per-summary is env-gated every 600 frames.
static const bool s_spotDiagCamEnabled = false;
static std::vector<bool> s_spotDiagCamSeenSlot;  // sized to MAX_LIGHTS_IN_WORLD lazily
static unsigned long s_spotDiagCamFrames        = 0;
static unsigned long s_spotDiagCamPointSeen     = 0;  // window: distinct POINT slots seen
static unsigned long s_spotDiagCamActiveTrue    = 0;  // window: active_after=true counts
static unsigned long s_spotDiagCamActiveFalse   = 0;  // window: active_after=false counts

// T1.16 — per-slot active-state tracker for (E)-owned slots only.
// Sentinel 0xFFu == "never seen yet" (no transition emitted on first sample).
static std::vector<unsigned char> s_spotDiagT16LastActive;

void Camera::updateLights()
{
	//---------------------------------------------------------------------------------
	// Check which lights are on screen and deactivate those which are NOT on screen!
	// Remember the previous counts: consumers hold the pointer array and a count that was
	// current at their last SetLightList, so entries past the new count must not keep pointing
	// at lights the owner has since freed.
	const long prevActiveLights = numActiveLights;
	const long prevTerrainLights = numTerrainLights;
	numActiveLights = numTerrainLights = 0;
	++s_spotDiagCamFrames;  // T1.15 frame tick (updateLights is once-per-frame)
	if (s_spotDiagCamSeenSlot.empty()) s_spotDiagCamSeenSlot.assign(MAX_LIGHTS_IN_WORLD, false);
	for (long i=0;i<MAX_LIGHTS_IN_WORLD;i++)
	{
		if (worldLights[i])
		{
			TG_LightPtr light = worldLights[i];
			
			// IF we are a terrain Light, we are ONLY active when day changes to Night or vice/versa.
			// On screen matters not.
			if (light->lightType == TG_LIGHT_TERRAIN)
			{
				Stuff::Vector4D dummy;
				// [PROJECTZ:LightingShadow id=light_terrain_active_test]
				PROJECTZ_SITE("light_terrain_active_test", "LightingShadow");
				light->active = projectForLightingShadow(light->position,dummy);
				if (light->active)
				{
					if (terrainLightCalc)
						activeLights[numActiveLights++] = light;
					terrainLights[numTerrainLights++] = light;
				}
				continue;			
			}
				
			//If we are infinite or Ambient, we MUST be active
			if ((light->lightType == TG_LIGHT_AMBIENT)  ||
				(light->lightType == TG_LIGHT_INFINITE))
			{
				light->active = true;
				activeLights[numActiveLights++] = light;
				terrainLights[numTerrainLights++] = light;
				continue;			   
			}
			
			// IF we are a Spot or Point light, we must know if we are onscreen.
			// This is complicated because these lights have a radius!
			// For now, simple position check.  Make complicated later!
			if (light->lightType >= TG_LIGHT_POINT && light->lightType < TG_LIGHT_TERRAIN)
			{
				Stuff::Vector4D dummy;
				// [PROJECTZ:LightingShadow id=light_spot_point_active_test]
				PROJECTZ_SITE("light_spot_point_active_test", "LightingShadow");
				// LIGHT-CULL-RADIUS-1 (batch-6 item 5/9): the legacy test was
				// position-only ("simple position check. Make complicated
				// later!") AND unconditionally overwrote the owner's intent.
				// Two field bugs fell out of that: (1) a mover leaving the
				// frame kills its pool instantly even though the light VOLUME
				// still reaches on-screen terrain (headlight popping); (2) at
				// oblique angles whole building clusters go dark when the
				// source props' light positions project off-screen while their
				// pools are plainly in view (mc2_10 airbase 204753 vs 204907).
				// Fix: admit when ANY extreme of the light volume (position
				// +/- farDistance on the ground axes) projects on-screen, and
				// compose with the owner's FoW/status intent (ownerEnabled,
				// written by mech3d/gvactor/bdactor) instead of racing it.
				bool lightOnScreen = projectForLightingShadow(light->position,dummy);
				{
					const float lightR = light->farDistance;
					if (!lightOnScreen && lightR > 0.0f)
					{
						for (int lo = 0; lo < 4 && !lightOnScreen; ++lo)
						{
							glm::vec3 probe = light->position;
							if      (lo == 0) probe.x += lightR;
							else if (lo == 1) probe.x -= lightR;
							else if (lo == 2) probe.y += lightR;
							else              probe.y -= lightR;
							lightOnScreen = projectForLightingShadow(probe,dummy);
						}
					}
					bool ownerOk = (light->ownerEnabled != 0);
					// Building-owned slots are night-only by authored
					// semantic (bdactor writer pred includes isNight) but
					// their visibility-gated update may not run at a
					// day/night transition while off-screen — compose the
					// night gate here so a stale ownerEnabled can't leak a
					// daytime pool.
					if (ownerOk && !isNight) {
						if (mc2_spotlight_diag::is_e_slot(this, i) == mc2_spotlight_diag::Bldg)
							ownerOk = false;
					}
					light->active = ownerOk && lightOnScreen;
				}
				// T1.16 [SPOT_DIAG v1] per-slot transition probe — emit ONLY for
				// (E)-owned slots, state-change-on-transition. Per-summary
				// snapshot below at end of updateLights() every 600 frames.
				if (mc2_spotlight_diag::is_enabled()) {
					if (const auto src = mc2_spotlight_diag::is_e_slot(this, i)) {
						if (s_spotDiagT16LastActive.size() < static_cast<size_t>(MAX_LIGHTS_IN_WORLD))
							s_spotDiagT16LastActive.assign(MAX_LIGHTS_IN_WORLD, 0xFFu);
						const unsigned char curActive = light->active ? 1u : 0u;
						const unsigned char prev = s_spotDiagT16LastActive[i];
						if (prev != 0xFFu && prev != curActive) {
							const char* srcName =
								(src == mc2_spotlight_diag::Bldg ? "bldg" :
								 src == mc2_spotlight_diag::Mech ? "mech" : "gv");
							MC2_DIAGFE(stderr,
								"[SPOT_DIAG v1] event=our_slot_transition slot=%ld src=%s "
								"frame=%lu light_pos=(%.1f, %.1f, %.1f) active=%u prev=%u\n",
								i, srcName, s_spotDiagCamFrames,
								static_cast<float>(light->position.x), static_cast<float>(light->position.y), static_cast<float>(light->position.z),
								static_cast<unsigned>(curActive), static_cast<unsigned>(prev));
							std::fflush(stderr);
						}
						s_spotDiagT16LastActive[i] = curActive;
					}
				}
				// T1.15 [SPOT_DIAG v1] camera-overwrite probe.
				// First-hit per slot index — always-on, one stderr line per
				// distinct slot ever seen here. Window counters tally
				// active_after true/false for periodic summary.
				if (static_cast<size_t>(i) < s_spotDiagCamSeenSlot.size() && !s_spotDiagCamSeenSlot[i]) {
					s_spotDiagCamSeenSlot[i] = true;
					const char* tname = (light->lightType == TG_LIGHT_POINT) ? "POINT" : "SPOT";
					MC2_DIAGFE(stderr,
						"[SPOT_DIAG v1] event=overwrite_first_seen slot=%ld type=%s "
						"pos=(%.2f,%.2f,%.2f) active_after=%d\n",
						i, tname,
						static_cast<float>(light->position.x), static_cast<float>(light->position.y), static_cast<float>(light->position.z),
						light->active ? 1 : 0);
					std::fflush(stderr);
					++s_spotDiagCamPointSeen;
				}
				if (light->active) ++s_spotDiagCamActiveTrue;
				else               ++s_spotDiagCamActiveFalse;
				activeLights[numActiveLights++] = light;
				terrainLights[numTerrainLights++] = light;
			}
		}
	}
	for (long i = numActiveLights; i < prevActiveLights && i < MAX_LIGHTS_IN_WORLD; ++i)
		activeLights[i] = nullptr;
	for (long i = numTerrainLights; i < prevTerrainLights && i < MAX_LIGHTS_IN_WORLD; ++i)
		terrainLights[i] = nullptr;
	// T1.15 [SPOT_DIAG v1] per-summary overwrite aggregate (env-gated).
	if (s_spotDiagCamEnabled && (s_spotDiagCamFrames % 600) == 0) {
		unsigned long total = s_spotDiagCamActiveTrue + s_spotDiagCamActiveFalse;
		double truePct  = (total > 0) ? (100.0 * static_cast<double>(s_spotDiagCamActiveTrue)  / static_cast<double>(total)) : 0.0;
		double falsePct = (total > 0) ? (100.0 * static_cast<double>(s_spotDiagCamActiveFalse) / static_cast<double>(total)) : 0.0;
		mc2::fmt::print_err("[SPOT_DIAG v1] event=overwrite_summary frames={:d} point_slots_seen={:d} active_true_pct={:.2f} active_false_pct={:.2f} samples={:d}\n", mc2::fmt::as_u32(s_spotDiagCamFrames), mc2::fmt::as_u32(s_spotDiagCamPointSeen), truePct, falsePct, mc2::fmt::as_u32(total));
		std::fflush(stderr);
		// Reset window counters (per-summary cadence per task spec)
		s_spotDiagCamActiveTrue  = 0;
		s_spotDiagCamActiveFalse = 0;
		// T1.16 — per-slot summary: one stderr line per tagged (E)-owned slot.
		// Periodic ground-truth snapshot (every 600 frames) without per-frame
		// line volume. Walks MAX_LIGHTS_IN_WORLD; emits only for slots that
		// is_e_slot() recognizes as tagged.
		for (long si = 0; si < MAX_LIGHTS_IN_WORLD; ++si) {
			if (const auto src = mc2_spotlight_diag::is_e_slot(this, si)) {
				TG_LightPtr l = worldLights[si];
				const char* srcName =
					(src == mc2_spotlight_diag::Bldg ? "bldg" :
					 src == mc2_spotlight_diag::Mech ? "mech" : "gv");
				mc2::fmt::print_err("[SPOT_DIAG v1] event=our_slot_summary frame={:d} slot={:d} src={} active={:d} pos=({:.1f}, {:.1f}, {:.1f})\n", mc2::fmt::as_u32(s_spotDiagCamFrames), mc2::fmt::as_i32(si), mc2::fmt::safe(srcName), mc2::fmt::as_u32(l ? static_cast<unsigned>((l->active ? 1u : 0u)) : 0xFFu), l ? static_cast<float>(l->position.x) : 0.0f, l ? static_cast<float>(l->position.y) : 0.0f, l ? static_cast<float>(l->position.z) : 0.0f);
				std::fflush(stderr);
			}
		}
	}
}
