#pragma once

#include <string_view>

// Register configured music for bounded streaming. No catalog audio is decoded.
bool gosAudio_PreloadMusic(std::string_view path);
void gosAudio_ClearMusicCache();
