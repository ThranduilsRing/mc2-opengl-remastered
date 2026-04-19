#pragma once

// Global runtime toggle for the GPU static-prop renderer.
// Default false — old CPU path is active until validated.
// Toggled at runtime via RAlt+0 (see gameosmain.cpp).
extern bool g_useGpuStaticProps;
