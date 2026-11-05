// Copyright (C) Microsoft Corporation. All rights reserved.
// Legacy named channels, routed through mc2::log with no file pool or line buffers.
#pragma once
#include "mc2_czstring.h"
#include "dgamelog.h"
#include "utils/mc2log.h"
#include <string>
inline constexpr int MAX_GAMELOGS = 6;
class GameLog {
    mc2::log::Cat category = mc2::log::Cat::Sim;
    std::string label;
public:
    long handle = -1;
    bool inUse = false;
    static GameLogPtr files[MAX_GAMELOGS];
    void init() { close(); }
    void destroy() { close(); }
    void dump() {}
    void close() { inUse = false; label.clear(); }
    long open(mc2::czstring name) {
        label = name ? name : "game";
        category = label.find("net") != std::string::npos ? mc2::log::Cat::Net : mc2::log::Cat::Sim;
        inUse = true; return 0;
    }
    void write(mc2::czstring text) {
        if (inUse && mc2::log::enabled(category, mc2::log::Level::Info))
            mc2::log::logFmt(category, mc2::log::Level::Info, mc2::log::Stream::Err,
                            std::source_location::current(), "{}: {}", label, text ? text : "");
    }
    static void setup();
    static void cleanup();
    static GameLogPtr getNewFile();
};
