// Camera: daylight fraction and sun pitch, HSI colour helpers and the per-frame light update.
// Declared in the class headers; shared names in camera_internal.h.
#include "camera_internal.h"
#include "mc2fmt.h"


//---------------------------------------------------------------------------
static const float rhatx = 1.0; //cos(0/*degrees*/ * DEGREES_TO_RADS);
static const float rhaty = 0.0; //sin(0/*degrees*/ * DEGREES_TO_RADS);
static const float ghatx = -0.5; //cos(120/*degrees*/ * DEGREES_TO_RADS);
static const float ghaty = 0.86602540378443864676372317075294f; //sin(120/*degrees*/ * DEGREES_TO_RADS);
static const float bhatx = -0.5; //cos(240/*degrees*/ * DEGREES_TO_RADS);
static const float bhaty = -0.86602540378443864676372317075294f; //sin(240/*degrees*/ * DEGREES_TO_RADS);
static const float two_pi = 6.283185307179586476925286766559f;

/* all params range from 0.0 to 1.0 */
static void rgb2hsi(float r, float g, float b, float &hue, float &saturation, float &intensity)
{
	intensity = (r + g + b) / 3.0;

	if (0.0 >= intensity) {
		saturation = 0.0;
		hue = 0.0;
		return;
	}

	float min = r;
	if (g < min) { min = g; }
	if (b < min) { min = b; }

	saturation = 1.0 - (min / intensity);

	float cx = r * rhatx + g * ghatx + b * bhatx;
	float cy = r * rhaty + g * ghaty + b * bhaty;

	hue = atan2(cy, cx);
	if (0.0 > hue) { hue += two_pi; }
	hue /= two_pi;
}

static void hsi2rgb(float hue, float saturation, float intensity, float &r, float &g, float &b)
{
	float thue = hue;

	if ((1.0f / 3.0f) > hue) {
	} else if ((2.0f / 3.0f) > hue) {
		thue -= 1.0f / 3.0f;
	} else {
		thue -= 2.0f / 3.0f;
	}

	float chatx = cos(two_pi * thue);
	float chaty = sin(two_pi * thue);
	float ti = 3.0f * intensity;

	{
		b = (1.0f - saturation) * intensity;
		float tib = ti - b;
		float denominator = (chaty*(ghatx - rhatx) - chatx*(ghaty - rhaty));
		if (0.0f == denominator) {
			/* I suspect it's mathematically impossible to get here, but I'm too lazy to make sure. */
			denominator = 0.000001f;
		}
		g = ((b*bhaty + tib*rhaty)*chatx - (b*bhatx + tib*rhatx)*chaty) / denominator;
		r = tib - g;
	}

	if ((1.0f / 3.0f) > hue) {
	} else if ((2.0f / 3.0f) > hue) {
		float swap = b;
		b = g;
		g = r;
		r = swap;
	} else {
		float swap = b;
		b = r;
		r = g;
		g = swap;
	}
}

/* interpolation with dy/dx == 0 at x = 0 and x = 1 */
float smoothConnectingInterpolation(float y0, float y1, float x)
{
	float a = 0.5 * (y0 - y1);
	float b = 0.5 * (y0 + y1);
	float retval = a * cos(x * (0.5 * two_pi)) + b;
	return retval;
}

//---------------------------------------------------------------------------
// GAMEPLAY-DRIVER-2: runtime sun control for the MC2_UI_COMMAND_FILE channel.
// See the contract comment on the declarations in camera.h.
float Camera::setDaylightFraction (float f)
{
	// Wrap rather than clamp: 0.25 == noon, 0.5 == sunset, 0.75 == midnight.
	f = f - floorf(f);

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
	updateDaylight(false);            // ambient / sun / sky / fog for this time
	setSunPitch(sunPitch);            // ... but hand update() the SUN angle
	return lightPitch;
}

float Camera::setSunPitch (float pitchDeg, bool setYaw, float yawDeg)
{
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

		/* arbitrary assumed daytime sky color */
		static const float fDaySkyRed = 193.0f;
		static const float fDaySkyGreen = 231.0f;
		static const float fDaySkyBlue = 238.0f;

		float fSkyRed;
		float fSkyGreen;
		float fSkyBlue;

		float fAmbientRed;
		float fAmbientGreen;
		float fAmbientBlue;

		float fLightRed;
		float fLightGreen;
		float fLightBlue;

		if (180.0f/*degrees*/ >= lightPitch) 
		{
		 	//Set flags to help renderer with light sources.
			float lightAltitude = lightPitch;
			if (lightPitch > 90.0f)
				lightAltitude = 180.0f - lightPitch;
				
			isNight = false;
			if (lightAltitude < NIGHT_LIGHT_PITCH)
				isNight = true;
				
			if (lightAltitude > NIGHT_START_PITCH)
			{
				nightFactor = 0.0f;
			}
			else
			{
				nightFactor = (NIGHT_START_PITCH - lightAltitude) / (NIGHT_START_PITCH - NIGHT_LIGHT_PITCH);
				if (nightFactor > 1.0f)
					nightFactor = 1.0f;
			}
		
			/* daytime */
			float fSineOfPitch = sin(lightPitch * DEGREES_TO_RADS);

			/* these calculations are sort of based on optics */
			//Do not let day ambient go darker then nightAmbient for smooth transitions.
			fAmbientRed = (dayAmbientRed - nightAmbientRed) * fSineOfPitch + nightAmbientRed;
			fAmbientGreen = (dayAmbientGreen - nightAmbientGreen) * fSineOfPitch + nightAmbientGreen;
			fAmbientBlue = (dayAmbientBlue - nightAmbientBlue) * fSineOfPitch + nightAmbientBlue;

			/* these sunset color calculations are not based on optics*/
			float fAngleFromTheHorizon = 90.0/*degrees*/ - fabs(90.0/*degrees*/ - lightPitch);
			/*static*/ const float fSunSetStartAngle = 35.0/*degrees*/;
			if (fSunSetStartAngle > fAngleFromTheHorizon) 
			{
				const float fSunSetRed = sunsetLightRed;
				const float fSunSetGreen = sunsetLightGreen;
				const float fSunSetBlue = sunsetLightBlue;
				float fSunSetHue;
				float fSunSetSaturation;
				float fSunSetIntensity;
				rgb2hsi(fSunSetRed / 255.0f, fSunSetGreen / 255.0f, fSunSetBlue / 255.0f
					, fSunSetHue, fSunSetSaturation, fSunSetIntensity);

				float fDayLightHue;
				float fDayLightSaturation;
				float fDayLightIntensity;
				rgb2hsi(dayLightRed / 255.0f, dayLightGreen / 255.0f, dayLightBlue / 255.0f
					, fDayLightHue, fDayLightSaturation, fDayLightIntensity);

				float fSunSetProportion = 0.75/*arbitrary*/ * sin((fSunSetStartAngle - fAngleFromTheHorizon) * (90.0f/*degrees*/ / fSunSetStartAngle) * DEGREES_TO_RADS);
				float fOneMinusSunSetProportion = 1.0f - fSunSetProportion;

				/* for sunlight color, linear interpolation in HSI color space is a better
				model than linear interpolation in RGB color space */
				float H = fSunSetHue * fSunSetProportion + fDayLightHue * fOneMinusSunSetProportion;
				float S = fSunSetSaturation * fSunSetProportion + fDayLightSaturation * fOneMinusSunSetProportion;
				float I = fSunSetIntensity * fSunSetProportion + fDayLightIntensity * fOneMinusSunSetProportion;
				float R, G, B;
				hsi2rgb(H, S, I, R, G, B);
				if (R > 0.99f) { R = 0.99f; }
				if (G > 0.99f) { G = 0.99f; }
				if (B > 0.99f) { B = 0.99f; }
				if (R < 0.0f) { R = 0.0f; }
				if (G < 0.0f) { G = 0.0f; }
				if (B < 0.0f) { B = 0.0f; }
				fLightRed = R * 255.0f;
				fLightGreen = G * 255.0f;
				fLightBlue = B * 255.0f;


				/* Basically I'm using an arbitrary harcoded formula (which may need some
				tweaking) to approximate the sky color as it progresses to sunset. For more
				accurate results it would probably be better to store a table of colors obtained
				from photographs. */

				fSunSetProportion = sin((fSunSetStartAngle - fAngleFromTheHorizon) * (90.0f/*degrees*/ / fSunSetStartAngle) * DEGREES_TO_RADS);
				float fSunAzimuth = lightYaw;
				if (90.0/*degrees*/ < lightPitch) {
					fSunAzimuth += 180.0/*degress*/;
				}
				float fAzimuthCameraAngleRelativeToTheSun = fmod(fabs(fSunAzimuth - (getCameraRotation() - 135.0)), 360.0/*degrees*/);
				if (180.0/*degress*/ < fAzimuthCameraAngleRelativeToTheSun) {
					fAzimuthCameraAngleRelativeToTheSun = 360.0/*degrees*/ - fAzimuthCameraAngleRelativeToTheSun;
				}
				fSunSetProportion *= (1.0 - pow(fAzimuthCameraAngleRelativeToTheSun / 180.0/*degress*/, 2.0/*arbitrary*/));
				fOneMinusSunSetProportion = 1.0 - fSunSetProportion;

				float fDaySkyHue;
				float fDaySkySaturation;
				float fDaySkyIntensity;
				rgb2hsi(fDaySkyRed / 255.0f, fDaySkyGreen / 255.0f, fDaySkyBlue / 255.0f
					, fDaySkyHue, fDaySkySaturation, fDaySkyIntensity);
				H = fSunSetHue * fSunSetProportion + fDaySkyHue * fOneMinusSunSetProportion;
				/*static*/ const float fGreenHue = 1.0f / 3.0f;
				float za;
				if (fGreenHue > H) {
					/*static*/ const float fGreenHueMargin = 1.0 / 6.0 + 0.12/*arbitrary*/;
					if ((fGreenHue - fGreenHueMargin) > H) {
						za = 0.0;
					} else {
						za = smoothConnectingInterpolation(0, 1, (H - (fGreenHue - fGreenHueMargin)) / fGreenHueMargin);
						za = pow(fabs(za), 0.15f/*arbitrary*/);
					}
				} else {
					/*static*/ const float fGreenHueMargin = 1.0 / 6.0 + 0.00/*arbitrary*/;
					if ((fGreenHue + fGreenHueMargin) < H) {
						za = 0.0;
					} else {
						za = smoothConnectingInterpolation(0, 1, ((fGreenHue + fGreenHueMargin) - H) / fGreenHueMargin);
						za = pow(fabs(za), 0.15f/*arbitrary*/);
					}
				}
				S = fSunSetSaturation * fSunSetProportion + fDaySkySaturation * fOneMinusSunSetProportion;
				S = 0.0f/*arbitrary*/ * za + S * (1.0f - za);
				/*static*/ const float brightHue = 0.25 * 1.0 / 3.0; /*arbitrary*/
				float zb;
				if (brightHue > H) {
					if (brightHue != fSunSetHue) {
						zb = sin((H - fSunSetHue) / (brightHue - fSunSetHue) * 0.35f * two_pi);
					} else {
						zb = 0.0;
					}
				} else {
					zb = sin((fDaySkyHue - H) / (fDaySkyHue - brightHue) * 0.35f * two_pi);
				}
				zb = pow(fabs(zb), 1.0f);
				I = fSunSetIntensity * fSunSetProportion + fDaySkyIntensity * fOneMinusSunSetProportion;
				I = 0.85f/*arbitrary*/ * zb + I * (1.0f - zb);
				I *= (1.0 - (fAzimuthCameraAngleRelativeToTheSun / 180.0/*degress*/) * (1.0 - (fAngleFromTheHorizon / fSunSetStartAngle)));
				hsi2rgb(H, S, I, R, G, B);
				if (R > 0.99f) { R = 0.99f; }
				if (G > 0.99f) { G = 0.99f; }
				if (B > 0.99f) { B = 0.99f; }
				if (R < 0.0f) { R = 0.0f; }
				if (G < 0.0f) { G = 0.0f; }
				if (B < 0.0f) { B = 0.0f; }
				fSkyRed = R * 255.0f;
				fSkyGreen = G * 255.0f;
				fSkyBlue = B * 255.0f;

				/*static*/ const float fFadeToDarknessStartAngle  = 5.0/*degrees*/;
				if (fFadeToDarknessStartAngle > fAngleFromTheHorizon) 
				{
					float fOneMinusDarknessProportion = sin((fAngleFromTheHorizon) * (90.0f/*degrees*/ / fFadeToDarknessStartAngle) * DEGREES_TO_RADS);
					float fDarknessProportion = 1.0f - fOneMinusDarknessProportion;

					fLightRed = fLightRed * fOneMinusDarknessProportion + nightLightRed * fDarknessProportion;
					fLightGreen = fLightGreen * fOneMinusDarknessProportion + nightLightGreen * fDarknessProportion;
					fLightBlue = fLightBlue * fOneMinusDarknessProportion + nightLightBlue * fDarknessProportion;

					fSkyRed = fSkyRed * fOneMinusDarknessProportion;
					fSkyGreen = fSkyGreen * fOneMinusDarknessProportion;
					fSkyBlue = fSkyBlue * fOneMinusDarknessProportion;
				}
			}
			else 
			{
				fLightRed = dayLightRed;
				fLightGreen = dayLightGreen;
				fLightBlue = dayLightBlue;

				fSkyRed = fDaySkyRed;
				fSkyGreen = fDaySkyGreen;
				fSkyBlue = fDaySkyBlue;
			}
		} 
		else 
		{
		 	//Set flags to help renderer with light sources.
			isNight = true;
			nightFactor = 1.0f;	
			
			/* nighttime */
			float moonPitch = 180.0f/*degrees*/ - fabs(180.0f/*degrees*/ - lightPitch);
			lightPitch = moonPitch;

			//float fSineOfPitch = sin(moonPitch * DEGREES_TO_RADS);

			/* these calculations are sort of based on optics */
			// DO NOT ever get darker then the night Ambient.  Change hue with moon.
			// Not realistic but very unplayable in the pitch dark.
			fAmbientRed = nightAmbientRed;// * fSineOfPitch;
			fAmbientGreen = nightAmbientGreen;// * fSineOfPitch;
			fAmbientBlue = nightAmbientBlue;// * fSineOfPitch;

			fLightRed = nightLightRed;
			fLightGreen = nightLightGreen;
			fLightBlue = nightLightBlue;

			fSkyRed = 0.0;
			fSkyGreen = 0.0;
			fSkyBlue = 0.0;
		}

		ambientRed = fAmbientRed;
		ambientBlue = fAmbientBlue;
		ambientGreen = fAmbientGreen;
		
		lightRed = fLightRed;
		lightGreen = fLightGreen;
		lightBlue = fLightBlue;

		glm::mat4 lightToWorldMatrix;
		mc2::math::buildTranslation(lightToWorldMatrix, glm::vec3(0.0f,0.0f,0.0f));
		mc2::math::buildRotation(lightToWorldMatrix, mc2::math::Euler(lightPitch * DEGREES_TO_RADS, (lightYaw + 135.0) * DEGREES_TO_RADS, 0.0f));
		worldLights[0]->SetLightToWorld(&lightToWorldMatrix);

		/* In the real world, fog color is generally varies with the ambient color, but in our
		game fog color more reflects "sky color near the horizon" */

		float fFogTransparency = fogTransparency;
		float fOpaqueDayFogRed = static_cast<float>(((dayFogColor >> 16) & 0xff));
		float fOpaqueDayFogGreen = static_cast<float>(((dayFogColor >> 8) & 0xff));
		float fOpaqueDayFogBlue = static_cast<float>(((dayFogColor) & 0xff));

		static const float fBlackSkyLevel = 32.0f; /* If the light is below this level, the sky is effectively black. */

		//float fFogRed = fLightRed - fBlackSkyLevel;
		//float fFogGreen = fLightGreen - fBlackSkyLevel;
		//float fFogBlue = fLightBlue - fBlackSkyLevel;
		float fFogRed = fAmbientRed - fBlackSkyLevel;
		float fFogGreen = fAmbientGreen - fBlackSkyLevel;
		float fFogBlue = fAmbientBlue - fBlackSkyLevel;
		if (0.0f > fFogRed) { fFogRed = 0.0f; }
		if (0.0f > fFogGreen) { fFogGreen = 0.0f; }
		if (0.0f > fFogBlue) { fFogBlue = 0.0f; }

		//float fTmpRed = (float)(dayLightRed) - fBlackSkyLevel;
		//float fTmpGreen = (float)(dayLightGreen) - fBlackSkyLevel;
		//float fTmpBlue = (float)(dayLightBlue) - fBlackSkyLevel;
		float fTmpRed = static_cast<float>((dayAmbientRed)) - fBlackSkyLevel;
		float fTmpGreen = static_cast<float>((dayAmbientGreen)) - fBlackSkyLevel;
		float fTmpBlue = static_cast<float>((dayAmbientBlue)) - fBlackSkyLevel;
		float epsilon = 1.0;
		if (epsilon > fTmpRed) { fTmpRed = epsilon; }
		if (epsilon > fTmpGreen) { fTmpGreen = epsilon; }
		if (epsilon > fTmpBlue) { fTmpBlue = epsilon; }

		float fOneMinusFogTransparency = 1.0 - fFogTransparency;
		fFogRed = fFogRed / fTmpRed * fOpaqueDayFogRed * fOneMinusFogTransparency;
		fFogGreen = fFogGreen / fTmpGreen * fOpaqueDayFogGreen * fOneMinusFogTransparency;
		fFogBlue = fFogBlue / fTmpBlue * fOpaqueDayFogBlue * fOneMinusFogTransparency;

		/* Here we add in the sky color component. */
		fFogRed = fFogRed + fFogTransparency * fSkyRed;
		fFogGreen = fFogGreen + fFogTransparency * fSkyGreen;
		fFogBlue = fFogBlue + fFogTransparency * fSkyBlue;

		unsigned int fogRed = fFogRed;
		unsigned int fogGreen = fFogGreen;
		unsigned int fogBlue = fFogBlue;
		if (0xff < fogRed) { fogRed = 0xff; }
		if (0xff < fogGreen) { fogGreen = 0xff; }
		if (0xff < fogBlue) { fogBlue = 0xff; }
		fogColor = (fogRed << 16) + (fogGreen << 8) + fogBlue;

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
		
		fadeAlpha = (fadeStart >> 24) + (fadePercent * (float(fadeColor >> 24) - float(fadeStart >> 24)));
	}
}

//---------------------------------------------------------------------------
// T1.15 [SPOT_DIAG v1] file-scope state for the camera-overwrite probe.
// First-seen-per-slot is always-on (one stderr line per unique slot index seen
// here); per-summary is env-gated every 600 frames.
static const bool s_spotDiagCamEnabled = mc2::gate::get<"MC2_SPOT_DIAG">();
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
				// Killswitch MC2_LIGHT_CULL_RADIUS=0 -> legacy position-only
				// overwrite.
				static const bool s_cullRadius = mc2::gate::get<"MC2_LIGHT_CULL_RADIUS">();
				bool lightOnScreen = projectForLightingShadow(light->position,dummy);
				if (s_cullRadius)
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
						mc2_spotlight_diag::SourceClass lsrc;
						if (mc2_spotlight_diag::is_e_slot(this, i, &lsrc)
							&& lsrc == mc2_spotlight_diag::Bldg)
							ownerOk = false;
					}
					light->active = ownerOk && lightOnScreen;
				}
				else
					light->active = lightOnScreen;
				// T1.16 [SPOT_DIAG v1] per-slot transition probe — emit ONLY for
				// (E)-owned slots, state-change-on-transition. Per-summary
				// snapshot below at end of updateLights() every 600 frames.
				if (mc2_spotlight_diag::is_enabled()) {
					mc2_spotlight_diag::SourceClass src;
					if (mc2_spotlight_diag::is_e_slot(this, i, &src)) {
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
			mc2_spotlight_diag::SourceClass src;
			if (mc2_spotlight_diag::is_e_slot(this, si, &src)) {
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
