// The production ControlGui::playMovie() deferral, on the real registered overlap worker.
// Real: ControlGui::playMovie (code/controlgui_chat.cpp), MC2Movie::init (code/mc2movie.cpp), tick_commit, sim_overlap.
// Seam: only the decoder/device end (video_open, which creates the Render-owner-only gos texture) and its resolver.
// Regression guarded: the worker (ABL playVideo, objective CPlayBIK) opened the movie -> gos_NewEmptyTexture requireOwner abort.
#include "doctest.h"
#include "../../code/controlgui.h"
#include "../../code/mc2video.h"
#include "../../code/tick_commit.h"
#include "../../mclib/sim_overlap.h"
#include "../../code/chatwindow.h"
#include "../../code/comndr.h"
#include "../../code/forcegroupbar.h"
#include "../../code/gamesound.h"
#include "../../code/missiongui.h"
#include "../../code/multplyr.h"
#include "../../code/objective.h"
#include "../../code/pausewindow.h"
#include "../../code/tacordr.h"
#include "../../code/team.h"
#include "../../GameOS/gameos/gos_smoke.h"
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

namespace {
struct OpenProbe {
    int opens = 0;
    int opensOnWorker = 0;        // video_open reached while on the sim worker
    int opensWithoutRender = 0;   // video_open reached by a thread that does not own Render (the real call would abort)
    void reset() { *this = OpenProbe{}; }
} probe;
}

// Seam: the decoder/device end. The real video_open calls gos_NewEmptyTexture, whose first line is
// requireOwner(Owner::Render); isOwner is the same predicate without the abort, so a violation is countable.
VideoSession* video_open(const VideoOpenParams&, VideoOpenResult*) {
    ++probe.opens;
    if (mc2::overlap::onSimWorker()) ++probe.opensOnWorker;
    if (!mc2::overlap::isOwner(mc2::overlap::Owner::Render)) ++probe.opensWithoutRender;
    return nullptr;   // rejected: playMovie then takes its documented rejected path
}
void video_close(VideoSession*) {}
bool video_update(VideoSession*) { return false; }
void video_render(VideoSession*) {}
void video_pause(VideoSession*, bool) {}
void video_stop(VideoSession*) {}
void video_restart(VideoSession*) {}
void video_setRect(VideoSession*, int, int, int, int) {}
unsigned int video_get_texture(VideoSession*) { return 0; }
bool video_get_layout(VideoSession*, float*, float*, float*, float*, int*, int*, int*, int*) { return false; }
bool g_ffmpegAvailable = true;
bool resolveVideoCandidate(mc2::czstring, bool, int candidateIndex, char* out, unsigned outSize) {
    if (candidateIndex != 0) return false;
    std::snprintf(out, outSize, "seam_movie.bik");
    return true;
}

// Inert stand-ins for everything else controlgui_chat.cpp / mc2movie.cpp link against (none is reached by playMovie).
GameSoundSystem* soundSystem = nullptr;
float scenarioTime = 0.0f;
long helpTextHeaderID = 0;
long helpTextID = 0;
ChatWindow* ChatWindow::s_instance = nullptr;
MissionInterfaceManager* MissionInterfaceManager::s_instance = nullptr;
CommanderPtr Commander::home = nullptr;
TeamPtr Team::home = nullptr;
long MultiPlayer::colors[MAX_COLORS] = {};
long ControlGui::hiResOffsetX = 0;
long ControlGui::hiResOffsetY = 0;
gosEnum_KeyStatus gos_GetKeyStatus(gosEnum_KeyIndex) { return KEY_FREE; }
std::string objectiveObservationQuote(std::string_view) { return {}; }
CObjectives::Observation* objectiveObservation() { return nullptr; }
void objectiveObservationSink(const char*, const char*, const char*, const char*) {}
void CObjectives::Observation::SinkResult(const char*, const char*, const char*, std::string_view, unsigned long long) {}
const SmokeMode::State& SmokeMode::state() { static SmokeMode::State st{}; return st; }
bool ControlGui::hudTabsGateOn() const { return false; }
void ControlGui::setActiveHudTab(int, const char*) {}
void ControlGui::setRange(int) {}
ControlButton* ControlGui::getButton(int) { return nullptr; }
void ControlButton::press(bool) {}
bool ForceGroupBar::isPlayingVideo() { return false; }
bool ForceGroupBar::setPilotVideo(const char*, MechWarrior*) { return false; }
void ForceGroupBar::removeAll() {}
void PauseWindow::begin(bool) {}
void PauseWindow::end() {}
bool MultiPlayer::isServerMissing() { return false; }
void MultiPlayer::sendChat(NETPLAYER, char, char*) {}
void MultiPlayer::sendHoldPosition() {}
MoverPtr Team::getMover(long) { return nullptr; }
long TacticalOrder::pack(MoverGroupPtr, MoverPtr) { return 0; }
int MissionInterfaceManager::getHotKey(int, gosEnum_KeyIndex&, bool&, bool&, bool&) { return 0; }
int MissionInterfaceManager::togglePauseWithoutMenu() { return 0; }
long SoundSystem::playDigitalSample(unsigned long, glm::vec3, bool) { return 0; }
unsigned long aFont::height() const { return 0; }
unsigned long aFont::height(const char*, int) const { return 0; }
unsigned long aFont::width(const char*) const { return 0; }
float aObject::width() const { return 0; }
float aObject::height() const { return 0; }
float aObject::x() const { return 0; }
void aObject::resize(long, long) {}
void aObject::setColor(unsigned int, bool) {}
void aText::setText(std::string_view) {}
std::string aEdit::entry() const { return {}; }
void aEdit::setEntry(std::string_view, unsigned char) {}
void aEdit::setFocus(bool) {}

TEST_CASE("ControlGui::playMovie from the sim worker defers the decoder open to the main-thread commit") {
    // ControlGui's constructor loads GUI assets; playMovie only needs the zero state a fresh object has for these members.
    alignas(ControlGui) static unsigned char storage[sizeof(ControlGui)];
    std::memset(storage, 0, sizeof storage);
    auto* gui = reinterpret_cast<ControlGui*>(storage);
    probe.reset();

    mc2::overlap::runOnWorker([&] {
        CHECK(mc2::overlap::onSimWorker());
        mc2::tick::Scope scope;
        mc2::tick::setCollecting(true);
        gui->playMovie("seam_movie");
    });
    mc2::overlap::waitWorker();

    // The request was queued, not run: nothing reached the decoder, least of all on the worker.
    CHECK(probe.opens == 0);
    CHECK(probe.opensOnWorker == 0);
    CHECK(probe.opensWithoutRender == 0);

    // The overlap join commits the MoverUi queue on the main thread, which owns Render.
    CHECK_FALSE(mc2::overlap::onSimWorker());
    mc2::tick::commitTickQueues();
    CHECK(probe.opens == 1);
    CHECK(probe.opensOnWorker == 0);
    CHECK(probe.opensWithoutRender == 0);

    // Frame (non-collecting) callers still run inline, once.
    probe.reset();
    gui->playMovie("seam_movie");
    CHECK(probe.opens == 1);
    CHECK(probe.opensWithoutRender == 0);
}
