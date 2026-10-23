#pragma once

#include <string_view>

// Decode configured music before mission ticks. Existing resources survive cache release.
bool gosAudio_PreloadMusic(std::string_view path);
void gosAudio_ClearMusicCache();
