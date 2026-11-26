// Archives panel: browse a .mc2pak/.zip/.fst, filter, extract, and pack a folder. Entry preview is not hooked up
// (a texture/FIT hand-off to the inspector needs more than the one-call hookup the brief allows).
#pragma once
#include "ArchiveIo.h"
#include <memory>
#include <string>
#include <vector>

class ArchivePanel {
public:
    void open(const std::string& path);
    void draw();
    void drawDetails();   // selected-entry details + 4 KB preview (the browser pane)
private:
    int current_ = -1, previewRow_ = -1;
    std::vector<uint8_t> previewBytes_;
    size_t previewTotal_ = 0;
    bool previewIsText_ = false;
    void refilter();
    void extract(bool selectedOnly);
    std::unique_ptr<ArchiveSource> src_;
    std::vector<size_t> shown_;
    std::vector<char> selected_;
    char path_[512] = "", filter_[128] = "*", outDir_[512] = "extracted", packDir_[512] = "", packOut_[512] = "out.mc2pak";
    std::string status_;
};
