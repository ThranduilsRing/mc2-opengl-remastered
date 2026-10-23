// The real SDL_mixer backend: handle lifetime and audible stereo gains, without a game or window.
#include "doctest.h"
#include "gameos.hpp"
#include "alloc_probe.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
extern bool gos_CreateAudio();
extern void gos_DestroyAudio();
namespace {
struct AudioDevice {
    std::string previous = SDL_getenv("SDL_AUDIODRIVER") ? SDL_getenv("SDL_AUDIODRIVER") : "";
    AudioDevice() { SDL_setenv("SDL_AUDIODRIVER", "dummy", 1); }
    ~AudioDevice() { Mix_SetPostMix(nullptr, nullptr); gos_DestroyAudio(); SDL_setenv("SDL_AUDIODRIVER", previous.c_str(), 1); }
};
struct Clip {
    HGOSAUDIO handle = nullptr;
    std::array<short, 8192> pcm;
    Clip() {
        for (std::size_t frame = 0; frame < pcm.size() / 2; ++frame) {
            const auto sample = static_cast<short>(12000 * std::sin(6.283185307179586 * 440 * static_cast<double>(frame) / 22050));
            pcm[2 * frame] = pcm[2 * frame + 1] = sample;
        }
        gosAudio_Format format{};
        format.wFormatTag = 1; format.nChannels = 2; format.nSamplesPerSec = 22050;
        format.wBitsPerSample = 16; format.nBlockAlign = 4; format.nAvgBytesPerSec = 88200;
        gosAudio_CreateResource(&handle, gosAudio_UserMemory, nullptr, &format, pcm.data(), static_cast<int>(sizeof(pcm)));
    }
    ~Clip() { gosAudio_DestroyResource(&handle); }
};
struct Capture {
    std::mutex mutex;
    std::condition_variable ready;
    std::array<short, 8192> pcm{};
    std::size_t count = 0;
    static void mix(void* context, Uint8* stream, int length) {
        auto& self = *static_cast<Capture*>(context);
        std::lock_guard lock(self.mutex);
        const auto count = std::min(self.pcm.size() - self.count, static_cast<std::size_t>(length) / sizeof(short));
        std::memcpy(self.pcm.data() + self.count, stream, count * sizeof(short));
        self.count += count;
        if (self.count == self.pcm.size()) self.ready.notify_one();
    }
    bool record() {
        Mix_SetPostMix(mix, this);
        std::unique_lock lock(mutex);
        const bool complete = ready.wait_for(lock, std::chrono::seconds(3), [&] { return count == pcm.size(); });
        lock.unlock(); Mix_SetPostMix(nullptr, nullptr);
        return complete;
    }
    double mean(int side) const {
        double sum = 0;
        // Discard the first mixer buffer so the result cannot include a pre-slider buffer.
        for (std::size_t i = 4096 + side; i < pcm.size(); i += 2) sum += std::abs(static_cast<int>(pcm[i]));
        return sum / 2048;
    }
    void save(const char* name) const {
        const char* directory = std::getenv("MC2_AUDIO_TEST_WAV_DIR");
        if (!directory || !*directory) return;
        const auto path = std::string(directory) + "/" + name + ".wav";
        std::FILE* file = std::fopen(path.c_str(), "wb");
        REQUIRE(file != nullptr);
        auto word = [&](unsigned value, int bytes) { for (int i = 0; i < bytes; ++i) std::fputc((value >> (8 * i)) & 255, file); };
        std::fwrite("RIFF", 1, 4, file); word(36 + static_cast<unsigned>(sizeof(pcm)), 4);
        std::fwrite("WAVEfmt ", 1, 8, file); word(16, 4); word(1, 2); word(2, 2);
        word(22050, 4); word(88200, 4); word(4, 2); word(16, 2);
        std::fwrite("data", 1, 4, file); word(static_cast<unsigned>(sizeof(pcm)), 4);
        std::fwrite(pcm.data(), 1, sizeof(pcm), file); std::fclose(file);
    }
};
}
TEST_CASE("AudioBackend/resources are freed unbound and detached from every channel") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    { Clip warm; REQUIRE(warm.handle != nullptr); }
    const ScopedLeakCheck allocations;
    {
        Clip unbound;
        REQUIRE(unbound.handle != nullptr);
        gosAudio_DestroyResource(&unbound.handle);
        CHECK(unbound.handle == nullptr);
        gosAudio_DestroyResource(&unbound.handle);
    }
    CHECK(allocations.delta() == 0);
    {
        Clip shared;
        gosAudio_AssignResourceToChannel(0, shared.handle);
        gosAudio_AssignResourceToChannel(1, shared.handle);
        gosAudio_SetChannelPlayMode(0, gosAudio_Loop);
        gosAudio_SetChannelPlayMode(1, gosAudio_Loop);
        gosAudio_DestroyResource(&shared.handle);
        CHECK(shared.handle == nullptr);
        CHECK(gosAudio_GetChannelPlayMode(0) == gosAudio_Stop);
        CHECK(gosAudio_GetChannelPlayMode(1) == gosAudio_Stop);
        // Starting an emptied channel must not dereference a freed resource.
        gosAudio_SetChannelPlayMode(1, gosAudio_PlayOnce);
        CHECK(gosAudio_GetChannelPlayMode(1) == gosAudio_Stop);
    }
    CHECK(allocations.delta() == 0);
}
TEST_CASE("AudioBackend/mixed PCM preserves center volume and pans independently") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    Clip clip;
    REQUIRE(clip.handle != nullptr);
    gosAudio_AllocateChannelSliders(0, gosAudio_Volume | gosAudio_Panning);
    gosAudio_AssignResourceToChannel(0, clip.handle);
    const char* names[] = {"left", "center", "right", "right_quiet"};
    const float pans[] = {-1, 0, 1, 1};
    std::array<double, 4> left{}, right{};
    for (int i = 0; i < 4; ++i) {
        gosAudio_SetChannelPlayMode(0, gosAudio_Stop);
        // Game order: set sliders before playback, then reuse the stopped channel.
        gosAudio_SetChannelSlider(0, gosAudio_Panning, pans[i]);
        gosAudio_SetChannelSlider(0, gosAudio_Volume, i == 3 ? 0.25f : 1.0f);
        float cached = 0;
        gosAudio_GetChannelSlider(0, gosAudio_Panning, &cached);
        CHECK(cached == pans[i]);
        gosAudio_SetChannelPlayMode(0, gosAudio_Loop);
        gosAudio_SetChannelPlayMode(0, gosAudio_Stop);
        gosAudio_SetChannelPlayMode(0, gosAudio_Loop);
        Capture capture;
        REQUIRE(capture.record());
        capture.save(names[i]);
        left[i] = capture.mean(0); right[i] = capture.mean(1);
    }
    CHECK(left[0] > 1000); CHECK(right[0] == 0);
    CHECK(left[1] == doctest::Approx(right[1]).epsilon(0.03));
    CHECK(left[0] == doctest::Approx(left[1]).epsilon(0.03));
    CHECK(left[2] == 0); CHECK(right[2] == doctest::Approx(right[1]).epsilon(0.03));
    CHECK(left[3] == 0);
    CHECK(right[3] / right[2] > 0.4); CHECK(right[3] / right[2] < 0.6);
    std::printf("AudioBackend PCM: left=%.0f/%.0f center=%.0f/%.0f right=%.0f/%.0f quiet=%.0f/%.0f\n",
                left[0], right[0], left[1], right[1], left[2], right[2], left[3], right[3]);
}

TEST_CASE("AudioBackend/predecoded music owns PCM across handles and cache release") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    const auto path = std::filesystem::temp_directory_path() /
        ("mc2-predecoded-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct RemoveFile {
        std::filesystem::path path;
        ~RemoveFile() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    const auto name = path.string();
    CHECK_FALSE(gosAudio_PreloadMusic(name.c_str())); // Failed loads must be retryable.
    Clip original;
    SDL_RWops* file = SDL_RWFromFile(name.c_str(), "wb");
    REQUIRE(file != nullptr);
    SDL_RWwrite(file, "RIFF", 1, 4); SDL_WriteLE32(file, 36 + sizeof(original.pcm));
    SDL_RWwrite(file, "WAVEfmt ", 1, 8); SDL_WriteLE32(file, 16);
    SDL_WriteLE16(file, 1); SDL_WriteLE16(file, 2); SDL_WriteLE32(file, 22050);
    SDL_WriteLE32(file, 88200); SDL_WriteLE16(file, 4); SDL_WriteLE16(file, 16);
    SDL_RWwrite(file, "data", 1, 4); SDL_WriteLE32(file, sizeof(original.pcm));
    SDL_RWwrite(file, original.pcm.data(), sizeof(original.pcm), 1); SDL_RWclose(file);
    REQUIRE(gosAudio_PreloadMusic(name.c_str()));
    REQUIRE(gosAudio_PreloadMusic(name.c_str()));
    REQUIRE(std::filesystem::remove(path)); // Subsequent resource creation must perform no file I/O.
    HGOSAUDIO first = nullptr, second = nullptr;
    gosAudio_CreateResource(&first, gosAudio_StreamedFile, name.c_str());
    gosAudio_CreateResource(&second, gosAudio_StreamedFile, name.c_str());
    REQUIRE(first != nullptr); REQUIRE(second != nullptr); CHECK(first != second);
    gosAudio_AssignResourceToChannel(0, first);
    gosAudio_AssignResourceToChannel(1, second);
    gosAudio_SetChannelSlider(1, gosAudio_Volume, 1.0f);
    gosAudio_SetChannelSlider(1, gosAudio_Panning, 0.0f);
    gosAudio_SetChannelPlayMode(1, gosAudio_Loop);
    gosAudio_DestroyResource(&first);
    gosAudio_ClearMusicCache();
    CHECK(gosAudio_GetChannelPlayMode(1) == gosAudio_Loop);
    Capture capture;
    REQUIRE(capture.record());
    CHECK(capture.mean(0) > 1000);
    CHECK(capture.mean(0) == doctest::Approx(capture.mean(1)).epsilon(0.03));
    gosAudio_DestroyResource(&second);
    HGOSAUDIO missing = nullptr;
    gosAudio_CreateResource(&missing, gosAudio_StreamedFile, name.c_str());
    CHECK(missing == nullptr);
}
