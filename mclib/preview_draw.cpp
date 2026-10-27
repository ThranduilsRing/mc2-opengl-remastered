// UI preview draw (Mech Bay, Purchase, component and weapon panels): draws one TG_MultiShape straight to the
// preview target with gos_DrawTriangles. It needs only the hierarchy matrices (shapeToWorld / worldToShape) that any
// TransformMultiShape* variant leaves behind: no TGL pools, no MLR queue, no per-shape scratch.
// Lighting matches the old per-vertex kernel (hot-colour markers, ambient, infinite lights, highlight, base colour);
// fog, specular, point/spot/terrain lights and shadows are not used by the SimpleCamera preview.
#include "msl_internal.h"
#include "tgl_internal.h"
#include "mc2fmt.h"
#include <algorithm>
#include <vector>

void TG_MultiShape::PreviewDraw (void)
{
	if (!eye)
		return;
	BindMaterials();
	SetLightList(eye->getWorldLights(), eye->getNumLights());
	for (long i = 0; i < numTG_Shapes; i++)
	{
		if (listOfShapes[i].processMe && listOfShapes[i].node)
			listOfShapes[i].node->PreviewDraw(listOfShapes[i].shapeToWorld, listOfShapes[i].worldToShape, alphaValue);
	}
}

void TG_Shape::PreviewDraw (const glm::mat4& shapeToWorld, const glm::mat4& worldToShape, BYTE alphaValue)
{
	if (!renderTGLShapes || !numVertices || isSpotlight || !eye)
		return;

	TG_TypeShapePtr theShape = static_cast<TG_TypeShapePtr>(myType);

	bool isNight = eye->getIsNight();
	float nightFactor = eye->getNightFactor();
	if (lightsOut)
	{
		isNight = false;
		nightFactor = 0.0f;
	}

	// Ambient colour and the infinite lights' directions in shape space.
	DWORD redAmb = 0, greenAmb = 0, blueAmb = 0;
	glm::vec3 lightDir[MAX_LIGHTS_IN_WORLD];
	for (DWORD i = 0; i < s_numLights && i < MAX_LIGHTS_IN_WORLD; i++)
	{
		if (!s_listOfLights[i] || !s_listOfLights[i]->active)
			continue;
		if (s_listOfLights[i]->lightType == TG_LIGHT_AMBIENT)
		{
			const DWORD c = s_listOfLights[i]->GetaRGB();
			redAmb = (c >> 16) & 0xff; greenAmb = (c >> 8) & 0xff; blueAmb = c & 0xff;
		}
		else if (s_listOfLights[i]->lightType == TG_LIGHT_INFINITE)
		{
			const glm::mat4 lightToShape = s_listOfLights[i]->lightToWorld * worldToShape;
			lightDir[i] = mc2::math::localForwardInWorld(lightToShape);
		}
	}

	const glm::mat4 shapeToClip = shapeToWorld * s_worldToClip;
	static thread_local std::vector<gos_VERTEX> verts;
	verts.resize(numVertices);

	for (long j = 0; j < numVertices; j++)
	{
		glm::vec3 pos = theShape->listOfTypeVertices[j].position;
		if (shapeScalar > 0.0f)
			pos *= shapeScalar;

		Stuff::Vector4D xform;
		xform.MultiplyPoint(pos, shapeToClip);
		gos_VERTEX& v = verts[j];
		if (eye->usePerspective)
		{
			const float rhw = (xform.w != 0.0f) ? 1.0f / xform.w : 1.0f;
			v.x = (xform.x * rhw) * viewMulX + viewAddX;
			v.y = (xform.y * rhw) * viewMulY + viewAddY;
			v.z = xform.z * rhw;
			v.rhw = fabs(rhw);
		}
		else
		{
			v.x = (1.0f - xform.x) * viewMulX + viewAddX;
			v.y = (1.0f - xform.y) * viewMulY + viewAddY;
			v.z = xform.z;
			v.rhw = 0.000001f;
		}
		v.frgb = 0xff000000;
		if (g_previewCaptureBounds)
		{
			if (v.x < g_previewBoundsMinX) g_previewBoundsMinX = v.x;
			if (v.x > g_previewBoundsMaxX) g_previewBoundsMaxX = v.x;
			if (v.y < g_previewBoundsMinY) g_previewBoundsMinY = v.y;
			if (v.y > g_previewBoundsMaxY) g_previewBoundsMaxY = v.y;
		}

		// Base colour: the hot-colour markers, else an authored vertex colour.
		DWORD r = 0, g = 0, b = 0;
		const DWORD start = theShape->listOfTypeVertices[j].aRGBLight;
		const DWORD pink = theShape->hotPinkRGB, yellow = theShape->hotYellowRGB, green = theShape->hotGreenRGB;
		auto setRGB = [&](DWORD c, float scale) {
			r = float2long(((c >> 16) & 0xff) * scale); g = float2long(((c >> 8) & 0xff) * scale); b = float2long((c & 0xff) * scale);
		};
		if (start == 0xffff00ff)		// lit windows: night only, else dark grey
		{
			if (isNight) setRGB(pink, 1.0f);
			else if (nightFactor > Stuff::SMALL) setRGB(pink, nightFactor);
			else r = g = b = 0x2f;
		}
		else if (start == 0xffffff00)	// outside building lights
		{
			if (nightFactor >= 0.75f) setRGB(yellow, isNight ? 1.0f : nightFactor);
		}
		else if (start == 0xff00ff00)	// base lights
		{
			if (isNight) setRGB(green, 1.0f);
			else if (nightFactor > Stuff::SMALL) setRGB(green, nightFactor);
		}
		else if (start == 0xffff0000 || start == 0xff0000ff)
		{
		}
		else if (start & 0x00ffffff)
		{
			if (!lightsOut) setRGB(start, 1.0f);
		}
		if (BaseVertexColor)
		{
			r = std::min<DWORD>(r + ((BaseVertexColor >> 16) & 0xff), 0xff);
			g = std::min<DWORD>(g + ((BaseVertexColor >> 8) & 0xff), 0xff);
			b = std::min<DWORD>(b + (BaseVertexColor & 0xff), 0xff);
		}

		// Infinite lights, then ambient.
		const glm::vec3& n = theShape->listOfTypeVertices[j].normal;
		for (DWORD i = 0; i < s_numLights && i < MAX_LIGHTS_IN_WORLD; i++)
		{
			if (!s_listOfLights[i] || !s_listOfLights[i]->active || s_listOfLights[i]->lightType != TG_LIGHT_INFINITE)
				continue;
			const float cosine = glm::dot(lightDir[i], n);
			if (cosine < 0.0f)
			{
				const DWORD c = s_listOfLights[i]->GetaRGB();
				const float k = fabs(cosine);
				r += float2long(float((c >> 16) & 0xff) * k);
				g += float2long(float((c >> 8) & 0xff) * k);
				b += float2long(float(c & 0xff) * k);
			}
		}
		r = std::min<DWORD>(r + redAmb, 255); g = std::min<DWORD>(g + greenAmb, 255); b = std::min<DWORD>(b + blueAmb, 255);
		if (aRGBHighlight)
		{
			r = std::min<DWORD>(r + ((aRGBHighlight >> 16) & 0xff), 255);
			g = std::min<DWORD>(g + ((aRGBHighlight >> 8) & 0xff), 255);
			b = std::min<DWORD>(b + (aRGBHighlight & 0xff), 255);
		}
		v.argb = (DWORD(alphaValue) << 24) + (r << 16) + (g << 8) + b;
	}

	gos_SetRenderState(gos_State_Fog, 0);
	gos_SetRenderState(gos_State_AlphaTest, theShape->alphaTestOn);
	gos_SetRenderState(gos_State_Filter, theShape->filterOn ? gos_FilterBiLinear : gos_FilterNone);
	gos_SetRenderState(gos_State_ZWrite, 1);

	const glm::vec3 camPosition = s_cameraOrigin ? mc2::math::getTranslation(*s_cameraOrigin) : glm::vec3(0.0f);
	const glm::vec3 backFacePoint = mc2::math::transformPoint(camPosition, worldToShape);
	const TG_TinyTexture* textures = materialTextures();
	DWORD lastTextureUsed = 0xffffffff;

	for (DWORD j = 0; j < numTriangles; j++)
	{
		const TG_TypeTriangle& tri = theShape->listOfTypeTriangles[j];
		if (!(glm::dot(backFacePoint, tri.faceNormal) > 0.0f))
			continue;

		gos_VERTEX gv[3] = { verts[tri.Vertices[0]], verts[tri.Vertices[1]], verts[tri.Vertices[2]] };
		gv[0].u = tri.uvdata.u0; gv[0].v = tri.uvdata.v0;
		gv[1].u = tri.uvdata.u1; gv[1].v = tri.uvdata.v1;
		gv[2].u = tri.uvdata.u2; gv[2].v = tri.uvdata.v2;

		if (textures && textures[tri.localTextureHandle].gosTextureHandle != 0xffffffff)
		{
			const TG_TinyTexture& tex = textures[tri.localTextureHandle];
			if (tex.gosTextureHandle != lastTextureUsed)
			{
				gos_SetRenderState(gos_State_Texture, tex.gosTextureHandle);
				if (tex.textureAlpha)
				{
					gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
					gos_SetRenderState(gos_State_AlphaTest, true);
				}
				lastTextureUsed = tex.gosTextureHandle;
			}
		}
		else
			gos_SetRenderState(gos_State_Texture, 0);

		if (gv[0].z >= 0.0f && gv[0].z < 1.0f && gv[1].z >= 0.0f && gv[1].z < 1.0f && gv[2].z >= 0.0f && gv[2].z < 1.0f)
			gos_DrawTriangles(gv, 3);
	}
	gos_SetRenderState(gos_State_Fog, 0);
}
