#pragma once
#include "render_submissions.h"
// Temporary lifecycle facade; submission and lighting have independent owners.
class MC_TextureManager {
public:
    void start();
    void destroy();
    void flush(bool justTextures=false);
    ~MC_TextureManager();
};
extern MC_TextureManager* mcTextureManager;
