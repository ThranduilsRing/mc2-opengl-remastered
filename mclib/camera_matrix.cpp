// Camera: the camera transform: activation, orthogonal setup, line of sight, origin, GL matrices and projection constants.
// Declared in the class headers; shared names in camera_internal.h.
#include "camera_internal.h"


namespace {
// XFORM-CONVENTION-HARNESS-1: the two canonical clip-space convention
// matrices used to live here as anonymous-namespace free functions. They were
// lifted verbatim into the GL-free leaf TU mclib/xform_conventions.cpp so a
// host unit test (tests/unit/test_xform_convention.cpp) can link them without
// pulling GL-coupled camera.cpp. The full rationale for the sign/handedness
// choices is preserved at the new TU's function definitions. camera.cpp keeps
// its two file-scope constants and simply initializes them from the lifted
// functions; the hot paths below are byte-identical to before.
const Stuff::Matrix4D kAxisSwapMC2toGL   = mc2xform::makeAxisSwapMC2toGL();
const Stuff::Matrix4D kPixelHomogToGLNDC = mc2xform::makePixelHomogToGLNDC();
} // namespace

//---------------------------------------------------------------------------
long Camera::activate (void)
{
	//------------------------------------------
	// If camera is already active, just return
	if (ready && active)
	{
		updateDaylight(true);
		return(NO_ERR);
	}
	
 	return NO_ERR;
}

float zero = 0.0f;
float one = 1.0f;
float point1 = 0.000001f;

//---------------------------------------------------------------------------
void Camera::projectCamera (Stuff::Vector3D &point)
{
	//--------------------------------------------------------------------
	// This is already in -x,z,y reference frame.
	point *= worldToCameraMatrix;
}
 
//------------------------------------------------------------------------------
void Camera::setOrthogonal(void)
{
	if (!usePerspective)
	{
		//--------------------
		//Parallel Projection
		float invCamScale = 1.0 / newScaleFactor;
		
		float left_clip, right_clip,top_clip,bottom_clip,far_clip,near_clip;
		
		left_clip = invCamScale * 300.0;
		top_clip = left_clip * ((float)Environment.screenHeight / (float)Environment.screenWidth);
		right_clip = -invCamScale * 300.0;
		bottom_clip = right_clip * ((float)Environment.screenHeight / (float)Environment.screenWidth);
		near_clip = -2000.0;
		far_clip = 8000.0;
		// EDITOR-FAR-CLIP-1 (parallel/ortho mirror): the editor zooms the eye far
		// outside the map, so terrain crosses the FAR side of this orthographic depth
		// slab (stock far = 8000) and gets clipped in the GPU-indirect pzOk gate
		// (u_worldToClipGL). Extend ONLY the far plane in the editor; leave near at
		// -2000 (widening near broke the reverse-Z depth window). Game byte-identical.
		{
			extern bool InEditor;
			if (InEditor) far_clip = 400000.0;
		}

		//
		//------------------------------------------------------------------------
		// Set up the camera to clip matrix.  This matrix takes camera space
		// coordinates and maps them into a homogeneous culling space where valid
		// X, Y, and Z axis values (when divided by W) will all be between 0 and 1
		//------------------------------------------------------------------------
		//
		cameraToClip(LEFT_AXIS, LEFT_AXIS) = 1.0f / (left_clip-right_clip);
		cameraToClip(LEFT_AXIS, UP_AXIS) = 0.0f;
		cameraToClip(LEFT_AXIS, FORWARD_AXIS) = 0.0f;
		cameraToClip(LEFT_AXIS, 3) = 0.0f;

		cameraToClip(UP_AXIS, LEFT_AXIS) = 0.0f;
		cameraToClip(UP_AXIS, UP_AXIS) = 1.0f / (top_clip-bottom_clip);
		cameraToClip(UP_AXIS, FORWARD_AXIS) = 0.0f;
		cameraToClip(UP_AXIS, 3) = 0.0f;

		cameraToClip(FORWARD_AXIS, LEFT_AXIS) = 0.0;
		cameraToClip(FORWARD_AXIS, UP_AXIS) = 0.0;
		// Reverse-Z (U1): parallel/ortho z:[near,far]->NDC[1,0] (was [0,1]).
		cameraToClip(FORWARD_AXIS, FORWARD_AXIS) = -1.0f / (far_clip-near_clip);
		cameraToClip(FORWARD_AXIS, 3) = 0.0;

		cameraToClip(3, LEFT_AXIS) = -right_clip / (left_clip-right_clip);
		cameraToClip(3, UP_AXIS) = -bottom_clip / (top_clip-bottom_clip);
		// Reverse-Z (U1): translate term flips sign and uses far_clip.
		cameraToClip(3, FORWARD_AXIS) = far_clip / (far_clip-near_clip);
		cameraToClip(3, 3) = 1.0f;
		// F2 unified-projection: keep GL-native product in sync with cameraToClip.
		cameraToClipGL.Multiply(cameraToClip, kPixelHomogToGLNDC);
	}
	else
	{
		//-----------------------
		//Perspective Projection
		//

		float far_clip,near_clip;
		near_clip = Camera::NearPlaneDistance;
		far_clip = Camera::FarPlaneDistance;

		// EDITOR-FAR-CLIP-1: the editor zooms the eye far outside the map, so the
		// terrain crosses the stock far plane (61555) and gets depth-clipped in the
		// GPU-indirect path (u_worldToClipGL) -> "can't see past some distance".
		// Extend the far plane in the EDITOR by default (InEditor) so the whole map
		// draws from any zoom. Reverse-Z keeps depth precision fine across the larger
		// range. MC2_EDITOR_FAR_CLIP=<value> overrides; the game is unaffected.
		// LIVE InEditor check (NOT static const): setOrthogonal runs during early
		// camera init, BEFORE InitializeGameEngine() sets InEditor=true, so a
		// static-const capture froze this at false and the override never applied
		// (measured: far map corners stayed beyond the small scaled far plane).
		extern bool InEditor;
		float editorFar = 0.0f;
		{
			if (mc2::gate::isNonEmpty<"MC2_EDITOR_FAR_CLIP">()) { float f = mc2::gate::get<"MC2_EDITOR_FAR_CLIP">(); editorFar = (f > 1000.0f) ? f : 400000.0f; }
			else if (InEditor) editorFar = 400000.0f;
		}
		if (editorFar > far_clip)
			far_clip = editorFar;

		float horizontal_fov = camera_fov * DEGREES_TO_RADS;
		// UI-ASPECT-ANCHOR-1 (camera): build the projection with the REAL
		// display aspect, not the HUD-clamped 800x600 (4:3). The clamped ratio
		// baked a 4:3 frustum that the full-window stretch then distorted at
		// any other aspect (16:9 = fat mechs; 25:9 = squashed scene). Pick math
		// shares this matrix (project/inverse through the same constants), so
		// screen<->world stays consistent. MC2_CAMERA_ASPECT_NATIVE=0 restores
		// the legacy stretched look.
		float height2width = ((float)Environment.screenHeight / (float)Environment.screenWidth);
		{
			static const bool s_nativeAspect = mc2::gate::get<"MC2_CAMERA_ASPECT_NATIVE">();
			// Panel previews (SimpleCamera -> fixed 800x600 FBO) must keep the
			// clamped ratio: their render target IS 4:3, not the window.
			extern int g_mechPreviewRenderDepth;
			const bool usingNative = s_nativeAspect && g_mechPreviewRenderDepth == 0
				&& Environment.drawableWidth > 0 && Environment.drawableHeight > 0;
			if (usingNative)
				height2width = (float)Environment.drawableHeight / (float)Environment.drawableWidth;
			// WATER-ASPECT-CORRECT-2: mirror this frame's choice so downstream
			// consumers (the water MDI shader's dFdx(nz) aspect compensation,
			// gameos_graphics.cpp) can tell whether the projection this frame
			// actually matches the native drawable aspect (true, the default) or
			// is still the legacy 4:3-locked frustum (MC2_CAMERA_ASPECT_NATIVE=0
			// or a mech-preview FBO pass). See g_cameraProjAspectNative below.
			g_cameraProjAspectNative = usingNative;
		}

		// META-FIX design D (MC2_RES_ASSERT) chokepoint (i): the perspective
		// frustum aspect must equal the drawable aspect whenever the native-
		// aspect override is active (issue #45 class — regression here bakes a
		// 4:3 frustum that the full-window stretch distorts). Skip conditions
		// re-derived INDEPENDENTLY of the override block above, so a regression
		// there cannot simultaneously disarm this audit. Default OFF = one
		// static bool test (tier1 byte-identical).
		if (mc2res::armed())
		{
			static const bool s_resAuditNativeAspect = mc2::gate::get<"MC2_CAMERA_ASPECT_NATIVE">();
			extern int g_mechPreviewRenderDepth;
			if (s_resAuditNativeAspect && g_mechPreviewRenderDepth == 0
				&& Environment.drawableWidth > 0 && Environment.drawableHeight > 0)
			{
				const float expectedH2W =
					(float)Environment.drawableHeight / (float)Environment.drawableWidth;
				if (fabsf(height2width - expectedH2W) >= 1e-3f)
				{
					char resDetail[192];
					snprintf(resDetail, sizeof(resDetail),
						"h2w=%.5f expected=%.5f drawable=%dx%d screen=%dx%d",
						(double)height2width, (double)expectedH2W,
						Environment.drawableWidth, Environment.drawableHeight,
						Environment.screenWidth, Environment.screenHeight);
					mc2res::violation("camera_aspect", resDetail);
				}
			}
		}

		//
		//-------------------------------------------------------
		// Calculate the horizontal, vertical, and forward ranges
		//-------------------------------------------------------
		//
		float left_clip = -(float)(near_clip * tan(horizontal_fov*0.5f));
		float top_clip = -left_clip * height2width;
		float bottom_clip = -top_clip;
		float right_clip = -left_clip;
		
		float horizontal_range = APPLY_LEFT_SIGN(1.0f) / (left_clip - right_clip);
		float vertical_range = APPLY_UP_SIGN(1.0f) / (top_clip - bottom_clip);
		float depth_range = APPLY_FORWARD_SIGN(1.0f) / (far_clip - near_clip);
		
 		//
		//------------------------------------------------------------------------
		// Set up the camera to clip matrix.  This matrix takes camera space
		// coordinates and maps them into a homogeneous culling space where valid
		// X, Y, and Z axis values (when divided by W) will all be between 0 and 1
		//------------------------------------------------------------------------
		//
		cameraToClip(LEFT_AXIS, LEFT_AXIS) = near_clip * horizontal_range;
		cameraToClip(LEFT_AXIS, UP_AXIS) = 0.0f;
		cameraToClip(LEFT_AXIS, FORWARD_AXIS) = 0.0f;
		cameraToClip(LEFT_AXIS, 3) = 0.0f;
	
		cameraToClip(UP_AXIS, LEFT_AXIS) = 0.0f;
		cameraToClip(UP_AXIS, UP_AXIS) = near_clip * vertical_range;
		cameraToClip(UP_AXIS, FORWARD_AXIS) = 0.0f;
		cameraToClip(UP_AXIS, 3) = 0.0f;
	
		cameraToClip(FORWARD_AXIS, LEFT_AXIS) = -right_clip * horizontal_range;
		cameraToClip(FORWARD_AXIS, UP_AXIS) = -bottom_clip * vertical_range;
		// Reverse-Z (U1): for NDC=(A*z+B)/z, near->1/far->0 gives
		// A=-near/(far-near), B=+near*far/(far-near). depth_range already
		// carries 1/(far-near) and the axis sign, so: negate-and-near-swap
		// the FF term, sign-flip the 3F term.
		cameraToClip(FORWARD_AXIS, FORWARD_AXIS) = -near_clip * depth_range;
		cameraToClip(FORWARD_AXIS, 3) = 1.0f;

		cameraToClip(3, LEFT_AXIS) = 0.0f;
		cameraToClip(3, UP_AXIS) = 0.0f;
		cameraToClip(3, FORWARD_AXIS) = far_clip * near_clip * depth_range;
		cameraToClip(3, 3) = 0.0f;
		// F2 unified-projection: keep GL-native product in sync with cameraToClip.
		cameraToClipGL.Multiply(cameraToClip, kPixelHomogToGLNDC);

		// [REVERSE_Z v1] env-gated lifecycle print (MC2_REVERSE_Z_TRACE=1),
		// silent by default; one-shot via a static latch. Matches the
		// MC2_DEBUG_SHADOW_* pattern (CLAUDE.md Debug instrumentation rule;
		// structural depth change). Samples the perspective z-row at the
		// near and far planes: under reverse-Z near must map to NDC z=1 and
		// far to NDC z=0. NDC z = (FF*z_cam + B) / z_cam, with FF and B the
		// just-written cameraToClip terms.
		static const bool s_rzTrace = mc2::gate::get<"MC2_REVERSE_Z_TRACE">();
		if (s_rzTrace) {
			static int s_rzBuild = 0;
			++s_rzBuild;
			const float FF = (float)cameraToClip(FORWARD_AXIS, FORWARD_AXIS);
			const float B  = (float)cameraToClip(3, FORWARD_AXIS);
			const float ndcNear = (FF * near_clip + B) / near_clip;
			const float ndcFar  = (FF * far_clip  + B) / far_clip;
			fprintf(stderr,
				"[REVERSE_Z v1] event=proj_build n=%d mode=perspective "
				"near=%.4f far=%.4f FF=%.8f B=%.8f "
				"ndcZ_near=%.6f(expect~1) ndcZ_far=%.6f(expect~0)\n",
				s_rzBuild, near_clip, far_clip, FF, B, ndcNear, ndcFar);
		}

		// [WATER_ASPECT_DIAG v1] env-gated one-shot (MC2_WATER_ASPECT_DIAG=1),
		// silent by default. Confirms the projection-vs-viewport aspect mismatch
		// suspected behind the "water dark/blown/noisy at odd desktop resolution"
		// issue. The perspective frustum aspect here comes ONLY from
		// Environment.screenWidth/Height (force-clamped to 800x600 by the
		// HUD-RES-CLAMP, gameos_graphics.cpp), while the scene FBO + glViewport
		// rasterize at Environment.drawableWidth/Height (native FULLSCREEN_DESKTOP
		// drawable). When those aspects differ, geometry is stretched in X by
		// (vpAspect/projAspect) and the water FS screen-space derivatives
		// (dFdx/dFdy) go anisotropic. stretchX != 1.0 => mismatch present.
		static const bool s_waAspect = mc2::gate::get<"MC2_WATER_ASPECT_DIAG">();
		if (s_waAspect) {
			const float projAspect = (float)Environment.screenWidth /
			                         (float)(Environment.screenHeight ? Environment.screenHeight : 1);
			const float vpAspect   = (float)Environment.drawableWidth /
			                         (float)(Environment.drawableHeight ? Environment.drawableHeight : 1);
			const float stretchX   = projAspect != 0.0f ? (vpAspect / projAspect) : 0.0f;
			fprintf(stderr,
				"[WATER_ASPECT_DIAG v1] proj=%dx%d (aspect=%.4f, 4:3=1.333) "
				"viewport=%dx%d (aspect=%.4f) stretchX=%.4f (1.0=match) "
				"dFdx/dFdy_aniso~=%.4f\n",
				Environment.screenWidth, Environment.screenHeight, projAspect,
				Environment.drawableWidth, Environment.drawableHeight, vpAspect,
				stretchX, stretchX != 0.0f ? (1.0f / stretchX) : 0.0f);
		}
	}
}

extern bool useLOSAngle;
extern float	worldUnitsPerMeter;
Stuff::Vector3D actualPosition;

#define MOVE_IN_INC			50.0f
#define ELEVATION_BUFFER	125.0f
#define ANGLE_UP_INC		0.25f

//-----------------------------------------------------------------------------------------------
bool CameraLineOfSight (Stuff::Vector3D position, Stuff::Vector3D targetPosition) 
{
	int posCellR, posCellC;
	int tarCellR, tarCellC;
	land->worldToCell(position, posCellR, posCellC);
	land->worldToCell(targetPosition, tarCellR, tarCellC);
	
	//-----------------------------------------------------
	// Once we allow teams to have alliances (for contacts,
	// etc.), simply set all nec. team bits in this mask...

	//------------------------------------------------------------------------------------------
	// Within magic radius.  Check REAL LOS now.
	// Check is really simple.
	// Find deltaCellRow and deltaCellCol and iterate over them from source to dest.
	// If the magic line ever goes BELOW the terrainElevation PLUS localElevation return false.
	Stuff::Vector3D startPos, endPos;
	startPos.Zero();
	endPos.Zero();
	
	int tCellRow = tarCellR, tCellCol = tarCellC;
	int mCellRow = posCellR, mCellCol = posCellC;
	
	land->getCellPos(tCellRow,tCellCol,endPos);
	startPos = position;
	
//	land->getCellPos(mCellRow,mCellCol,startPos);
//	startPos.z = position.z;

	Stuff::Vector3D deltaCellVec;
	deltaCellVec.y = tCellRow - mCellRow;
	deltaCellVec.x = tCellCol - mCellCol;
	deltaCellVec.z = 0.0f;
	float startHeight = startPos.z;
	
	float length = deltaCellVec.GetApproximateLength();
	
	if (length > Stuff::SMALL)
	{
		float colLength = deltaCellVec.x / length;
		float rowLength = deltaCellVec.y / length;
		float heightLen = (endPos.z - startPos.z) / length;
		
		float lastCol = fabs(colLength * 5.0);
		float lastRow = fabs(rowLength * 5.0);
		
		float startCellRow = mCellRow;
		float startCellCol = mCellCol;
		
		float endCellRow = tCellRow;
		float endCellCol = tCellCol;
		
		Stuff::Vector3D currentPos = startPos;
		Stuff::Vector3D dist;
		dist.Subtract(endPos,currentPos);
		
		bool colDone = false, rowDone = false;
		while (!colDone || !rowDone)
		{
			if (fabs(startCellRow - endCellRow) > lastRow)	//DO NOT INCLUDE LAST CELL!!!!!
			{
				startCellRow += rowLength;
			}
			else
			{
				rowDone = true;
			}
				
			if (fabs(startCellCol - endCellCol) > lastCol)	//DO NOT INCLUDE LAST CELL!!!!!
			{
				startCellCol += colLength;
			}
			else
			{
				colDone = true;
			}

			startHeight += heightLen;
			
			int startCellC = startCellCol;
			int startCellR = startCellRow;

			land->getCellPos(startCellR,startCellC,currentPos);
			//float localElev = (worldUnitsPerMeter * 4.0f * (float)GameMap->getLocalHeight(startCellR,startCellC));
			//currentPos.z += localElev;
			
			if (startHeight < currentPos.z)
			{
				return false;
			}
		}
	}
	
	return true;
}

//------------------------------------------------------------
void Camera::setCameraOrigin (void)
{
	if (!usePerspective)
	{
		//--------------------
		//Parallel Projection
		cameraDirection.range = -3000.0f;
		//cameraDirection.pitch = cameraDirection.pitch;
		cameraOrigin.BuildRotation(Stuff::EulerAngles(cameraDirection.pitch, cameraDirection.yaw, 0.0f));

		Stuff::Point3D translation(cameraDirection);

		translation.x -= cameraShift.x;
		translation.z += cameraShift.y;
		translation.y += cameraShiftZ;

		cameraOrigin.BuildTranslation(translation);
		
		lookVector.Normalize(translation);
	}
	else
	{
		//--------------------------------
		// Perspective projection
		bool isOKView = false;
		float localAltitude = cameraAltitude;
		float localAngle = projectionAngle;
		while (!isOKView)
		{
			cameraDirection.range = localAltitude;
			cameraDirection.pitch = -localAngle * DEGREES_TO_RADS;
			cameraOrigin.BuildRotation(Stuff::EulerAngles(cameraDirection.pitch, cameraDirection.yaw, 0.0f));
	
			Stuff::Point3D translation(cameraDirection);
			lookVector = translation;
			lookVector.Normalize(lookVector);
			lookVector.Negate(lookVector);
			
			translation.x += -cameraShift.x;
			translation.z += cameraShift.y;
			translation.y += cameraShiftZ;
			
			actualPosition.x = -translation.x;
			actualPosition.y = translation.z;
			actualPosition.z = translation.y - ELEVATION_BUFFER;

			// [HZB_CAM v1] one-shot diagnostic (env-gated, at most once per second)
			if (mc2::gate::get<"MC2_HZB_FORCE_HORIZON">())
			{
				static double s_hzbCamLastPrint = -999.0;
				double nowSec = (double)clock() / CLOCKS_PER_SEC;
				if (nowSec - s_hzbCamLastPrint >= 1.0)
				{
					s_hzbCamLastPrint = nowSec;
					fprintf(stderr,
						"[HZB_CAM v1] eyeX=%f eyeY=%f lookX=%f lookY=%f rot=%f angle=%f\n",
						actualPosition.x, actualPosition.y,
						lookVector.x, lookVector.y,
						cameraRotation, projectionAngle);
					fflush(stderr);
				}
			}

			// Clamp camera to terrain floor via position, not angle.
			// When clamped, recompute the rotation to look FROM the lifted
			// position TOWARD the focus — prevents the lifted camera from
			// aiming into the ground with the pre-lift pitch.
			if (land)
			{
				float terrainAtCam = land->getTerrainElevation(actualPosition);
				if (terrainAtCam < Terrain::waterElevation)
					terrainAtCam = Terrain::waterElevation;
				if (translation.y < terrainAtCam + AltitudeMinimum)
				{
					translation.y = terrainAtCam + AltitudeMinimum;
					actualPosition.z = translation.y - ELEVATION_BUFFER;

					// Repoint at focus from clamped position.
					float dx = position.x - actualPosition.x;
					float dy = position.y - actualPosition.y;
					float dz = cameraShiftZ - translation.y;
					float horiz = sqrtf(dx*dx + dy*dy);
					float newPitch = (horiz > 0.1f) ? atan2f(dz, horiz) : (dz < 0.0f ? -1.5707f : 0.0f);
					cameraOrigin.BuildRotation(Stuff::EulerAngles(newPitch, cameraDirection.yaw, 0.0f));
				}
			}

			isOKView = true;
			cameraOrigin.BuildTranslation(translation);
			physicalPos = translation;
		}
	}

	//------------------------------------------------------------
	// DO Exactly what MLR does to create its Internal Matrices.
	Stuff::LinearMatrix4D cameraToWorldMatrix(cameraOrigin);
																  
	worldToCameraMatrix.Invert(cameraToWorldMatrix);

	worldToClip.Multiply(worldToCameraMatrix, cameraToClip);
	
	if (usePerspective)
		clipToWorld.Invert(worldToClip);
}

//---------------------------------------------------------------------------
Stuff::Matrix4D Camera::worldToClipGL() const
{
    // F2 unified-projection: GPU-path projection product.
    //   = kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL
    // cameraToClipGL was precomputed at camera-update time (see
    // cameraToClipGL.Multiply invocations after every cameraToClip write
    // in calculateProjectionConstants). Replaces the prior per-call
    // kPixelHomogToGLNDC Multiply.
    Stuff::Matrix4D viewClipGL;
    viewClipGL.Multiply(worldToCameraMatrix, cameraToClipGL);
    Stuff::Matrix4D out;
    out.Multiply(kAxisSwapMC2toGL, viewClipGL);
    return out;
}

//---------------------------------------------------------------------------
Stuff::Matrix4D Camera::worldToViewGL() const
{
    // F1-3A ViewUniforms: view matrix = kAxisSwapMC2toGL * worldToCameraMatrix.
    // Mirrors worldToClipGL() but stops before cameraToClipGL multiply.
    // Uses Matrix4D::Multiply(const AffineMatrix4D&, const Matrix4D&) overload,
    // valid because LinearMatrix4D inherits AffineMatrix4D.
    Stuff::Matrix4D out;
    // Reuse worldToClipGL() intermediate: out = kAxisSwapMC2toGL * worldToCameraMatrix
    // There is no Multiply(Matrix4D, LinearMatrix4D) directly, but LinearMatrix4D
    // IS-A AffineMatrix4D, so we produce a temporary Matrix4D first.
    Stuff::Matrix4D viewM(worldToCameraMatrix); // AffineMatrix4D -> Matrix4D promotion
    out.Multiply(kAxisSwapMC2toGL, viewM);
    return out;
}

//---------------------------------------------------------------------------
Stuff::Vector3D Camera::cameraOriginGL() const
{
    // F1-3A ViewUniforms: camera world position transformed to GL coord space.
    // kAxisSwapMC2toGL: x' = -x, y' = z, z' = y.
    // physicalPos holds the camera world position in MC2 space (set in updateCameraInfo
    // as physicalPos = translation).
    Stuff::Vector3D renderPos;
    mc2ToRender(renderPos, physicalPos);
    return renderPos;
}

//---------------------------------------------------------------------------
ModernClipResult Camera::projectModernClipGL(const Stuff::Vector3D& world) const
{
    // F4 projectZ-bypass helper. Row-vector convention:
    //   clip_row = world_row * M
    // world.w = 1.
    // M = worldToClipGL() = kAxisSwapMC2toGL * worldToCameraMatrix * cameraToClipGL.
    Stuff::Matrix4D M = worldToClipGL();
    ModernClipResult r;
    r.clip.x = world.x * M(0,0) + world.y * M(1,0) + world.z * M(2,0) + M(3,0);
    r.clip.y = world.x * M(0,1) + world.y * M(1,1) + world.z * M(2,1) + M(3,1);
    r.clip.z = world.x * M(0,2) + world.y * M(1,2) + world.z * M(2,2) + M(3,2);
    r.clip.w = world.x * M(0,3) + world.y * M(1,3) + world.z * M(2,3) + M(3,3);
    r.admit  = clipSpaceFrustumAdmitGL(r.clip);
    return r;
}

//---------------------------------------------------------------------------
void Camera::calculateProjectionConstants (void)
{
	cameraDirection.yaw = cameraRotation * DEGREES_TO_RADS;
	cameraDirection.pitch = projectionAngle * DEGREES_TO_RADS;
	
	screenCenter.x = screenResolution.x / 2.0;
	screenCenter.y = screenResolution.y / 2.0;
	screenCenter.z = screenResolution.z = 0.0f;
	
	if (usePerspective)
	{
		float oneOverResMag = screenResolution.GetLength();
		if (oneOverResMag > Stuff::SMALL)
			oneOverResMag = 1.0f / oneOverResMag;

		verticalSphereClipConstant = tan(screenResolution.y * oneOverResMag * camera_fov * 0.7071f * DEGREES_TO_RADS);
		horizontalSphereClipConstant = tan(screenResolution.x * oneOverResMag * camera_fov * 0.7071f * DEGREES_TO_RADS);
		
		cameraFrame.reset_to_world_frame();
		
		Stuff::Vector3D rotationVector(-projectionAngle,0.0f,cameraRotation);
		cameraFrame.rotate(rotationVector);
	}

	setOrthogonal();		//Setup the camera Clip matrix.

	setCameraOrigin();

	// Invalidate pick screen-rect cache when camera inputs change visually.
	// Coarse grid: position snapped to 0.25 WU, rotation to 0.05 deg, fov to 0.1 deg.
	// Stable under per-frame altitude/zoom lerp micro-drift that would otherwise
	// cause the revision to increment every frame even on a "stationary" camera.
	{
		auto snapI = [](float v, float step) -> uint32_t {
			return (uint32_t)(int32_t)(v / step);
		};
		uint32_t h = snapI(position.x,      0.25f) * 2654435761u
		           ^ snapI(position.y,      0.25f) * 2246822519u
		           ^ snapI(cameraShiftZ,    0.25f) * 3266489917u
		           ^ snapI(cameraRotation,  0.05f) * 374761393u
		           ^ snapI(projectionAngle, 0.05f) * 668265263u
		           ^ snapI(camera_fov,      0.1f)  * 2166136261u
		           ^ (uint32_t)(int32_t)screenResolution.x
		           ^ (uint32_t)(int32_t)screenResolution.y * 40503u;
		if (h != lastCameraInputHash_) {
			lastCameraInputHash_ = h;
			++viewProjectionRevision_;
			if (viewProjectionRevision_ == 0) viewProjectionRevision_ = 1;
		}
	}
}

//---------------------------------------------------------------------------
void Camera::calculateTopViewConstants (void)
{
	//-----------------------------------------------------------
	// Make camera look down on terrain.
	cameraDirection.yaw = cameraRotation * DEGREES_TO_RADS;
	cameraDirection.pitch = -90.0f * DEGREES_TO_RADS;
	
	screenCenter.x = screenResolution.x / 2.0;
	screenCenter.y = screenResolution.y / 2.0;
	
	setOrthogonal();		//Setup the camera Clip matrix.
	
	setCameraOrigin();
}

//---------------------------------------------------------------------------
void Camera::deactivate (void)
{
	//--------------------------------------------------------
	// Anything a camera needs to do on shutdown goes in here.
	active = FALSE;
}
