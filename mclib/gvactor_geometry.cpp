#include "mc2_czstring.h"
#include "../code/tick_commit.h"
// GVAppearance: paint schemes, gestures, object/mover parameters, bounds, destruction and node positions.
// Declared in the class headers; shared names in gvactor_internal.h.
#include <atomic>
#include <cmath>  // before any other include: which sqrt/pow overload a call binds must not depend on include order
#include "texture_assets.h"
#include "gvactor_internal.h"
#include "render_snap.h"
#include "appearance_rng.h"
#include "../RenderCore/axis_convert.h"
#include "mc2fmt.h"
#include "model_override_registry.h"
#include "../RenderCore/SurfaceMaterial.h"
#include "assimp_importer.h"
#include "Gates/gate.h"
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

extern std::atomic<uint32_t> g_mc2FrameCounter;

// The presentation mesh follows the stock pose without entering simulation or picking.
void GVAppearance::updateRenderShape()
{
	if (!simPresentationPass && (simulationOwned() || mc2::overlap::onSimWorker())) return;
	mc2::overlap::assertMainThread("GVAppearance::updateRenderShape");
	// findMoverByMouse selects vehicles by the stock screen rectangle, not triangles.
	if (g_mc2FrameCounter.load(std::memory_order_relaxed) == 60 && false)
		mc2::fmt::print_out("[VEHICLE_PICK] appearance={} pos=({:.2f},{:.2f}) bounds=({:.6f},{:.6f},{:.6f},{:.6f})\n",
			appearType->name, position.x, position.y, upperLeft.x, upperLeft.y, lowerRight.x, lowerRight.y);
	if (!appearType->renderLods[0] || status == OBJECT_STATUS_DESTROYED || status == OBJECT_STATUS_DISABLED) {
		renderShape.reset();
		renderLOD = -1;
		return;
	}
	int lod = 0;
	glm::vec3 translation;
	mc2ToRender(translation, position);
	const float eyeDistance = eye->usePerspective ? glm::length(translation - glm::vec3(eye->getCameraOrigin())) : 0.0f;
	for (int i = 1; i < 3; ++i)
		if (appearType->renderLods[i] && eyeDistance > appearType->renderLodDistance[i]) lod = i;
	const auto* override = ModelOverrideRegistry::instance().resolve("vehicle", appearType->name);
	TG_TypeMultiShape* variantType = nullptr;
	bool newVariant = false;
	const bool shared = override && override->usesShaderTeamPaint() && appearType->renderLods[lod];
	if (renderLOD != lod || (!shared && (renderRed != psRed || renderGreen != psGreen || renderBlue != psBlue))) {
		if (shared) variantType = appearType->renderLods[lod].get();
		else for (auto& variant : appearType->renderVariants)
			if (variant.lod == lod && variant.red == psRed && variant.green == psGreen && variant.blue == psBlue)
				variantType = variant.shape.get();
		if (!variantType) {
#ifdef ENABLE_ASSIMP_IMPORTER
			// A failed import is remembered per (type, lod): logged once, never retried, stock shape stays.
			static std::set<std::pair<std::string, int>> badLods;
			const auto badKey = std::make_pair(std::string(appearType->name), lod);
			if (badLods.count(badKey)) { renderShape.reset(); renderLOD = -1; return; }
			auto failLod = [&](std::string_view why) {
				badLods.insert(badKey);
				mc2::fmt::print_err("[VEHICLE_RENDER] lod import failed type={} lod={} ({}); using stock shape\n", appearType->name, lod, why);
				renderShape.reset(); renderLOD = -1;
			};
			const auto* lodSrc = !override ? nullptr : (lod == 0 ? &override->sourceRelPath : (static_cast<size_t>(lod - 1) < override->lods.size() ? &override->lods[lod - 1].sourceRelPath : nullptr));
			if (!lodSrc) { failLod("no override source"); return; }
			auto type = std::make_unique<TG_TypeMultiShape>();
			const auto path = (std::filesystem::path(override->manifestDir) / *lodSrc).generic_string();
			try {
				if (ImportGeometryFromFile(path.c_str(), type.get(), false, true, true) != 0) { failLod(path.c_str()); return; }
			} catch (...) { failLod("exception"); return; }
			variantType = type.get();
			appearType->renderVariants.push_back({lod, psRed, psGreen, psBlue, std::move(type)});
			newVariant = true;
#else
			return;
#endif
		}
		for (auto handle : renderTextures) if (handle != 0xffffffff) mc2::textures::release(handle);
		renderTextures.clear();
		renderShape.reset(variantType->CreateFrom());
		renderLOD = lod;
	}
	if (renderTextures.empty() || renderRed != psRed || renderGreen != psGreen || renderBlue != psBlue) {
		for (auto handle : renderTextures) if (handle != 0xffffffff) mc2::textures::release(handle);
		renderTextures.clear();
		auto colourKey = [](DWORD c) { return ((c >> 5) & 7) | (((c >> 13) & 7) << 3) | (((c >> 21) & 7) << 6); };
		const DWORD paintKey = 0x80000000u | (colourKey(psRed) << 18) | (colourKey(psGreen) << 9) | colourKey(psBlue);
		std::vector<std::pair<std::string, DWORD>> published;
		for (long i = 0; i < renderShape->GetNumTextures(); ++i) {
			if (variantType && !newVariant) {
				const auto handle = variantType->GetTextureHandle(i);
				renderTextures.push_back(handle == 0xffffffff ? handle : mc2::textures::retain(handle));
				continue;
			}
			char name[1024];
			renderShape->GetTextureName(i, name, sizeof(name));
			if (!name[0] || S_stricmp(name, "NULLTXM") == 0) {
				renderTextures.push_back(0xffffffff);
				continue;
			}
			char path[1024];
			mc2::fmt::to_buf(path, "{}{:d}/{}", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize), name);
			const bool alpha = S_strnicmp(name, "a_", 2) == 0;
			const auto format = alpha ? gos_Texture_Alpha : gos_Texture_Solid;
			const auto hints = gosHint_MipmapFilter0 | gosHint_DontShrink;
			bool tint = true;
			if (override) for (const auto& material : override->materials)
				if (material.slot == static_cast<uint32_t>(i))
					tint = material.materialClass != static_cast<uint32_t>(RenderCore::SurfaceClass::Emissive)
						&& material.materialClass != static_cast<uint32_t>(RenderCore::SurfaceClass::Glass);
			DWORD sharedPaint = 0xffffffff;
			if (tint) for (const auto& [textureName, textureHandle] : published)
				if (textureName == name) sharedPaint = textureHandle;
			if (sharedPaint != 0xffffffff) {
				renderTextures.push_back(mc2::textures::retain(sharedPaint));
				continue;
			}
			const auto variant = tint ? paintKey : 0;
			auto handle = mc2::textures::findAndRetain(path, format, hints, variant);
			const bool painted = handle != 0;
			if (!painted) handle = mc2::textures::load(path, format, hints, variant);
			// Reuse the stock key-colour classifier on paintable material slots.
			if (tint && !painted && handle != 0xffffffff) {
				auto* stock = gvShape;
				gvShape = renderShape.get();
				gvShape->SetTextureHandle(0, handle);
				setPaintScheme();
				gvShape = stock;
			}
			if (tint && handle != 0xffffffff) {
				const auto cpu = mc2::textures::sourceId(handle);
				TEXTUREPTR pixels{};
				gos_LockTexture(cpu, 0, 1, &pixels);
				// The batcher reads immutable source bytes, rather than the legacy CPU texture.
				const auto coloured = pixels.pTexture && pixels.Width == pixels.Height
					? mc2::textures::fromPixels(pixels.pTexture, format, hints, pixels.Width) : 0xffffffff;
				gos_UnLockTexture(cpu);
				if (coloured != 0xffffffff) {
					mc2::textures::release(handle);
					handle = coloured;
					published.emplace_back(name, handle);
				}
			}
			renderTextures.push_back(handle);
		}
		renderRed = psRed; renderGreen = psGreen; renderBlue = psBlue;
	}
	for (long i = 0; i < renderShape->GetNumTextures(); ++i)
		renderShape->SetTextureHandle(i, renderTextures[i]);
	// Batcher packets bind type materials. Cache painted types so both teams retain
	// independent albedo while sharing geometry within the same palette and LOD.
	if (newVariant) {
		for (long i = 0; i < variantType->GetNumTextures(); ++i) {
			const auto handle = renderTextures[i];
			variantType->SetTextureHandle(i, handle == 0xffffffff ? handle : mc2::textures::retain(handle));
			char name[256]; variantType->GetTextureName(i, name, sizeof(name));
			variantType->SetTextureAlpha(i, S_strnicmp(name, "a_", 2) == 0);
		}
		GpuStaticPropBatcher::instance().registerMultiShape(variantType, true, override);
		mc2::fmt::print_err("[VEHICLE_RENDER] {} lod={} paint={:08x}/{:08x}/{:08x} type_materials={}\n",
			appearType->name, lod, psRed, psGreen, psBlue, renderShape->usesTypeMaterials());
	}
	if (unitAnim().gvAnimationState >= 0) appearType->setAnimation(renderShape.get(), unitAnim().gvAnimationState);
	renderShape->SetFrameNum(unitAnim().currentFrame);
	auto turret = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, unitAnim().turretRotation * DEGREES_TO_RADS, 0.0f));
	const char* turretNode = appearType->rotationalNodeId;
	// Stock Centipede's INI says sc_turret; the actual stock/exported mesh names this node.
	if (S_stricmp(turretNode, "sc_turret") == 0) turretNode = "Centipede_turret";
	renderShape->SetNodeRotation(turretNode, &turret);
	renderShape->SetAlphaValue(gvShape->GetAlphaValue());
	renderShape->SetFogRGB(0xffffffff);
	// Owner 11-15: vehicles (repair truck included) are lit day and night; lights-out tracks
	// destroyed/disabled status exactly like the legacy gvShape (gvactor_update.cpp).
	renderShape->SetLightsOut(status == OBJECT_STATUS_DESTROYED || status == OBJECT_STATUS_DISABLED);
	auto rotation = poseRotation();
	// The static-prop batcher draws a type-material body from its node matrices; a declined submit falls back to the
	// CPU draw from the same matrices.
	renderShape->TransformMultiShape_HierarchyOnly(&translation, &rotation);
	if (!GpuStaticPropBatcher::instance().isMultiShapeEligibleForGpuObjects(renderShape.get()))
		renderShape->ReserveCpuDraw();
}


void GVAppearance::setPaintScheme (void)
{
    if (mc2::tick::defer(mc2::tick::Queue::Release, [this] { setPaintScheme(); })) return;
	DWORD gosHandle = mc2::textures::sourceId(gvShape->GetTextureHandle(0));
	const bool authoredPaint = renderShape && gvShape == renderShape.get();

	if (gosHandle && gosHandle != 0xffffffff)
	{
		TEXTUREPTR textureData;
		gos_LockTexture(gosHandle, 0, 0, &textureData);

		//-------------------------------------------------------
		// Dominant-channel paint classifier. A pixel belongs to a paint slot
		// iff its R/G/B dominates the other two by at least kRatio. Boundary
		// (antialiased) pixels blend tint vs original proportional to how
		// cleanly they dominate -- the original strict-zero check missed every
		// antialiased mask edge and every upscaled gradient pixel.
		// Bug fixed: inner loop was j < textureData.Height (should be Width).
		DWORD *textureMemory = textureData.pTexture;
		for (long i=0;i<textureData.Height;i++)
		{
			for (long j=0;j<textureData.Width;j++)
			{
				DWORD srcColor = *textureMemory;
				DWORD srcA = (srcColor & 0xff000000);
				int   srcR = static_cast<int>(((srcColor >> 16) & 0xff));
				int   srcG = static_cast<int>(((srcColor >>  8) & 0xff));
				int   srcB = static_cast<int>(( srcColor        & 0xff));

				int   slot       = -1;
				DWORD slotColor  = 0;
				float shade      = 0.0f;
				float mixFactor  = 0.0f;
				const int   kMinDom      = 16;
				// Authored baked paint is less saturated than the stock channel masks.
				const float kRatio       = authoredPaint ? 1.5f : 3.0f;
				const float kRatioMargin = authoredPaint ? 1.1f : 1.5f;

				int domChan = 0, maxOther = 0;
				if (srcR >= kMinDom && static_cast<float>(srcR) >= kRatio * static_cast<float>(srcG) && static_cast<float>(srcR) >= kRatio * static_cast<float>(srcB))
				{
					slot = 0; slotColor = psRed;
					domChan = srcR;
					maxOther = srcG > srcB ? srcG : srcB;
				}
				else if (srcG >= kMinDom && static_cast<float>(srcG) >= kRatio * static_cast<float>(srcR) && static_cast<float>(srcG) >= kRatio * static_cast<float>(srcB))
				{
					slot = 1; slotColor = psGreen;
					domChan = srcG;
					maxOther = srcR > srcB ? srcR : srcB;
				}
				else if (srcB >= kMinDom && static_cast<float>(srcB) >= kRatio * static_cast<float>(srcR) && static_cast<float>(srcB) >= kRatio * static_cast<float>(srcG))
				{
					slot = 2; slotColor = psBlue;
					domChan = srcB;
					maxOther = srcR > srcG ? srcR : srcG;
				}

				if (slot != -1)
				{
					float d = static_cast<float>(domChan) / 255.0f;
					shade = 1.0f - (1.0f - d) * (1.0f - d);

					float ratio = static_cast<float>(domChan) / static_cast<float>((maxOther + 1));
					float margin = (ratio - kRatio) / (kRatio * (kRatioMargin - 1.0f));
					margin = std::clamp(margin, 0.0f, 1.0f);
					mixFactor = margin;
				}

				DWORD newColor = srcColor;
				if (slot != -1 && mixFactor > 0.0f)
				{
					float tintR = static_cast<float>(((slotColor >> 16) & 0xff)) * shade;
					float tintG = static_cast<float>(((slotColor >>  8) & 0xff)) * shade;
					float tintB = static_cast<float>(( slotColor        & 0xff)) * shade;

					float outR = tintR * mixFactor + static_cast<float>(srcR) * (1.0f - mixFactor);
					float outG = tintG * mixFactor + static_cast<float>(srcG) * (1.0f - mixFactor);
					float outB = tintB * mixFactor + static_cast<float>(srcB) * (1.0f - mixFactor);

					unsigned char r = static_cast<unsigned char>((outR < 0.0f ? 0.0f : outR > 255.0f ? 255.0f : outR));
					unsigned char g = static_cast<unsigned char>((outG < 0.0f ? 0.0f : outG > 255.0f ? 255.0f : outG));
					unsigned char b = static_cast<unsigned char>((outB < 0.0f ? 0.0f : outB > 255.0f ? 255.0f : outB));

					newColor = srcA | (static_cast<DWORD>(r) << 16) | (static_cast<DWORD>(g) << 8) | static_cast<DWORD>(b);
				}

				*textureMemory = newColor;
				textureMemory++;
			}
		}

		gos_UnLockTexture(gosHandle);
	}
}	

DWORD bgrTorgb (DWORD frontRGB);

void GVAppearance::setPaintScheme (DWORD mcRed, DWORD mcGreen, DWORD mcBlue)
{
#ifdef BGR
	// These come into here bgr instead of RGB.  CONVERT!
	psRed = bgrTorgb(mcRed);
	psBlue = bgrTorgb(mcBlue);
	psGreen = bgrTorgb(mcGreen);
#else /*BGR*/
	psRed = mcRed;
	psBlue = mcBlue;
	psGreen = mcGreen;
#endif /*BGR*/

	setPaintScheme();	
}	

void GVAppearance::getPaintScheme( DWORD& red, DWORD& green, DWORD& blue )
{
#ifdef BGR
	red = bgrTorgb(psRed);
	blue = bgrTorgb(psBlue);
	green = bgrTorgb(psGreen);
#else /*BGR*/
	red = psRed;
	blue = psBlue;
	green = psGreen;
#endif /*BGR*/
}

void GVAppearance::resetPaintScheme (DWORD red, DWORD green, DWORD blue)
{
	
	if (localTextureHandle)
		mc2::textures::release(localTextureHandle);
	
	char txmName[1024];
	gvShape->GetTextureName(0,txmName,256);

   	char texturePath[1024];
   	mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));

	FullPathFileName legacyTextureName;
	legacyTextureName.init(texturePath,txmName,"");
	const auto pbrAlbedo = mc2::textures::stockPbrOverride(static_cast<const char*>(legacyTextureName), true, stockPbr);
	const char* textureName = pbrAlbedo.c_str();

	/* The texture manager asks for a unique 32bit identifier for every texture instance.
	However, it requires 72 bits to fully describe a mech texture (the base color (stored in
	the variable "red"), highlight color1 (blue), and highlight color2 (green), each of which
	is a 24bit number made up of 8bit r, g, and b components). Instead of creating a
	mapping between the mech textures used (probably less than 2^32 of them) and
	32bit identifiers, for the sake of expediency I'm just taking the 3 most significant bits of
	the 9 rgb components to make a 27 bit identifier. This means that two mech textures
	that are close in color (i.e. all of the 3 most significant bits of the 9 rgb components are
	the same) will be treated as the same texture, which is not necessarily a bad thing in
	our case.  */
	DWORD ccbase = ((red >> 5) & 7) + (((red >> 13) & 7) << 3) + (((red >> 21) & 7) << 6);
	DWORD cchighlight1 = ((green >> 5) & 7) + (((green >> 13) & 7) << 3) + (((green >> 21) & 7) << 6);
	DWORD cchighlight2 = ((blue >> 5) & 7) + (((blue >> 13) & 7) << 3) + (((blue >> 21) & 7) << 6);
	DWORD paintInstance = (ccbase << 18) + (cchighlight1 << 9) + (cchighlight2);
	
	if (fileExists(textureName))
	{
		if (S_strnicmp(txmName,"a_",2) == 0)
		{
			DWORD textureInstanceAlreadyExists = mc2::textures::findAndRetain(textureName,gos_Texture_Solid,gosHint_MipmapFilter0 | gosHint_DontShrink,paintInstance);
			if (!textureInstanceAlreadyExists)
				localTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_MipmapFilter0 | gosHint_DontShrink,paintInstance);
			else
				localTextureHandle = textureInstanceAlreadyExists;
				
			gvShape->SetTextureHandle(0,localTextureHandle);
			gvShape->SetTextureAlpha(0,true);
			
			if (textureInstanceAlreadyExists)
			{
				/* In this case, the texture returned should already have the paint scheme
				applied. */
	
				psRed = red;
				psGreen = green;
				psBlue = blue;
				return;
			}
		}
		else
		{
			DWORD textureInstanceAlreadyExists = mc2::textures::findAndRetain(textureName,gos_Texture_Solid,gosHint_MipmapFilter0 | gosHint_DontShrink,paintInstance);
			if (!textureInstanceAlreadyExists)
				localTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_MipmapFilter0 | gosHint_DontShrink,paintInstance);
			else
				localTextureHandle = textureInstanceAlreadyExists;
				
 			gvShape->SetTextureHandle(0,localTextureHandle);
			gvShape->SetTextureAlpha(0,false);
			if (textureInstanceAlreadyExists)
			{
				/* In this case, the texture returned should already have the paint scheme
				applied. */
	
				psRed = red;
				psGreen = green;
				psBlue = blue;
				return;
			}
		}
	}
	else
	{
		gvShape->SetTextureHandle(0,0xffffffff);
	}
	
	setPaintScheme(red,green,blue);
}	

void GVAppearance::setGesture (unsigned long gestureId)
{
	if (gestureId >= MAX_GV_ANIMATIONS)
		return;

	// The tick (gesture requests from GroundVehicle::updateAnimations) reads the sim status and leaves the draw shape alone;
	// updateGeometry applies the animation to gvShape before it draws.
	const bool simPass = simulationOwned() && !simPresentationPass;
	const long gestureStatus = simPass ? simStatus_ : status;
	if ((gestureStatus == OBJECT_STATUS_DESTROYED) || (gestureStatus == OBJECT_STATUS_DISABLED))
		return;
		
	if (!simPass)
		appearType->setAnimation(gvShape,gestureId);
	unitAnim().gvAnimationState = gestureId;
	unitAnim().currentFrame = 0.0f;
	if (appearType->gvStartF[gestureId])
		unitAnim().currentFrame = appearType->gvStartF[gestureId];
		
	unitAnim().isReversed = false;
	
	if (appearType->isReversed(unitAnim().gvAnimationState))
	{
		unitAnim().currentFrame = appearType->getNumFrames(unitAnim().gvAnimationState)-1;
		unitAnim().isReversed = true;
	}
	
	if (appearType->isRandom(unitAnim().gvAnimationState))
	{
		const long count = appearType->getNumFrames(unitAnim().gvAnimationState)-1;
		unitAnim().currentFrame = simPresentationPass || !simulationOwned() ?
			mc2_appearance_rng::randomNumber(count) : mc2_anim_rng::randomNumber(count);
	}
	
	unitAnim().isLooping = appearType->isLooped(unitAnim().gvAnimationState);
	
	unitAnim().gvFrameRate = appearType->getFrameRate(unitAnim().gvAnimationState);
	
	unitAnim().setFirstFrame = true;
	unitAnim().canTransition = false;
}

glm::vec3 GVAppearance::getHitNode (void)
{
	if (hitNodeId == -1)
		hitNodeId = gvShape->GetNodeNameId("hitnode");

	glm::vec3 result = getNodeIdPosition(hitNodeId);
 	return result;
}

// The tick's entry to the sim half of the appearance.
void GVAppearance::setSimInput (const SimInput& in)
{
	simPosition_ = in.position;
	simRotation_ = in.rotation;
	unitAnim().turretRotation = in.torsoRotation;
	unitAnim().pitch = in.pitch;
	unitAnim().roll = in.roll;
	if (in.hasStatus)
		simStatus_ = in.status;
}

void GVAppearance::setObjectParameters (const glm::vec3 &pos, float Rot, long sel, long team, long homeRelations)
{
	// Load and init paths seed the sim copy through here (then refreshSimPose); the tick uses setSimInput. As before, the
	// turret starts out facing the body heading.
	if (simulationOwned() && !simPresentationPass)
	{
		SimInput seed;
		seed.position = pos;
		seed.rotation = Rot;
		seed.torsoRotation = Rot;
		seed.pitch = unitAnim().pitch;
		seed.roll = unitAnim().roll;
		setSimInput(seed);
	}

	movedThisFrame = false;
	if ((rotation != Rot) || (!mc2::math::closeEnough(pos, position)))
		movedThisFrame = true;
	
	rotation = Rot;
	unitAnim().turretRotation = Rot;

	position = pos;

	selected = sel;

	actualRotation = Rot;

	teamId = team;
	homeTeamRelationship = homeRelations;
}

void GVAppearance::setMoverParameters (float turretRot, float lArmRot, float rArmRot, bool isAirborne)
{
	unitAnim().turretRotation = turretRot;
	unitAnim().pitch = lArmRot;
	unitAnim().roll = rArmRot;
	isInfantry = isAirborne;
}	

void GVAppearance::debugUpdate (void)
{
	if (!inDebugMoveMode)
		return;

	if (userInput->getKeyDown(KEY_LEFT) && userInput->ctrl())
	{
		vehicleDebugAngle += 11.25;
		if (vehicleDebugAngle > 180.0)
			vehicleDebugAngle -= 360;
	}

	if (userInput->getKeyDown(KEY_RIGHT) && userInput->ctrl())
	{
		vehicleDebugAngle -= 11.25;
		if (vehicleDebugAngle < -180.0)
			vehicleDebugAngle += 360.0;
	}

	if (userInput->getKeyDown(KEY_UP) && userInput->ctrl())
	{
		turretDebugAngle += 11.25;
		if (turretDebugAngle > 180.0)
			turretDebugAngle -= 360;
	}

	if (userInput->getKeyDown(KEY_DOWN) && userInput->ctrl())
	{
		turretDebugAngle -= 11.25;
		if (turretDebugAngle < -180.0)
			turretDebugAngle += 360.0;
	}

	if (userInput->getKeyDown(KEY_EQUALS) && userInput->ctrl())
	{
		debugVelMag += 10.0;
	}
						
	if (userInput->getKeyDown(KEY_MINUS) && userInput->ctrl())
	{
		debugVelMag -= 10.0;
	}

	if (userInput->getKeyDown(KEY_0) && userInput->ctrl())
	{
		debugVelMag = 0.0;
	}

	glm::vec3 velocity;
	velocity.x = 0.7071f;
	velocity.z = 0.0;
	velocity.y = -0.7071f;

	Rotate(velocity,-vehicleDebugAngle);

	float velMag = debugVelMag;

	glm::vec3 currentNormal = land->getTerrainNormal(debugGVActorPosition);
	float angle = angle_from(velocity,currentNormal);
	if (angle != 90.0)
	{
		float hillFactor = cos(angle * DEGREES_TO_RADS) * velMag;
		velMag += hillFactor;
	}

	velocity *= velMag * worldUnitsPerMeter;

	velocity *= frameLength;

	debugGVActorPosition += velocity;
	debugGVActorPosition.z = land->getTerrainElevation(debugGVActorPosition);

	setObjectParameters(debugGVActorPosition,vehicleDebugAngle,true,0, 0);
	update();
	recalcBounds();
}

bool GVAppearance::isMouseOver (float px, float py)
{
	if (inView)
	{
		if ((px <= lowerRight.x) && (py <= lowerRight.y) &&
			(px >= upperLeft.x) &&
			(py >= upperLeft.y))
		{
			return inView;
		}
		else
		{
			return FALSE;
		}
	}
	
	return(inView);
}	

bool GVAppearance::recalcBounds (void)
{
	// F3 CPU projection cost-baseline: aggregate per-actor scope into the
	// recalcBounds_perframe bucket. No-op when env OFF.
	::mc2_cpu_proj_cost::Scope _f3_recalcBounds_scope(
	    ::mc2_cpu_proj_cost::BucketId::BUCKET_RECALCBOUNDS_PERFRAME);
	::mc2_cpu_proj_cost::add_workload_recalcbounds(1);
	Stuff::Vector4D tempPos;
	setVisibilityGatesFromLegacy(false);

	float eyeDistance = 0.0f;

	if (eye)
	{
		//ALWAYS need to do this or select is YAYA
		// [PROJECTZ:ScreenXYOracle id=gvactor_screen_pos]
		eye->projectForScreenXY(position,screenPos);
		
		if (eye->usePerspective)
		{
			glm::vec3 Distance;
			glm::vec3 objPosition;
			glm::vec3 eyePosition(eye->getCameraOrigin());
			mc2ToRender(objPosition, position);
	
			Distance = objPosition - eyePosition;
			eyeDistance = glm::length(Distance);
			if (eyeDistance > Camera::MaxClipDistance)
			{
				hazeFactor = 1.0f;
				setVisibilityGatesFromLegacy(false);
			}
			else if (eyeDistance > Camera::MinHazeDistance)
			{
				Camera::HazeFactor = Camera::fogHaze(eyeDistance);
				setVisibilityGatesFromLegacy(true);
			}
			else
			{
				Camera::HazeFactor = 0.0f;
				setVisibilityGatesFromLegacy(true);
			}
			
			if (inView)
			{
				glm::vec3 Distance;
				glm::vec3 objPosition;
				glm::vec3 eyePosition(eye->getCameraOrigin());
				mc2ToRender(objPosition, position);
		
				Distance = objPosition - eyePosition;
				Distance = glm::normalize(Distance);
				
				float cosine = glm::dot(Distance, eye->getLookVector());
 				if (cosine > eye->cosHalfFOV)
					setVisibilityGatesFromLegacy(true);
				else
					setVisibilityGatesFromLegacy(false);
			}
		}
		else
		{
			Camera::HazeFactor = 0.0f;
			setVisibilityGatesFromLegacy(true);
		}
		
		if (inView)
		{
			if (reloadBounds)
				appearType->reinit();

			appearType->boundsLowerRightY = (OBBRadius * eye->getTiltFactor() * 2.0f);
			
			//-------------------------------------------------------------------------
			// do a rough check if on screen.  If no where near, do NOT do the below.
			// Mighty mighty slow!!!!
			// Use the original check done before all this 3D madness.  Dig out sourceSafe tomorrow!
			tempPos = screenPos;
			upperLeft.x = tempPos.x;
			upperLeft.y = tempPos.y;
			
			lowerRight.x = tempPos.x;
			lowerRight.y = tempPos.y;
			
			upperLeft.x += (appearType->boundsUpperLeftX * eye->getScaleFactor());
			upperLeft.y += (appearType->boundsUpperLeftY * eye->getScaleFactor());
	
			lowerRight.x += (appearType->boundsLowerRightX * eye->getScaleFactor());
			lowerRight.y += (appearType->boundsLowerRightY * eye->getScaleFactor());

			if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
				(upperLeft.x <= eye->getScreenResX()) &&
				(upperLeft.y <= eye->getScreenResY()))
			{
				glm::vec3 boxCoords[8];
				Stuff::Vector4D bcsp[8];

				glm::vec3 minBox;
				mc2ToRender(minBox, appearType->typeUpperLeft);
				
				glm::vec3 maxBox;
				mc2ToRender(maxBox, appearType->typeLowerRight);
				
				if (rotation != 0.0f)
					Rotate(minBox,-rotation);
 	
				if (rotation != 0.0f)
					Rotate(maxBox,-rotation);
					
 				boxCoords[0].x = position.x + minBox.x;
				boxCoords[0].y = position.y + minBox.y;
				boxCoords[0].z = position.z + minBox.z;
	
				boxCoords[1].x = position.x + minBox.x;
				boxCoords[1].y = position.y + maxBox.y;
				boxCoords[1].z = position.z + minBox.z;
 	
				boxCoords[2].x = position.x + maxBox.x;
				boxCoords[2].y = position.y + minBox.y;
				boxCoords[2].z = position.z + minBox.z;
    
				boxCoords[3].x = position.x + maxBox.x;
				boxCoords[3].y = position.y + maxBox.y;
				boxCoords[3].z = position.z + minBox.z;
    
				boxCoords[4].x = position.x + maxBox.x;
				boxCoords[4].y = position.y + maxBox.y;
				boxCoords[4].z = position.z + maxBox.z;
    
				boxCoords[5].x = position.x + maxBox.x;
				boxCoords[5].y = position.y + minBox.y;
				boxCoords[5].z = position.z + maxBox.z;
    
				boxCoords[6].x = position.x + minBox.x;
				boxCoords[6].y = position.y + maxBox.y;
				boxCoords[6].z = position.z + maxBox.z;
 	
				boxCoords[7].x = position.x + minBox.x;
				boxCoords[7].y = position.y + minBox.y;
				boxCoords[7].z = position.z + maxBox.z;
 	
				mc2snap::liftBoxZ(boxCoords, visualDelta_);   // UNIT-VISUAL-SNAP-2: selection box follows the drawn mesh
				float maxX = 0.0f, maxY = 0.0f;
				float minX = 0.0f, minY = 0.0f;

				for (long i=0;i<8;i++)
				{
					// [PROJECTZ:ScreenXYOracle id=gvactor_box_rect]
					eye->projectForScreenXY(boxCoords[i],bcsp[i]);
					if (!i)
					{
						maxX = minX = bcsp[i].x;
						maxY = minY = bcsp[i].y;
					}
					
					if (i)
					{
						maxX = std::max(maxX, bcsp[i].x);
						minX = std::min(minX, bcsp[i].x);
						maxY = std::max(maxY, bcsp[i].y);
						minY = std::min(minY, bcsp[i].y);
					}
				}
		
				upperLeft.x = minX;
				upperLeft.y = minY;
				lowerRight.x = maxX;
				lowerRight.y = maxY;
				
				if ((lowerRight.x >= 0) && (lowerRight.y >= 0) &&
					(upperLeft.x <= eye->getScreenResX()) &&
					(upperLeft.y <= eye->getScreenResY()))
				{
					setVisibilityGatesFromLegacy(true);
					if ((status != OBJECT_STATUS_DESTROYED) && (status != OBJECT_STATUS_DISABLED))
					{
						//-------------------------------------------------------------------------------
						//Set LOD of Model here because we have the distance and we KNOW we can see it!
						bool baseLOD = true;
						DWORD selectLOD = 0;
						if (useHighObjectDetail)
						{
							for (long i=1;i<MAX_LODS;i++)
							{
								if (appearType->gvShape[i] && (eyeDistance > appearType->lodDistance[i]))
								{
									baseLOD = false;
									selectLOD = i;
								}
							}
						}
						else	//We always want to use the lowest LOD!!
						{
							if (appearType->gvShape[1])
							{
								baseLOD = false;
								selectLOD = 1;
							}
						}
						
						if (selectLOD != currentLOD)
						{
							currentLOD = selectLOD;

							BYTE alphaValue = gvShape->GetAlphaValue();
							gvShape->ClearAnimation();
							gvShapeOwner_.reset();
							gvShape = nullptr;

							gvShapeOwner_.reset(appearType->gvShape[currentLOD]->CreateFrom());
							gvShape = gvShapeOwner_.get();
							if (unitAnim().gvAnimationState != -1)
								appearType->setAnimation(gvShape,unitAnim().gvAnimationState);

							gvShape->SetAlphaValue(alphaValue);

							for (long j=0;j<gvShape->GetNumTextures();j++)
							{
								char txmName[1024];
								gvShape->GetTextureName(j,txmName,256);

								char texturePath[1024];
								mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));

								FullPathFileName textureName;
								textureName.init(texturePath,txmName,"");

								if (fileExists(textureName))
								{
									if (S_strnicmp(txmName,"a_",2) == 0)
									{
										DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink);
										gosASSERT(gosTextureHandle != 0xffffffff);
										gvShape->SetTextureHandle(j,gosTextureHandle);
										gvShape->SetTextureAlpha(j,true);
									}
									else
									{
										DWORD gosTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink);
										gosASSERT(gosTextureHandle != 0xffffffff);
										gvShape->SetTextureHandle(j,gosTextureHandle);
										gvShape->SetTextureAlpha(j,false);
									}
								}
								else
								{
									gvShape->SetTextureHandle(j,0xffffffff);
								}
							}
							DWORD r, g, b;
							getPaintScheme(r, g, b);
							resetPaintScheme(r,g,b);
						}

						if (currentLOD && baseLOD)
						{
							currentLOD = 0;
							
							BYTE alphaValue = gvShape->GetAlphaValue();
							gvShape->ClearAnimation();
							gvShapeOwner_.reset();
							gvShape = nullptr;
							
							gvShapeOwner_.reset(appearType->gvShape[currentLOD]->CreateFrom());
							gvShape = gvShapeOwner_.get();
							gvShape->SetAlphaValue(alphaValue);
							
							for (long i=0;i<gvShape->GetNumTextures();i++)
							{
								char txmName[1024];
								gvShape->GetTextureName(i,txmName,256);
										
								char texturePath[1024];
								mc2::fmt::to_buf(texturePath, "{}{:d}" PATH_SEPARATOR "", mc2::fmt::safe(tglPath), mc2::fmt::as_i32(ObjectTextureSize));
						
								FullPathFileName textureName;
								textureName.init(texturePath,txmName,"");
										
								if (fileExists(textureName))
								{
									if (S_strnicmp(txmName,"a_",2) == 0)
									{
										localTextureHandle = mc2::textures::load(textureName,gos_Texture_Alpha,gosHint_DisableMipmap | gosHint_DontShrink,true);
										gosASSERT(localTextureHandle != 0xffffffff);
										gvShape->SetTextureHandle(i,localTextureHandle);
										gvShape->SetTextureAlpha(i,true);
									}
									else
									{
										localTextureHandle = mc2::textures::load(textureName,gos_Texture_Solid,gosHint_DisableMipmap | gosHint_DontShrink,true);
										gosASSERT(localTextureHandle != 0xffffffff);
										gvShape->SetTextureHandle(i,localTextureHandle);
										gvShape->SetTextureAlpha(i,false);
									}
								}
								else
								{
									gvShape->SetTextureHandle(i,0xffffffff);
								}
							}

							DWORD r, g, b;
							getPaintScheme(r, g, b);
							resetPaintScheme(r,g,b);
						}					
					}
				}
				else
				{
					setVisibilityGatesFromLegacy(false);		//Did alot of extra work checking this, but WHY draw and insult to injury?
				}
			}
			else
			{
				setVisibilityGatesFromLegacy(false);
			}
		}
	}
	
	return(inView);
}

bool GVAppearance::playDestruction (void)
{
	if (simulationOwned() && !simPresentationPass)
	{
		// Tick: record the request (presentEffects plays it) and answer from data only.
		const bool available = appearType->destructEffect[0] &&
			gosFX::EffectLibrary::Instance->Find(appearType->destructEffect);
		if (available) ++unitAnim().destructionSerial;
		return available;
	}
	if (appearType->destructEffect[0])
	{
		unsigned flags = gosFX::Effect::ExecuteFlag;

		Check_Object(gosFX::EffectLibrary::Instance);
		gosFX::Effect::Specification* gosEffectSpec = gosFX::EffectLibrary::Instance->Find(appearType->destructEffect);
		
		if (gosEffectSpec)
		{
			destructFX = gosFX::EffectLibrary::Instance->MakeEffect(gosEffectSpec->m_effectID, flags);
			gosASSERT(destructFX != nullptr);
		
			MidLevelRenderer::MLRTexturePool::Instance->LoadImages();
		
			glm::vec3			tPosition;
			glm::mat4 	shapeOrigin;
			glm::mat4	localToWorld;
			
			mc2ToRender(tPosition, position);
			
			mc2::math::buildRotation(shapeOrigin, mc2::math::Euler(0.0f,0.0f,0.0f));
			mc2::math::buildTranslation(shapeOrigin, tPosition);
			
			gosFX::Effect::ExecuteInfo info(static_cast<Stuff::Time>(scenarioTime),&shapeOrigin,nullptr);
			destructFX->Start(&info);
			
			return true;
		}
		
		return false;
	}
	
	return false;		//We didn't have a destruct effect.  Tell the object to play its default.
}

glm::vec3 GVAppearance::getNodeNamePosition (mc2::czstring nodeName)
{
    if (!simPresentationPass && simPose->ready) {
        const auto node = simulationShape_ ? simulationShape_->GetNodeNameId(nodeName) : -1;
        return node >= 0 && static_cast<size_t>(node) < simPose->shapeNodes.size()
            ? simPose->shapeNodes[node] : simPose->root;
    }
	GV_POSE_VIEW_LOCALS
	glm::vec3 result = position;

	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().turretRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&torsoRot);

	gvShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
   
	result = mc2snap::unliftNodeZ(gvShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeName), pv_.lift);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}

glm::vec3 GVAppearance::getNodeIdPosition (long nodeId)
{
    if (!simPresentationPass && simPose->ready) return nodeId >= 0 && static_cast<size_t>(nodeId) < simPose->shapeNodes.size() ? simPose->shapeNodes[nodeId] : simPose->root;

	GV_POSE_VIEW_LOCALS
	glm::vec3 result = position;

	// C3: route to GPU-lagged visibility when killswitch is enabled.
	if (s_gpuCullLifecycle) {
		if (!gpu_cull::readback_isActorVisibleLagged(static_cast<uint32_t>(actorHandle_)))
			return result;
	} else {
		if (!inView)
			return result;
	}

   	glm::quat qRotation;
   	float yaw = rotation * DEGREES_TO_RADS;
   	qRotation = mc2::math::quatFromEuler(mc2::math::Euler(0.0f, yaw, 0.0f));
   
   	glm::vec3 xlatPosition;
   	mc2ToRender(xlatPosition, position);
   
   	glm::quat torsoRot;
   	torsoRot = mc2::math::quatFromEuler(mc2::math::Euler(0.0f,(unitAnim().turretRotation * DEGREES_TO_RADS),0.0f));
	if (rotationalNodeIndex == -1)
	   	rotationalNodeIndex = gvShape->SetNodeRotation(appearType->rotationalNodeId,&torsoRot);

	gvShape->SetNodeRotation(rotationalNodeIndex,&torsoRot);
   
	result = mc2snap::unliftNodeZ(gvShape->GetTransformedNodePosition(&xlatPosition,&qRotation,nodeId), pv_.lift);

	if ((result.x == 0.0f) &&
		(result.y == 0.0f) && 
		(result.z == 0.0f))
		result = position;
		
	return result;
}
