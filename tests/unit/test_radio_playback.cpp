// Production radio state machine; only the device and queue endpoints are seams.
#include "doctest.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <string>

namespace {
constexpr bool TRUE = true, FALSE = false;
constexpr int RADIO_CHANNEL = 0, BUZZER1 = 1, BETTY_CHANNEL = 2, SUPPORT_CHANNEL = 3;
constexpr float ALARM_TIME = 5;
bool useSound = true, GeneralAlarm = false;
float frameLength = 1.0f / 30;
enum { gosAudio_Stop, gosAudio_PlayOnce };
int channelMode = gosAudio_Stop;
int gosAudio_GetChannelPlayMode(int) { return channelMode; }
void gosAudio_DestroyResource(void** handle) { *handle = nullptr; }
struct Pilot {
    void setMessagePlaying() {}
    void clearMessagePlaying() {}
};
struct RadioClip {
    int cue = 0;
    bool requested = false;
    bool playable() const { return cue != 0; }
};
struct RadioData {
    std::array<RadioClip, 16> data{}, noise{};
    Pilot* pilot = nullptr;
};
struct { std::atomic<bool> active{false}; } g_va;
struct SoundSystem {
    void* stream3Handle = nullptr;
    bool otherSpeech = false;
    bool isChannelPlaying(int channel) { return channel != RADIO_CHANNEL && otherSpeech; }
    void update() {}
    bool isPlayingVoiceOver();
    std::vector<std::string> pendingStreams, wavPlays;
    void startDigitalStream(const char* name) { wavPlays.emplace_back(name); stream3Handle = this; }
    void updateDigitalStreams();
};
#include "voiceover_playback.inc"
struct GameSoundSystem : SoundSystem {
    std::unique_ptr<RadioData> currentMessage, queued;
    unsigned messagesInQueue = 0, currentFragment = 0;
    bool playingNoise = false, wholeMsgDone = true;
    void* radioHandle = nullptr;
    float generalAlarmTimer = 0;
    std::vector<int> plays;
    bool playClip(const RadioClip& clip) {
        plays.push_back(clip.cue);
        channelMode = gosAudio_PlayOnce;
        return true;
    }
    void playDigitalSample(int) {}
    void moveFromQueueToPlaying() {
        currentMessage = std::move(queued);
        messagesInQueue = 0;
    }
    void removeCurrentMessage() { currentMessage.reset(); wholeMsgDone = true; }
    void playNoiseClip();
    void playNextDataClip();
    void update();
};
#define VIEWER
#include "radio_playback.inc"
#undef VIEWER
}

TEST_CASE("MissionAudio/briefing and tutorial speech hold pilot radio in its queue") {
    for (bool video : {false, true}) {
        channelMode = gosAudio_Stop;
        GameSoundSystem sound;
        sound.queued = std::make_unique<RadioData>();
        sound.queued->data[0].cue = 7;
        sound.messagesInQueue = 1;
        g_va.active = video;
        sound.stream3Handle = video ? nullptr : &sound;
        sound.update();
        CHECK(sound.plays.empty());
        CHECK(sound.messagesInQueue == 1);
        g_va.active = false;
        sound.stream3Handle = nullptr;
        sound.otherSpeech = true;
        sound.update();
        CHECK(sound.plays.empty());
        sound.otherSpeech = false;
        sound.update();
        CHECK(sound.plays == std::vector<int>{7});
    }
}

TEST_CASE("MissionAudio/objective WAVs wait for Renard and busy speech in FIFO order") {
    SoundSystem sound;
    sound.pendingStreams = {"tutorial.wav", "objective.wav"};
    g_va.active = true;
    sound.updateDigitalStreams();
    CHECK(sound.wavPlays.empty());
    g_va.active = false;
    sound.otherSpeech = true;
    sound.updateDigitalStreams();
    CHECK(sound.wavPlays.empty());
    sound.otherSpeech = false;
    sound.updateDigitalStreams();
    sound.updateDigitalStreams();
    CHECK(sound.wavPlays == std::vector<std::string>{"tutorial.wav"});
    sound.stream3Handle = nullptr;
    sound.updateDigitalStreams();
    CHECK(sound.wavPlays == std::vector<std::string>{"tutorial.wav", "objective.wav"});
    CHECK(sound.pendingStreams.empty());
}

TEST_CASE("MissionAudio/radio cues play once with and without opening static and a slow load") {
    for (bool openingStatic : {false, true}) {
        for (int busyFrames : {0, 90}) {
            CAPTURE(openingStatic);
            CAPTURE(busyFrames);
            channelMode = gosAudio_Stop;
            GameSoundSystem sound;
            sound.queued = std::make_unique<RadioData>();
            // A callsign followed by the message, as assembled by Radio::playMessage.
            sound.queued->data[0].cue = 1;
            sound.queued->data[1].cue = 2;
            sound.queued->noise[0] = {3, openingStatic};
            sound.messagesInQueue = 1;
            sound.update();
            // A stalled game update cannot restart the current device clip.
            for (int i = 0; i < busyFrames; ++i) sound.update();
            for (int i = 0; i < 8; ++i) {
                channelMode = gosAudio_Stop;
                sound.update();
            }
            CHECK(sound.plays == (openingStatic ? std::vector<int>{3, 1, 2} : std::vector<int>{1, 2}));
            CHECK(sound.wholeMsgDone);
            CHECK(sound.currentMessage == nullptr);
        }
    }
}

TEST_CASE("MissionAudio/an empty opening speech fragment completes without restarting") {
    channelMode = gosAudio_Stop;
    GameSoundSystem sound;
    sound.queued = std::make_unique<RadioData>();
    sound.messagesInQueue = 1;
    sound.update();
    CHECK(sound.wholeMsgDone);
    CHECK(sound.plays.empty());
    sound.update();
    CHECK(sound.currentMessage == nullptr);
    CHECK(sound.plays.empty());
}

TEST_CASE("MissionAudio/campaign briefing and mc2_01 voices start once without overlap") {
    SoundSystem sound;
    std::array<int, 4> starts{}; // campaign intro, V1A, V1C, tut_1cii
    int next = 0, remaining = 0;
    for (int tick = 0; tick < 450; ++tick) {
        if (remaining && --remaining == 0) {
            g_va.active = false;
            sound.stream3Handle = nullptr;
        }
        // mc2_01 stages 12 and 14 wait for NOT isPlayingVoiceOver.
        // Keep each decoder busy beyond the script's 2 s / 1.5 s delays.
        if (!sound.isPlayingVoiceOver() && next < 4) {
            CHECK(remaining == 0);
            ++starts[next];
            if (next < 3) g_va.active = true;
            else sound.stream3Handle = &sound;
            remaining = 90;
            ++next;
        }
    }
    CHECK(starts == std::array<int, 4>{1, 1, 1, 1});
    CHECK_FALSE(sound.isPlayingVoiceOver());
}
