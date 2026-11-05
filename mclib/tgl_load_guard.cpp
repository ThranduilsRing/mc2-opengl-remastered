//===========================================================================//
// mclib/tgl_load_guard.cpp — META-FIX design B PART 1.
// See tgl_load_guard.h for the contract.
//===========================================================================//

#include "mc2_czstring.h"
#include "tgl_load_guard.h"
#include "inifile.h"
#include "content_error.h"
#include "texture_assets.h"

#include "msl.h"        // TG_TypeMultiShape + TG_TypeShape + tglHeap
#include "render_submissions.h"     // mc2::textures::ready() (placeholder texture registration)
#include "mc2_audit.h"  // [ASSET_LOAD v1] marker emitter (INFRA-1)
#include "mc2fmt.h"
#include "mc2_verify.h" // MC2_VERIFY live data-contract guard (log/fatal/off)

#include <cstring>
#include <cmath>
#include "Gates/gate.h"
#include <string_view>

//---------------------------------------------------------------------------
namespace {

unsigned long long guardKey(mc2::czstring ctx, mc2::czstring path)
{
	unsigned long long h = 5381;
	const auto mix = [&h](std::string_view s) {
		for (const unsigned char c : s) h = h * 33 + c;
	};
	mix(ctx ? std::string_view(ctx) : std::string_view());
	mix(path ? std::string_view(path) : std::string_view());
	return h;
}

// Populate `shape` with the fail-loud placeholder: one magenta box
// (12x12x20 local units) + a REAL 4x4 magenta texture registered through
// mc2::textures::ready(). Runs the same Track-D construction API the Assimp
// importer uses (AllocateImportedShapes / SetImportedTextures /
// InitFromImportedMesh), so the result is a first-class TG type shape the
// GPU batchers can register (valid handle => layerForPacket resolves; a
// textureless placeholder would be culled — bdactor.cpp layerForPacket
// evidence).
//
// Known residual (documented in the design): per-INSTANCE texture rebind
// loops resolve texture NAMES from disk and reset unresolvable slots to
// 0xffffffff on the legacy CPU path — "mc2_load_placeholder" is never on
// disk, so on that path the box may degrade to untextured-but-visible
// geometry (the stock NULLTXM case, crash-free). The GPU batchers read the
// TYPE-level handle set here at finalize and keep the magenta. The one
// instance-side rebind that would clobber the MECH placeholder
// (resetPaintScheme) is guarded by AppearanceType::baseShapeLoadFailed.
void populatePlaceholder(TG_TypeMultiShape* shape)
{
	// A failed load can leave a partially-populated multishape (e.g. the
	// LoadBinaryCopy unknown-nodeType abort clears shapes but keeps the
	// texture array). destroy() resets every member cleanly, then the
	// Track-D construction asserts hold legitimately.
	shape->destroy();

	shape->AllocateImportedShapes(1);

	static const char* kTexNames[1] = { "mc2_load_placeholder" };
	static const bool  kTexAlphas[1] = { false };
	shape->SetImportedTextures(1, kTexNames, kTexAlphas);

	TG_TypeNodePtr slot = shape->GetTypeNode(0);
	if (!slot || slot->GetNodeType() != SHAPE_NODE)
		return; // allocation failed — leave empty (fail-soft)
	TG_TypeShape* ts = static_cast<TG_TypeShape*>(slot);

	const int kNumVerts = 8;
	const int kNumTris  = 12;
	std::vector<TG_TypeVertex> verts(kNumVerts);
	std::vector<TG_TypeTriangle> tris(kNumTris);

	// Box corners: x/y in [-6,6], z in [0,20] (visible whichever axis is
	// "up" in the local shape space).
	const float kHx = 6.0f, kHy = 6.0f, kZ0 = 0.0f, kZ1 = 20.0f;
	const float px[8] = { -kHx,  kHx,  kHx, -kHx, -kHx,  kHx,  kHx, -kHx };
	const float py[8] = { -kHy, -kHy,  kHy,  kHy, -kHy, -kHy,  kHy,  kHy };
	const float pz[8] = {  kZ0,  kZ0,  kZ0,  kZ0,  kZ1,  kZ1,  kZ1,  kZ1 };
	for (int v = 0; v < 8; ++v)
	{
		verts[v].position.x = px[v];
		verts[v].position.y = py[v];
		verts[v].position.z = pz[v];
		// Radial normals — good enough for an error placeholder.
		float nx = px[v], ny = py[v], nz = pz[v] - (kZ1 * 0.5f);
		float len = sqrtf(nx * nx + ny * ny + nz * nz);
		if (len < 1e-4f) len = 1.0f;
		verts[v].normal.x = nx / len;
		verts[v].normal.y = ny / len;
		verts[v].normal.z = nz / len;
		verts[v].aRGBLight = 0xffffffff;
	}

	static const int kIdx[12][3] = {
		{0,1,2},{0,2,3},   // bottom
		{4,6,5},{4,7,6},   // top
		{0,4,5},{0,5,1},   // -y side
		{1,5,6},{1,6,2},   // +x side
		{2,6,7},{2,7,3},   // +y side
		{3,7,4},{3,4,0},   // -x side
	};
	for (int t = 0; t < kNumTris; ++t)
	{
		TG_TypeTriangle& tr = tris[t];
		tr.Vertices[0] = kIdx[t][0];
		tr.Vertices[1] = kIdx[t][1];
		tr.Vertices[2] = kIdx[t][2];
		tr.localTextureHandle = 0;   // texture slot 0 (the magenta)
		tr.renderStateFlags = 0;
		// Solid-color texture — UVs just need to land inside it.
		tr.uvdata.u0 = 0.0f; tr.uvdata.v0 = 0.0f;
		tr.uvdata.u1 = 1.0f; tr.uvdata.v1 = 0.0f;
		tr.uvdata.u2 = 1.0f; tr.uvdata.v2 = 1.0f;
		glm::vec3 fn;
		fn.x = verts[tr.Vertices[0]].normal.x + verts[tr.Vertices[1]].normal.x + verts[tr.Vertices[2]].normal.x;
		fn.y = verts[tr.Vertices[0]].normal.y + verts[tr.Vertices[1]].normal.y + verts[tr.Vertices[2]].normal.y;
		fn.z = verts[tr.Vertices[0]].normal.z + verts[tr.Vertices[1]].normal.z + verts[tr.Vertices[2]].normal.z;
		tr.faceNormal = fn;
	}

	glm::vec3 zero;
	zero.x = zero.y = zero.z = 0.0f;
	ts->InitFromImportedMesh("mc2_load_placeholder", "None", zero, std::move(verts), std::move(tris));

	// Bounding volume (CreateFrom/cull path reads these).
	shape->minBox.x = -kHx; shape->minBox.y = -kHy; shape->minBox.z = kZ0;
	shape->maxBox.x =  kHx; shape->maxBox.y =  kHy; shape->maxBox.z = kZ1;
	shape->extentRadius = sqrtf(kHx * kHx + kHy * kHy + kZ1 * kZ1);

	// REAL registered texture (contract: valid handle, not numTextures=0).
	// 4x4 solid magenta, ARGB. Null-guarded: shapes loaded before the
	// texture manager exists keep the 0xffffffff sentinel (still geometry).
	if (mc2::textures::ready())
	{
		static DWORD s_magentaHandle = 0xffffffff;
		if (s_magentaHandle == 0xffffffff)
		{
			DWORD texels[16];
			for (unsigned int & texel : texels)
				texel = 0xffff00ff;   // A=ff R=ff G=00 B=ff
			s_magentaHandle = mc2::textures::fromPixels(
				texels, gos_Texture_Solid,
				gosHint_DisableMipmap | gosHint_DontShrink, 4);
		}
		if (s_magentaHandle != 0xffffffff)
		{
			shape->SetTextureHandle(0, s_magentaHandle);
			shape->SetTextureAlpha(0, false);
		}
	}
}


} // namespace

void tglReadImportSource(FitIniFile& ini, std::span<char, 256> source)
{
	if (ini.seekBlock("Import") == NO_ERR &&
	    ini.readIdString("Source", source.data(), 255) == NO_ERR && source[0]) {
		if (char* dot = std::strrchr(source.data(), '.')) *dot = '\0';
	}
}

//---------------------------------------------------------------------------
bool tglVerifyShapeLoaded(TG_TypeMultiShape* shape, long rc, mc2::czstring ctx,
                          const char* assetPath, bool* failFlag)
{
	const bool loaded = (shape != nullptr) && (rc == 0) && (shape->GetNumShapes() > 0);
	if (loaded)
		return true;
	MC2_VERIFY(false, "required shape load failed ctx=%s path=%s",
		ctx ? ctx : "?", (assetPath && assetPath[0]) ? assetPath : "?");
	mc2::content::error(mc2::content::Kind::Shape, assetPath ? assetPath : "", "required shape missing; visible placeholder");

	// (1) always-on marker (rate-limited per (ctx,path); asset_load_fail
	//     red bucket counts this event).
	if (mc2AuditOnce("ASSET_LOAD", guardKey(ctx, assetPath)))
		mc2AuditLinef("[ASSET_LOAD v1] event=required_missing ctx={} rc={:d} path={}",
			mc2::fmt::safe(ctx ? ctx : "?"), mc2::fmt::as_i32(rc),
			mc2::fmt::safe((assetPath && assetPath[0]) ? assetPath : "?"));

	// (3) flag the owning AppearanceType (PART 2 backstop + mech
	//     resetPaintScheme placeholder guard key on this).
	if (failFlag)
		*failFlag = true;

	// Preserve visible failure geometry for appearance consumers.
	if (shape)
	{
		populatePlaceholder(shape);
		return shape->GetNumShapes() > 0;
	}
	return false;
}

//---------------------------------------------------------------------------
long tglLoadShapeChecked(TG_TypeMultiShape* shape, mc2::czstring asePath,
                         const char* ctx, bool* failFlag)
{
	long rc = shape ? shape->LoadTGMultiShapeFromASE(asePath) : -1;
	tglVerifyShapeLoaded(shape, rc, ctx, asePath, failFlag);
	return rc;
}
