// Copyright (C) Microsoft Corporation. All rights reserved.
#include "gamelog.h"
namespace { GameLog channels[MAX_GAMELOGS]; }
GameLogPtr GameLog::files[MAX_GAMELOGS]{};
void GameLog::setup() {
    for (int i = 0; i < MAX_GAMELOGS; ++i) { files[i] = &channels[i]; channels[i].handle = i; }
}
void GameLog::cleanup() { for (auto& channel : channels) channel.close(); }
GameLogPtr GameLog::getNewFile() {
    setup();
    for (auto& channel : channels) if (!channel.inUse) return &channel;
    return nullptr;
}
