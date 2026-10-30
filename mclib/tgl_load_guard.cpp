//===========================================================================//
// mclib/tgl_load_guard.cpp — META-FIX design B PART 1.
// See tgl_load_guard.h for the contract.
//===========================================================================//

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

//---------------------------------------------------------------------------
namespace {

unsigned long long guardKey(const char* ctx, const char* path)
{
	unsigned long long h = 5381;
	for (const char* p = ctx;  p && *p; ++p) h = h * 33 + static_cast<unsigned char>(*p);
	for (const char* p = path; p && *p; ++p) h = h * 33 + static_cast<unsigned char>(*p);
	return h;
}

// Discard partial geometry from malformed content; callers retain an empty shape.
void populatePlaceholder(TG_TypeMultiShape* shape) { shape->destroy(); }

} // namespace

void tglReadImportSource(FitIniFile& ini, std::span<char, 256> source)
{
	if (ini.seekBlock("Import") == NO_ERR &&
	    ini.readIdString("Source", source.data(), 255) == NO_ERR && source[0]) {
		if (char* dot = std::strrchr(source.data(), '.')) *dot = '\0';
	}
}

//---------------------------------------------------------------------------
bool tglVerifyShapeLoaded(TG_TypeMultiShape* shape, long rc, const char* ctx,
                          const char* assetPath, bool* failFlag)
{
	const bool loaded = (shape != 0) && (rc == 0) && (shape->GetNumShapes() > 0);
	if (loaded)
		return true;
	mc2::content::error(mc2::content::Kind::Shape, assetPath ? assetPath : "", "required shape missing; empty shape");

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

	// Keep the allocated shape empty so appearance ownership remains valid.
	if (shape)
	{
		populatePlaceholder(shape);
		return shape->GetNumShapes() > 0;
	}
	return false;
}

//---------------------------------------------------------------------------
long tglLoadShapeChecked(TG_TypeMultiShape* shape, const char* asePath,
                         const char* ctx, bool* failFlag)
{
	long rc = shape ? shape->LoadTGMultiShapeFromASE(asePath) : -1;
	tglVerifyShapeLoaded(shape, rc, ctx, asePath, failFlag);
	return rc;
}
