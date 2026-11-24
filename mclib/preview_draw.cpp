// CPU draw of one TG_MultiShape from the hierarchy matrices (shapeToWorld / worldToShape) that TransformMultiShape_*
// leaves behind: no per-shape scratch. Two modes:
//   UI preview (Mech Bay, Purchase, component and weapon panels): straight to the preview target with gos_DrawTriangles.
//   World fallback (compass, sensor markers, detached arms, vehicles and props a batcher declined): queued through the
//   render submissions like any other legacy triangle.
// Lighting matches the old per-vertex kernel (hot-colour markers, ambient, infinite lights, highlight, base colour);
// fog uses the shape's fogRGB alpha plus the camera distance haze (the GPU path's poseFog_/hazeFactor).
// Specular, point/spot/terrain lights and shadows are not applied. The GPU batchers do those for everything they take.
#include "msl_internal.h"
#include "tgl_internal.h"
#include "mc2fmt.h"
#include "tgl_force_z.h"
#include "tex_resolve_table.h"
#include "battle_bench_sink.h"
#include "../code/render_view.h"
#include <array>
#include <algorithm>
#include <span>
#include <vector>

namespace {
// The submissions node and flags a world-mode triangle uses; the reservation and the fill must agree.
void cpuDrawKey(const TG_TinyTexture* textures, const TG_TypeTriangle& tri, bool isWindow, bool isSpotlight, BYTE alphaValue, DWORD addFlags, DWORD& node, DWORD& flags)
{
	if (isSpotlight)
	{
		node = 0xffffffff;
		flags = MC2_ISSPOTLGT;
		return;
	}
	if (isWindow)
	{
		node = 0xffffffff;
		flags = MC2_DRAWALPHA;
		return;
	}
	node = textures[tri.localTextureHandle].mcTextureNodeIndex;
	flags = ((textures[tri.localTextureHandle].textureAlpha || alphaValue != 0xff) ? MC2_DRAWALPHA : MC2_DRAWSOLID) | addFlags;
}
DWORD cpuDrawFlags(bool alphaTest, bool hud, bool clamped)
{
	DWORD addFlags = 0;
	if (hud) addFlags |= MC2_ISCOMPASS;
	if (clamped) addFlags |= MC2_ISTERRAIN;
	if (alphaTest) addFlags |= MC2_ALPHATEST;
	return addFlags;
}

struct ClipVertex
{
	Stuff::Vector4D clip;
	float u, v;
	DWORD argb, frgb;
};

float clipDistance (const ClipVertex& v, int plane)
{
	// Reverse-Z: the near plane is z=w and the far plane is z=0. plane 2 is w>0 (in front of the eye).
	if (plane == 1)
		return -v.clip.z;
	if (plane == 2)
		return -v.clip.w;
	return v.clip.z - v.clip.w;
}

DWORD lerpColor (DWORD a, DWORD b, float t)
{
	DWORD out = 0;
	for (int shift = 0; shift < 32; shift += 8)
	{
		const float ca = static_cast<float>((a >> shift) & 0xff);
		const float cb = static_cast<float>((b >> shift) & 0xff);
		out |= static_cast<DWORD>(float2long(ca + (cb - ca) * t)) << shift;
	}
	return out;
}

ClipVertex lerpClip (const ClipVertex& a, const ClipVertex& b, float t)
{
	ClipVertex out;
	out.clip.x = a.clip.x + (b.clip.x - a.clip.x) * t;
	out.clip.y = a.clip.y + (b.clip.y - a.clip.y) * t;
	out.clip.z = a.clip.z + (b.clip.z - a.clip.z) * t;
	out.clip.w = a.clip.w + (b.clip.w - a.clip.w) * t;
	out.u = a.u + (b.u - a.u) * t;
	out.v = a.v + (b.v - a.v) * t;
	out.argb = lerpColor(a.argb, b.argb, t);
	out.frgb = lerpColor(a.frgb, b.frgb, t);
	return out;
}

int clipPolygon (std::span<const ClipVertex> src, int plane, ClipVertex* dst)
{
	int out = 0;
	const int count = static_cast<int>(src.size());
	for (int i = 0; i < count; i++)
	{
		const ClipVertex& a = src[i];
		const ClipVertex& b = src[(i + 1) % count];
		const float da = clipDistance(a, plane);
		const float db = clipDistance(b, plane);
		const bool insideA = da <= 0.0f;
		const bool insideB = db <= 0.0f;
		if (insideA)
			dst[out++] = a;
		if (insideA != insideB)
			dst[out++] = lerpClip(a, b, da / (da - db));
	}
	return out;
}

void projectClipVertex (const Stuff::Vector4D& xform, bool perspective, float viewMulX, float viewMulY, float viewAddX, float viewAddY, gos_VERTEX& v)
{
	if (perspective)
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
}
}

void TG_MultiShape::ReserveCpuDraw (bool spotlightsOnly)
{
	BindMaterials();
	for (long i = 0; i < numTG_Shapes; i++)
	{
		if (listOfShapes[i].processMe && listOfShapes[i].node)
			listOfShapes[i].node->ReserveCpuDraw(alphaValue, isHudElement, isClamped, spotlightsOnly);
	}
}

// A searchlight cone is drawn at night only, additively (MC2_ISSPOTLGT), and never for a mover faded to invisible.
static bool spotlightDrawn(bool lightsOut, BYTE alphaValue)
{
	return eye->getIsNight() && !lightsOut && !mc2_cone_glow::cone_geom_hidden_by_alpha(alphaValue);
}

void TG_Shape::ReserveCpuDraw (BYTE alphaValue, bool hud, bool clamped, bool spotlightsOnly)
{
	if (hud || !renderTGLShapes || !numVertices || !eye || (spotlightsOnly && !isSpotlight))
		return;
	if (isSpotlight && !spotlightDrawn(lightsOut, alphaValue))
		return;
	TG_TypeShapePtr theShape = static_cast<TG_TypeShapePtr>(myType);
	const TG_TinyTexture* textures = materialTextures();
	if (!textures && !isWindow && !isSpotlight)
		return;
	const DWORD addFlags = cpuDrawFlags(theShape->alphaTestOn, hud, clamped);
	for (DWORD j = 0; j < numTriangles; j++)
	{
		if (!isSpotlight && textures && theShape->listOfTypeTriangles[j].localTextureHandle >= theShape->numTextures)
			continue;
		DWORD node, flags;
		cpuDrawKey(textures, theShape->listOfTypeTriangles[j], isWindow, isSpotlight, alphaValue, addFlags, node, flags);
		// Near/far clipping can turn one triangle into a quad, so reserve both triangles up front.
		mc2::renderSubmissions().addTriangleBulk(node, flags, 2);
	}
}

void TG_MultiShape::PreviewDraw (mc2::tgl::ViewUniforms view)
{
	if (!view.cameraValid)
		return;
	BindMaterials();
	for (long i = 0; i < numTG_Shapes; i++)
	{
		if (listOfShapes[i].processMe && listOfShapes[i].node)
			listOfShapes[i].node->CpuDraw(view, listOfShapes[i].shapeToWorld, listOfShapes[i].worldToShape, alphaValue, false, -1.0f, false, false, false);
	}
}

void TG_MultiShape::Render (mc2::tgl::ViewUniforms view, bool refreshTextures, float forceZ, bool spotlightsOnly)
{
	if (!view.cameraValid)
		return;
	BindMaterials();
	for (long i = 0; i < numTG_Shapes; i++)
	{
		if (listOfShapes[i].processMe && listOfShapes[i].node)
			listOfShapes[i].node->CpuDraw(view, listOfShapes[i].shapeToWorld, listOfShapes[i].worldToShape, alphaValue, true, forceZ, isHudElement, isClamped, spotlightsOnly);
	}
}

void TG_Shape::CpuDraw (const mc2::tgl::ViewUniforms& view, const glm::mat4& shapeToWorld, const glm::mat4& worldToShape, BYTE alphaValue, bool world, float forceZ, bool hud, bool clamped, bool spotlightsOnly)
{
	if (!renderTGLShapes || !numVertices || !view.cameraValid || (isSpotlight && !world) || (spotlightsOnly && !isSpotlight))
		return;
	if (isSpotlight && (!view.night || lightsOut || mc2_cone_glow::cone_geom_hidden_by_alpha(alphaValue)))
		return;
	// World mode draws only leaves the hierarchy walk reached this frame (culled actors keep stale matrices).
	// Compare with the extracted turn: the overlap worker bumps the live turn (and may restamp a leaf) during draw.
	if (world && static_cast<long>(lastTurnTransformed) < mc2::render_view::clock().turn)
		return;
	if (mc2::battlebench::output) ++mc2::battlebench::cpuLeaves;

	TG_TypeShapePtr theShape = static_cast<TG_TypeShapePtr>(myType);

	bool isNight = view.night;
	float nightFactor = view.nightFactor;
	if (lightsOut)
	{
		isNight = false;
		nightFactor = 0.0f;
	}

	// Ambient colour and the infinite lights' directions in shape space.
	DWORD redAmb = 0, greenAmb = 0, blueAmb = 0;
	glm::vec3 lightDir[MAX_LIGHTS_IN_WORLD];
	// Window and searchlight leaves are not lit by the scene lights (as the old per-vertex kernel).
	const DWORD numLights = (isSpotlight || isWindow) ? 0 : static_cast<DWORD>(view.lights.size());
	for (DWORD i = 0; i < numLights && i < MAX_LIGHTS_IN_WORLD; i++)
	{
		if (view.lights[i].type == TG_LIGHT_AMBIENT)
		{
			const DWORD c = view.lights[i].color;
			redAmb = (c >> 16) & 0xff; greenAmb = (c >> 8) & 0xff; blueAmb = c & 0xff;
		}
		else if (view.lights[i].type == TG_LIGHT_INFINITE)
		{
			const glm::mat4 lightToShape = view.lights[i].lightToWorld * worldToShape;
			lightDir[i] = mc2::math::localForwardInWorld(lightToShape);
		}
	}

	const glm::mat4 shapeToClip = shapeToWorld * view.worldToClip;
	const bool compass = hud && !S_stricmp(theShape->getNodeId(), "compassplane");
	static thread_local std::vector<gos_VERTEX> verts;
	static thread_local std::vector<Stuff::Vector4D> clips;
	verts.resize(numVertices);
	if (world)
		clips.resize(numVertices);

	// The GPU legacy shader mixes the scene fog colour with vFogValue = frgb.a/255. The old kernel took
	// the darker of the shape's elevation fog and the camera distance haze; the GPU vehicle path does the
	// same via max(hazeFactor, poseFog_).
	DWORD worldFog = fogRGB;
	if (view.fogEnabled && !isSpotlight)
	{
		const float haze = std::clamp(view.haze, 0.0f, 1.0f);
		const DWORD distFog = static_cast<DWORD>(float2long((1.0f - haze) * 255.0f));
		if (distFog < (worldFog >> 24))
			worldFog = (worldFog & 0x00ffffff) | (distFog << 24);
	}

	for (long j = 0; j < numVertices; j++)
	{
		glm::vec3 pos = theShape->listOfTypeVertices[j].position;
		if (shapeScalar > 0.0f)
			pos *= shapeScalar;

		Stuff::Vector4D xform;
		xform.MultiplyPoint(pos, shapeToClip);
		if (world && hud) {
			glm::vec3 p = mc2::math::transformPoint(pos, shapeToWorld);
			xform.MultiplyPoint(p, view.worldToClip);
		}
		// Legacy perspective has negative w in front of the eye. Normalize
		// its homogeneous sign for clipping; projected coordinates stay identical.
		if (world && view.perspective)
			xform = Stuff::Vector4D(-xform.x, -xform.y, -xform.z, -xform.w);
		if (world)
			clips[j] = xform;
		gos_VERTEX& v = verts[j];
		projectClipVertex(xform, view.perspective, view.viewMulX, view.viewMulY, view.viewAddX, view.viewAddY, v);
		v.frgb = (world && !isSpotlight) ? worldFog : 0xff000000;
		if (g_previewCaptureBounds)
		{
			g_previewBoundsMinX = std::min(g_previewBoundsMinX, v.x);
			g_previewBoundsMaxX = std::max(g_previewBoundsMaxX, v.x);
			g_previewBoundsMinY = std::min(g_previewBoundsMinY, v.y);
			g_previewBoundsMaxY = std::max(g_previewBoundsMaxY, v.y);
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
		else if (hud)
		{
			r = g = b = 0xff;
		}
		if (BaseVertexColor)
		{
			r = std::min<DWORD>(r + ((BaseVertexColor >> 16) & 0xff), 0xff);
			g = std::min<DWORD>(g + ((BaseVertexColor >> 8) & 0xff), 0xff);
			b = std::min<DWORD>(b + (BaseVertexColor & 0xff), 0xff);
		}

		// Infinite lights, then ambient.
		const glm::vec3& n = theShape->listOfTypeVertices[j].normal;
		for (DWORD i = 0; i < numLights && i < MAX_LIGHTS_IN_WORLD; i++)
		{
			if (view.lights[i].type != TG_LIGHT_INFINITE)
				continue;
			const float cosine = glm::dot(lightDir[i], n);
			if (cosine < 0.0f)
			{
				const DWORD c = view.lights[i].color;
				const float k = fabs(cosine);
				r += float2long(static_cast<float>((c >> 16) & 0xff) * k);
				g += float2long(static_cast<float>((c >> 8) & 0xff) * k);
				b += float2long(static_cast<float>(c & 0xff) * k);
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

	// Mech lab GPU path: the GPU mech island draws the panel; this pass only measures the framing extents.
	if (g_previewBoundsOnly && !world)
		return;
	if (!world)
	{
		gos_SetRenderState(gos_State_Fog, 0);
		gos_SetRenderState(gos_State_AlphaTest, theShape->alphaTestOn);
		gos_SetRenderState(gos_State_Filter, theShape->filterOn ? gos_FilterBiLinear : gos_FilterNone);
		gos_SetRenderState(gos_State_ZWrite, 1);
	}
	const DWORD addFlags = cpuDrawFlags(theShape->alphaTestOn, hud, clamped);
	const float forcedZ = (forceZ >= 0.0f && forceZ < 1.0f) ? mc2_tgl::forceZClipDepth(forceZ) : -1.0f;

	const glm::vec3 camPosition = view.cameraValid ? mc2::math::getTranslation(view.cameraOrigin) : glm::vec3(0.0f);
	const glm::vec3 backFacePoint = mc2::math::transformPoint(camPosition, worldToShape);
	const TG_TinyTexture* textures = materialTextures();
	DWORD lastTextureUsed = 0xffffffff;

	auto projectClip = [&](const ClipVertex& cv, gos_VERTEX& v)
	{
		const Stuff::Vector4D& xform = cv.clip;
		projectClipVertex(xform, view.perspective, view.viewMulX, view.viewMulY, view.viewAddX, view.viewAddY, v);
		v.u = cv.u; v.v = cv.v; v.argb = cv.argb; v.frgb = cv.frgb;
	};

	for (DWORD j = 0; j < numTriangles; j++)
	{
		const TG_TypeTriangle& tri = theShape->listOfTypeTriangles[j];
		if (!isSpotlight && textures && tri.localTextureHandle >= theShape->numTextures)
			continue;
		if (!isSpotlight && !(glm::dot(backFacePoint, tri.faceNormal) > 0.0f))
			continue;

		gos_VERTEX gv[3] = { verts[tri.Vertices[0]], verts[tri.Vertices[1]], verts[tri.Vertices[2]] };
		gv[0].u = tri.uvdata.u0; gv[0].v = tri.uvdata.v0;
		gv[1].u = tri.uvdata.u1; gv[1].v = tri.uvdata.v1;
		gv[2].u = tri.uvdata.u2; gv[2].v = tri.uvdata.v2;

		if (world)
		{
			if (!isWindow && !isSpotlight && !textures)
				continue;
			const std::array<ClipVertex, 3> input = {{
				{ clips[tri.Vertices[0]], gv[0].u, gv[0].v, gv[0].argb, gv[0].frgb },
				{ clips[tri.Vertices[1]], gv[1].u, gv[1].v, gv[1].argb, gv[1].frgb },
				{ clips[tri.Vertices[2]], gv[2].u, gv[2].v, gv[2].argb, gv[2].frgb }
			}};
			ClipVertex nearClipped[8], farClipped[8];
			int count = clipPolygon(input, 0, nearClipped);
			if (count)
				count = clipPolygon(std::span<const ClipVertex>(nearClipped, count), 1, farClipped);
			if (count && view.perspective)
			{
				ClipVertex frontClipped[8];
				count = clipPolygon(std::span<const ClipVertex>(farClipped, count), 2, frontClipped);
				std::copy(frontClipped, frontClipped + count, farClipped);
			}
			if (count < 3)
				continue;
			DWORD node, flags;
			cpuDrawKey(textures, tri, isWindow, isSpotlight, alphaValue, addFlags, node, flags);
			for (int k = 1; k + 1 < count; k++)
			{
				gos_VERTEX out[3];
				projectClip(farClipped[0], out[0]);
				projectClip(farClipped[k], out[1]);
				projectClip(farClipped[k + 1], out[2]);
				if (compass) for (auto& v : out) v.frgb = 0x434f4d50;
				if (!compass && forcedZ >= 0.0f)
					out[0].z = out[1].z = out[2].z = forcedZ;
				if (hud) {
					// World-projected HUD shapes composite after grass, water and scene effects.
					const bool exempt = gos_GetHudScaleExempt();
					gos_SetHudScaleExempt(true);
					gos_SetRenderState(gos_State_IsHUD, 1);
					const DWORD texture = tex_resolve(node);
					gos_SetRenderState(gos_State_Texture, texture == 0xffffffff ? 0 : texture);
					gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_AlphaInvAlpha);
					gos_SetRenderState(gos_State_AlphaTest, theShape->alphaTestOn);
					gos_DrawTriangles(out, 3);
					gos_SetRenderState(gos_State_IsHUD, 0);
					gos_SetHudScaleExempt(exempt);
				} else {
					mc2::renderSubmissions().addVertices(node, out, flags);
				}
			}
			continue;
		}

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
	if (!world)
	{
		// Leave no state behind for the next panel: the loop sets the texture, alpha mode and alpha test.
		gos_SetRenderState(gos_State_Fog, 0);
		gos_SetRenderState(gos_State_Texture, 0);
		gos_SetRenderState(gos_State_AlphaMode, gos_Alpha_OneZero);
		gos_SetRenderState(gos_State_AlphaTest, false);
	}
}
