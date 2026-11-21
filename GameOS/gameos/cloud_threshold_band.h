// Engine-free cloud coverage/softness -> noise threshold band (pure; unit-tested).
#pragma once

struct CloudThreshBand { float lo; float hi; };

// coverage 0 keeps the legacy 0.3/0.7 band; >0 = band of half-width softness centred on the fbm noise quantile
// that covers that ground fraction (measured: 0.1 -> 0.32, 0.2 -> 0.376, 0.3 -> 0.42, 0.5 -> 0.50).
inline CloudThreshBand cloudThreshBand(float coverage, float softness) {
    const float c = 0.27f + 0.5f * coverage;
    return { coverage > 0.0f ? c - softness : 0.3f, coverage > 0.0f ? c + softness : 0.7f };
}
