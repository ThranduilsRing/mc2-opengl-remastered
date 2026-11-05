#pragma once
#include "mc2_czstring.h"
#include <span>

class FitIniFile;
class TG_TypeMultiShape;

// Optional import override: strip the extension before probing supported formats.
void tglReadImportSource(FitIniFile& ini, std::span<char, 256> source);

// Required shapes report a content error and retain an empty TG_TypeMultiShape
// on failure. Optional shapes keep their existing delete/null fallbacks.
// failFlag marks AppearanceType::baseShapeLoadFailed for downstream callers.
bool tglVerifyShapeLoaded(TG_TypeMultiShape* shape, long rc, mc2::czstring ctx,
                          const char* assetPath, bool* failFlag);

// Return the original ASE loader status after applying the required-shape check.
long tglLoadShapeChecked(TG_TypeMultiShape* shape, mc2::czstring asePath,
                         const char* ctx, bool* failFlag);
