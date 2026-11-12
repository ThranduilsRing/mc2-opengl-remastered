"""Run the production mission-selection update against a defs-backed operation page.

The operation page's base handleMessage is a no-op; defs clicks never press its
legacy buttons. Compile the actual update, stubbing only its engine services.
Fixture: Carver V campaign.fit group 5 after completing mc2_06 (Base Defense).
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import configure_ninja as cn
import console_child


def test_post_base_defense_marker_choices(tmp_path):
    source = (ROOT / "code/missionselectionscreen.cpp").read_text()
    update = source.split("void MissionSelectionScreen::update()", 1)[1].split(
        "void MissionSelectionScreen::begin()", 1)[0]
    cpp = tmp_path / "campaign_marker.cpp"
    exe = tmp_path / ("campaign_marker.exe" if os.name == "nt" else "campaign_marker")
    dispatch = source.split("int MissionSelectionScreen::handleMessage(", 1)[1].split(
        "void MissionSelectionScreen::updateListBox()", 1)[0]
    cpp.write_text(STUBS + "void MissionSelectionScreen::update()" + update
                   + "int MissionSelectionScreen::handleMessage(" + dispatch + MAIN)
    if os.name == "nt":
        command = subprocess.list2cmdline([
            "cl", "/nologo", "/EHsc", "/std:c++latest", "/O2", str(cpp),
            "/Fe:" + str(exe), "/Fo:" + str(tmp_path / "campaign_marker.obj")])
        assert cn.under_vcvars(command) == 0
    else:
        compiler = shutil.which("c++")
        assert compiler, "a C++ compiler is required for the production-path regression"
        subprocess.run([compiler, "-std=c++23", "-O2", str(cpp), "-o", str(exe)], check=True,  # background-launch: allow standalone C++ regression compiler, never a game
                       **console_child.child_kwargs())
    run = subprocess.run([str(exe)], capture_output=True, text=True,  # background-launch: allow standalone generated input regression, never a game
                         **console_child.child_kwargs(redirected=True))
    assert run.returncode == 0, run.stdout + run.stderr


STUBS = r'''
#include <cstdio>
#include <format>
#include <optional>
#include <string>
constexpr int MAX_MISSIONS_IN_GROUP = 6;
constexpr int MSG_FIRST_MISSION = 100, RP_TEXT = 4;
constexpr int MB_MSG_NEXT = 1, MB_MSG_PREV = 2, MN_MSG_PLAY = 3,
              MN_MSG_STOP = 4, MN_MSG_PAUSE = 5, MB_MSG_MAINMENU = 6;
namespace mc2::fmt { int as_i32(int v) { return v; } }
struct Input {
    int x = -1, y = 10; bool clicked = false;
    int getMouseX() { return x; } int getMouseY() { return y; }
    bool isLeftClick() { return clicked; }
} input, *userInput = &input;
struct Button {
    int index = 0; bool enabled = true, showing = true, pressed = false;
    bool isShowing() { return showing; } bool isEnabled() { return enabled; }
    bool isPressed() { return pressed; } void press(bool p) { if (enabled) pressed = p; }
    bool pointInside(int x, int y) { return x >= index * 100 && x < index * 100 + 50 && y >= 0 && y < 50; }
    int getID() { return 100 + index; } unsigned long getColor() { return 0xffff0000; }
    void showGUIWindow(bool) {}
};
struct Text { void setText(const std::string&) {} };
struct LogisticsScreen {
    Button buttons[6]; Text textObjects[5]; int buttonCount = 4;
    bool defs = true;
    bool defsUiPageUsable() { return defs; }
    // The actual defs page dispatches handleMessage; this base handler is inert.
    virtual int handleMessage(unsigned long, unsigned long) { return 0; }
    Button* getButton(unsigned long) { return &buttons[0]; }
    void setDefsElementText(const char*, const char*) {}
    void update() {
        for (int i = 0; i < buttonCount; ++i)
            if (input.clicked && buttons[i].pointInside(input.x,input.y) && buttons[i].enabled) {
                if (defs) handleMessage(0, buttons[i].getID());
                else buttons[i].press(true);
            }
    }
};
struct Data {
    static Data* instance; std::string selected = "mc2_07";
    void setCurrentMission(const char* name) { selected = name; }
    int getCBills() { return 829060; }
    std::optional<int> getCurrentMissionTune() { return {}; }
    std::string getMissionFriendlyName(const char* s) { return s; }
    bool skipLogistics() { return false; }
} data;
Data* Data::instance = &data;
using LogisticsData = Data;
struct Sound { void playDigitalMusic(int) {} } sound, *soundSystem = &sound;
struct Movie { void stop() {} void update() {} void restart() {} void pause(int) {} };
struct List { void update() {} };
struct MissionSelectionScreen : LogisticsScreen {
    LogisticsScreen operationScreen;
    bool playedLogisticsTune = true, bStop = false;
    Movie* bMovie = nullptr; List missionDescriptionListBox;
    int pressedButton = 1, missionCount = 3;
    const char* missionNames[6] = {"mc2_06", "mc2_07", "mc2_08", "mc2_09"};
    enum { NEXT, PREVIOUS, MAINMENU }; int status = 0;
    int handleMessage(unsigned long, unsigned long) override;
    void setMission(int);
    void updateListBox() {}
    void update();
};
'''

MAIN = r'''
int main() {
    for (bool defs : {true, false}) {
        MissionSelectionScreen screen;
        screen.buttonCount = 0; // the parent page has navigation, not operation markers
        screen.operationScreen.defs = defs;
        for (int i = 0; i < 4; ++i) screen.operationScreen.buttons[i].index = i;
        screen.operationScreen.buttons[0].enabled = false; // completed mc2_06
        screen.operationScreen.buttons[3].showing = false; // hidden mc2_09
        screen.operationScreen.buttons[1].pressed = true;
        data.selected = "mc2_07";
        for (int choice : {2, 1, 2, 1, 0, 3}) {
            input.x = choice * 100 + 10; input.clicked = true;
            screen.update();
            int expected = choice == 0 || choice == 3 ? 1 : choice;
            if (data.selected != screen.missionNames[expected] || screen.pressedButton != expected) {
                std::fprintf(stderr, "defs=%d click=mc2_%02d expected=mc2_%02d selected=%s\n",
                             defs, choice + 6, expected + 6, data.selected.c_str());
                return 1;
            }
            if (!screen.operationScreen.buttons[expected].isPressed()) return 2;
            if (screen.operationScreen.buttons[3 - expected].isPressed()) return 3;
            input.clicked = false;
            screen.update();
        }
    }
    std::puts("PASS: mc2_08 -> mc2_07 -> mc2_08 -> mc2_07; completed/hidden rejected; defs + legacy");
}
'''
