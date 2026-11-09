// Production radio state machine; only the device and queue endpoints are seams.
#include "doctest.h"
#include <array>
#include <memory>
#include <vector>

namespace {
constexpr bool TRUE = true, FALSE = false;
constexpr int RADIO_CHANNEL = 0, BUZZER1 = 1;
constexpr float ALARM_TIME = 5;
bool useSound = true, GeneralAlarm = false;
float frameLength = 1.0f / 30;
enum { gosAudio_Stop, gosAudio_PlayOnce };
int channelMode = gosAudio_Stop;
int gosAudio_GetChannelPlayMode(int) { return channelMode; }
void gosAudio_DestroyResource(int* handle) { *handle = 0; }
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
struct SoundSystem { void update() {} };
struct GameSoundSystem : SoundSystem {
    std::unique_ptr<RadioData> currentMessage, queued;
    unsigned messagesInQueue = 0, currentFragment = 0;
    bool playingNoise = false, wholeMsgDone = true;
    int radioHandle = 0;
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

TEST_CASE("MissionAudio/radio cues play once with and without opening static and a slow load") {
    for (bool openingStatic : {false, true}) {
        for (int busyFrames : {0, 90}) {
            CAPTURE(openingStatic, busyFrames);
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
