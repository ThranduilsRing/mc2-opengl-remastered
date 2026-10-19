// GUI sound requests use the game-owned sound service; no second singleton pointer.
#pragma once
namespace mc2::gui {
void playSound(long sampleId);
}
