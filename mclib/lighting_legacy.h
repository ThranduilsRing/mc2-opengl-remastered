#pragma once
// The legacy day/night colour maths (formerly the body of Camera::updateDaylight, mclib/camera_daylight.cpp), moved here as a pure
// function so LightingState's legacy curve generator and the camera share one copy. Verbatim arithmetic, including the float/double
// mix and the truncating byte casts: the unit test pins it against fixed values. Nothing calls it per frame (the call in
// gamecam.cpp stays commented out); Camera::updateDaylight is its only caller.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "dstd.h"
#include "lighting_state.h"

#pragma warning(push)
#pragma warning(disable:4305 4244 4267)

namespace mc2::lighting::legacy {
using namespace mc2::lighting;


//---------------------------------------------------------------------------
inline constexpr float rhatx = 1.0; //cos(0/*degrees*/ * DEGREES_TO_RADS);
inline constexpr float rhaty = 0.0; //sin(0/*degrees*/ * DEGREES_TO_RADS);
inline constexpr float ghatx = -0.5; //cos(120/*degrees*/ * DEGREES_TO_RADS);
inline constexpr float ghaty = 0.86602540378443864676372317075294f; //sin(120/*degrees*/ * DEGREES_TO_RADS);
inline constexpr float bhatx = -0.5; //cos(240/*degrees*/ * DEGREES_TO_RADS);
inline constexpr float bhaty = -0.86602540378443864676372317075294f; //sin(240/*degrees*/ * DEGREES_TO_RADS);
inline constexpr float two_pi = 6.283185307179586476925286766559f;

/* all params range from 0.0 to 1.0 */
inline void rgb2hsi(float r, float g, float b, float &hue, float &saturation, float &intensity)
{
	intensity = (r + g + b) / 3.0;

	if (0.0 >= intensity) {
		saturation = 0.0;
		hue = 0.0;
		return;
	}

	float min = r;
	min = std::min(min, g);
	min = std::min(min, b);

	saturation = 1.0 - (min / intensity);

	float cx = r * rhatx + g * ghatx + b * bhatx;
	float cy = r * rhaty + g * ghaty + b * bhaty;

	hue = atan2(cy, cx);
	if (0.0 > hue) { hue += two_pi; }
	hue /= two_pi;
}

inline void hsi2rgb(float hue, float saturation, float intensity, float &r, float &g, float &b)
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
inline float smoothConnectingInterpolation(float y0, float y1, float x)
{
	float a = 0.5 * (y0 - y1);
	float b = 0.5 * (y0 + y1);
	float retval = a * cos(x * (0.5 * two_pi)) + b;
	return retval;
}

// The mission's Camera-block colours (the legacy Day / Sunset / Night keys) and the fog block.
struct Params {
	uint8_t day[3]{}, sunset[3]{}, night[3]{}, dayAmb[3]{}, nightAmb[3]{};
	uint32_t dayFogColor = 0;
	float fogTransparency = 0;
};

struct Sample {
	float pitch = 0;                 // the sun pitch after the night mirror (moon pitch)
	uint8_t sun[3]{}, amb[3]{};
	uint32_t fogColor = 0;
	bool isNight = false;
	float nightFactor = 0;
};

inline constexpr float kNightStartPitch = 25.0f;   // = camera_internal.h NIGHT_START_PITCH
inline constexpr float kNightLightPitch = 10.0f;   // = NIGHT_LIGHT_PITCH

// lightPitchIn is the day-cycle angle in degrees (0..360: 0 and 180 are the horizons, 90 noon); cameraRotation is the camera yaw.
inline Sample sample(const Params& P, float lightPitchIn, float lightYaw, float cameraRotation)
{
	const float NIGHT_START_PITCH = kNightStartPitch, NIGHT_LIGHT_PITCH = kNightLightPitch;
	float lightPitch = lightPitchIn;
	bool isNight = false;
	float nightFactor = 0.0f;
	uint32_t fogColor = 0;
	uint8_t ambientRed, ambientGreen, ambientBlue, lightRed, lightGreen, lightBlue;
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
				nightFactor = std::min(nightFactor, 1.0f);
			}
		
			/* daytime */
			float fSineOfPitch = sin(lightPitch * DEGREES_TO_RADS);

			/* these calculations are sort of based on optics */
			//Do not let day ambient go darker then nightAmbient for smooth transitions.
			fAmbientRed = (P.dayAmb[0] - P.nightAmb[0]) * fSineOfPitch + P.nightAmb[0];
			fAmbientGreen = (P.dayAmb[1] - P.nightAmb[1]) * fSineOfPitch + P.nightAmb[1];
			fAmbientBlue = (P.dayAmb[2] - P.nightAmb[2]) * fSineOfPitch + P.nightAmb[2];

			/* these sunset color calculations are not based on optics*/
			float fAngleFromTheHorizon = 90.0/*degrees*/ - fabs(90.0/*degrees*/ - lightPitch);
			/*static*/ const float fSunSetStartAngle = 35.0/*degrees*/;
			if (fSunSetStartAngle > fAngleFromTheHorizon) 
			{
				const float fSunSetRed = P.sunset[0];
				const float fSunSetGreen = P.sunset[1];
				const float fSunSetBlue = P.sunset[2];
				float fSunSetHue;
				float fSunSetSaturation;
				float fSunSetIntensity;
				rgb2hsi(fSunSetRed / 255.0f, fSunSetGreen / 255.0f, fSunSetBlue / 255.0f
					, fSunSetHue, fSunSetSaturation, fSunSetIntensity);

				float fDayLightHue;
				float fDayLightSaturation;
				float fDayLightIntensity;
				rgb2hsi(P.day[0] / 255.0f, P.day[1] / 255.0f, P.day[2] / 255.0f
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
				R = std::min(R, 0.99f);
				G = std::min(G, 0.99f);
				B = std::min(B, 0.99f);
				R = std::max(R, 0.0f);
				G = std::max(G, 0.0f);
				B = std::max(B, 0.0f);
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
				float fAzimuthCameraAngleRelativeToTheSun = fmod(fabs(fSunAzimuth - (cameraRotation - 135.0)), 360.0/*degrees*/);
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
				R = std::min(R, 0.99f);
				G = std::min(G, 0.99f);
				B = std::min(B, 0.99f);
				R = std::max(R, 0.0f);
				G = std::max(G, 0.0f);
				B = std::max(B, 0.0f);
				fSkyRed = R * 255.0f;
				fSkyGreen = G * 255.0f;
				fSkyBlue = B * 255.0f;

				/*static*/ const float fFadeToDarknessStartAngle  = 5.0/*degrees*/;
				if (fFadeToDarknessStartAngle > fAngleFromTheHorizon) 
				{
					float fOneMinusDarknessProportion = sin((fAngleFromTheHorizon) * (90.0f/*degrees*/ / fFadeToDarknessStartAngle) * DEGREES_TO_RADS);
					float fDarknessProportion = 1.0f - fOneMinusDarknessProportion;

					fLightRed = fLightRed * fOneMinusDarknessProportion + P.night[0] * fDarknessProportion;
					fLightGreen = fLightGreen * fOneMinusDarknessProportion + P.night[1] * fDarknessProportion;
					fLightBlue = fLightBlue * fOneMinusDarknessProportion + P.night[2] * fDarknessProportion;

					fSkyRed = fSkyRed * fOneMinusDarknessProportion;
					fSkyGreen = fSkyGreen * fOneMinusDarknessProportion;
					fSkyBlue = fSkyBlue * fOneMinusDarknessProportion;
				}
			}
			else 
			{
				fLightRed = P.day[0];
				fLightGreen = P.day[1];
				fLightBlue = P.day[2];

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
			fAmbientRed = P.nightAmb[0];// * fSineOfPitch;
			fAmbientGreen = P.nightAmb[1];// * fSineOfPitch;
			fAmbientBlue = P.nightAmb[2];// * fSineOfPitch;

			fLightRed = P.night[0];
			fLightGreen = P.night[1];
			fLightBlue = P.night[2];

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

		/* In the real world, fog color is generally varies with the ambient color, but in our
		game fog color more reflects "sky color near the horizon" */

		float fFogTransparency = P.fogTransparency;
		float fOpaqueDayFogRed = static_cast<float>(((P.dayFogColor >> 16) & 0xff));
		float fOpaqueDayFogGreen = static_cast<float>(((P.dayFogColor >> 8) & 0xff));
		float fOpaqueDayFogBlue = static_cast<float>(((P.dayFogColor) & 0xff));

		static const float fBlackSkyLevel = 32.0f; /* If the light is below this level, the sky is effectively black. */

		//float fFogRed = fLightRed - fBlackSkyLevel;
		//float fFogGreen = fLightGreen - fBlackSkyLevel;
		//float fFogBlue = fLightBlue - fBlackSkyLevel;
		float fFogRed = fAmbientRed - fBlackSkyLevel;
		float fFogGreen = fAmbientGreen - fBlackSkyLevel;
		float fFogBlue = fAmbientBlue - fBlackSkyLevel;
		fFogRed = std::max(fFogRed, 0.0f);
		fFogGreen = std::max(fFogGreen, 0.0f);
		fFogBlue = std::max(fFogBlue, 0.0f);

		//float fTmpRed = (float)(P.day[0]) - fBlackSkyLevel;
		//float fTmpGreen = (float)(P.day[1]) - fBlackSkyLevel;
		//float fTmpBlue = (float)(P.day[2]) - fBlackSkyLevel;
		float fTmpRed = static_cast<float>((P.dayAmb[0])) - fBlackSkyLevel;
		float fTmpGreen = static_cast<float>((P.dayAmb[1])) - fBlackSkyLevel;
		float fTmpBlue = static_cast<float>((P.dayAmb[2])) - fBlackSkyLevel;
		float epsilon = 1.0;
		fTmpRed = std::max(fTmpRed, epsilon);
		fTmpGreen = std::max(fTmpGreen, epsilon);
		fTmpBlue = std::max(fTmpBlue, epsilon);

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
		fogRed = std::min<unsigned int>(0xff, fogRed);
		fogGreen = std::min<unsigned int>(0xff, fogGreen);
		fogBlue = std::min<unsigned int>(0xff, fogBlue);
		fogColor = (fogRed << 16) + (fogGreen << 8) + fogBlue;

	Sample s;
	s.pitch = lightPitch;
	s.sun[0] = lightRed; s.sun[1] = lightGreen; s.sun[2] = lightBlue;
	s.amb[0] = ambientRed; s.amb[1] = ambientGreen; s.amb[2] = ambientBlue;
	s.fogColor = fogColor;
	s.isNight = isNight;
	s.nightFactor = nightFactor;
	return s;
}

// The legacy curve generator: the stock Day/Sunset/Night colours sampled over one cycle (t = lightPitchIn / 360) as a TodCurve.
// A mission opts in by authoring this curve; nothing builds it by default and DayToNightTime is not wired to it.
inline TodCurve curve(const Params& P, float lightYaw, float cameraRotation, int steps)
{
	TodCurve c;
	steps = std::max(steps, 2);
	for (int i = 0; i <= steps; ++i) {
		TodKey k;
		k.t = static_cast<float>(i) / static_cast<float>(steps);
		const Sample s = sample(P, k.t * 360.0f, lightYaw, cameraRotation);
		k.mask = kPitch | kSun | kAmb | kFogColor;
		k.pitch = s.pitch;
		for (int j = 0; j < 3; ++j) { k.sun[j] = s.sun[j]; k.amb[j] = s.amb[j]; }
		k.fogColor = s.fogColor;
		c.keys.push_back(k);
	}
	return c;
}

} // namespace mc2::lighting::legacy

#pragma warning(pop)
