// The real SDL_mixer backend: handle lifetime and audible stereo gains, without a game or window.
#include "doctest.h"
#include "gameos.hpp"
#include "gameos_music_cache.h"
#include "../../GameOS/gameos/music_stream.h"
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
TEST_CASE("AudioBackend/file WAV preserves user-memory PCM") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    Clip original;
    const auto path = std::filesystem::temp_directory_path() / "mc2-file-pcm.wav";
    struct Remove { std::filesystem::path p; ~Remove() { std::filesystem::remove(p); } } cleanup{path};
    const auto name = path.string();
    auto* file = SDL_RWFromFile(name.c_str(), "wb");
    REQUIRE(file != nullptr);
    SDL_RWwrite(file, "RIFF", 1, 4); SDL_WriteLE32(file, 36 + sizeof(original.pcm));
    SDL_RWwrite(file, "WAVEfmt ", 1, 8); SDL_WriteLE32(file, 16);
    SDL_WriteLE16(file, 1); SDL_WriteLE16(file, 2); SDL_WriteLE32(file, 22050);
    SDL_WriteLE32(file, 88200); SDL_WriteLE16(file, 4); SDL_WriteLE16(file, 16);
    SDL_RWwrite(file, "data", 1, 4); SDL_WriteLE32(file, sizeof(original.pcm));
    SDL_RWwrite(file, original.pcm.data(), sizeof(original.pcm), 1); SDL_RWclose(file);
    HGOSAUDIO loaded = nullptr;
    gosAudio_CreateResource(&loaded, gosAudio_StreamedFile, name.c_str());
    REQUIRE(loaded != nullptr);
    gosAudio_AssignResourceToChannel(0, original.handle);
    gosAudio_AssignResourceToChannel(1, loaded);
    gosAudio_SetChannelPlayMode(0, gosAudio_Loop);
    gosAudio_SetChannelPlayMode(1, gosAudio_Loop);
    auto* expected = Mix_GetChunk(0);
    auto* actual = Mix_GetChunk(1);
    REQUIRE(expected != nullptr); REQUIRE(actual != nullptr);
    REQUIRE(actual->alen == expected->alen);
    CHECK(std::memcmp(actual->abuf, expected->abuf, actual->alen) == 0);
    gosAudio_DestroyResource(&loaded);
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

TEST_CASE("AudioBackend/two music slots stream independently across catalog release") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    const auto path = std::filesystem::temp_directory_path() /
        ("mc2-predecoded-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct RemoveFile {
        std::filesystem::path path;
        ~RemoveFile() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    const auto name = path.string();
    REQUIRE(gosAudio_PreloadMusic(name.c_str())); // Registration does no file IO.
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
    HGOSAUDIO first = nullptr, second = nullptr;
    gosAudio_CreateResource(&first, gosAudio_StreamedFile, name.c_str());
    gosAudio_CreateResource(&second, gosAudio_StreamedFile, name.c_str());
    REQUIRE(first != nullptr); REQUIRE(second != nullptr); CHECK(first != second);
    gosAudio_AssignResourceToChannel(0, first);
    gosAudio_AssignResourceToChannel(1, second);
    gosAudio_SetChannelSlider(1, gosAudio_Volume, 1.0f);
    gosAudio_SetChannelSlider(1, gosAudio_Panning, 0.0f);
    gosAudio_SetChannelPlayMode(1, gosAudio_Loop);
    gosAudio_SetChannelSlider(0, gosAudio_Volume, 1.0f);
    gosAudio_SetChannelSlider(0, gosAudio_Panning, -1.0f);
    gosAudio_SetChannelPlayMode(0, gosAudio_Loop);
    gosAudio_SetChannelSlider(1, gosAudio_Panning, 1.0f);
    Capture both;
    REQUIRE(both.record());
    CHECK(both.mean(0) > 1000);
    CHECK(both.mean(1) > 1000);
    gosAudio_DestroyResource(&first);
    gosAudio_ClearMusicCache();
    CHECK(gosAudio_GetChannelPlayMode(1) == gosAudio_Loop);
    Capture capture;
    REQUIRE(capture.record());
    CHECK(capture.mean(0) < 1);
    CHECK(capture.mean(1) > 1000);
    // GOS resources may be shared by channels too. Starting the same handle
    // again must not destroy a decoder still used by the other callback.
    gosAudio_AssignResourceToChannel(0, second);
    gosAudio_SetChannelPlayMode(0, gosAudio_Loop);
    Capture shared;
    REQUIRE(shared.record());
    CHECK(shared.mean(0) > 1000);
    CHECK(shared.mean(1) > 1000);
    gosAudio_DestroyResource(&second);
    CHECK(gosAudio_GetChannelPlayMode(0) == gosAudio_Stop);
    CHECK(gosAudio_GetChannelPlayMode(1) == gosAudio_Stop);
    REQUIRE(std::filesystem::remove(path));
    HGOSAUDIO missing = nullptr;
    gosAudio_CreateResource(&missing, gosAudio_StreamedFile, name.c_str());
    CHECK(missing == nullptr);
}

TEST_CASE("AudioBackend/stream prebuffers bounded chunks and loops without a gap") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    const auto path = std::filesystem::temp_directory_path() /
        ("mc2-stream-loop-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove(path); } } cleanup{path};
    // Three stereo frames force multiple EOF boundaries inside one mixer buffer.
    const std::array<short, 6> samples{1000,2000,3000,4000,5000,6000};
    SDL_RWops* file = SDL_RWFromFile(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    SDL_RWwrite(file, "RIFF", 1, 4); SDL_WriteLE32(file, 36 + sizeof(samples));
    SDL_RWwrite(file, "WAVEfmt ", 1, 8); SDL_WriteLE32(file, 16);
    SDL_WriteLE16(file, 1); SDL_WriteLE16(file, 2); SDL_WriteLE32(file, 22050);
    SDL_WriteLE32(file, 88200); SDL_WriteLE16(file, 4); SDL_WriteLE16(file, 16);
    SDL_RWwrite(file, "data", 1, 4); SDL_WriteLE32(file, sizeof(samples));
    SDL_RWwrite(file, samples.data(), sizeof(samples), 1); SDL_RWclose(file);
    auto stream = MusicStream::open(path.string().c_str(), 22050, 2);
    REQUIRE(stream);
    auto other = MusicStream::open(path.string().c_str(), 22050, 2);
    REQUIRE(other);
    std::array<short, 2> frame{};
    REQUIRE(stream->read(std::as_writable_bytes(std::span(frame))) == sizeof(frame));
    CHECK(frame[0] == 1000);
    std::array<short, 6> independent{};
    REQUIRE(other->read(std::as_writable_bytes(std::span(independent))) == sizeof(independent));
    CHECK(independent == samples);
    REQUIRE(stream->read(std::as_writable_bytes(std::span(frame))) == sizeof(frame));
    CHECK(frame[0] == 3000);
    other.reset();
    stream = MusicStream::open(path.string().c_str(), 22050, 2);
    REQUIRE(stream);
    stream->setLoop(true);
    // Exactly the prebuffered cycles: no dependency on scheduling the worker.
    std::array<short, 6 * MusicStream::PrebufferChunks> out{};
    CHECK(stream->read(std::as_writable_bytes(std::span(out))) == sizeof(out));
    for (size_t i = 0; i < out.size(); ++i) CHECK(out[i] == samples[i % samples.size()]);
    CHECK_FALSE(stream->ended());
    stream.reset();
    stream = MusicStream::open(path.string().c_str(), 22050, 2);
    REQUIRE(stream);
    CHECK(stream->read(std::as_writable_bytes(std::span(out))) == sizeof(samples));
    CHECK(stream->ended());
    for (size_t i = samples.size(); i < out.size(); ++i) CHECK(out[i] == 0);
}

TEST_CASE("AudioBackend/catalog beyond old PCM cache never decodes at registration") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    // None of these files exists. A full-decode fallback would fail immediately.
    for (int i = 0; i < 4096; ++i)
        REQUIRE(gosAudio_PreloadMusic("missing-music-" + std::to_string(i) + ".wav"));
    HGOSAUDIO missing = nullptr;
    gosAudio_CreateResource(&missing, gosAudio_StreamedFile, "missing-music-4095.wav");
    CHECK(missing == nullptr);
    gosAudio_ClearMusicCache();
}

TEST_CASE("AudioBackend/music prebuffer does not decode the declared full track") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    const auto path = std::filesystem::temp_directory_path() /
        ("mc2-stream-bounded-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove(path); } } cleanup{path};
    SDL_RWops* file = SDL_RWFromFile(path.string().c_str(), "wb");
    REQUIRE(file);
    constexpr Uint32 declaredBytes = 1024u * 1024u * 1024u;
    SDL_RWwrite(file, "RIFF", 1, 4); SDL_WriteLE32(file, 36 + declaredBytes);
    SDL_RWwrite(file, "WAVEfmt ", 1, 8); SDL_WriteLE32(file, 16);
    SDL_WriteLE16(file, 1); SDL_WriteLE16(file, 2); SDL_WriteLE32(file, 22050);
    SDL_WriteLE32(file, 88200); SDL_WriteLE16(file, 4); SDL_WriteLE16(file, 16);
    SDL_RWwrite(file, "data", 1, 4); SDL_WriteLE32(file, declaredBytes);
    // A sparse one-GiB catalog track: bounded startup only touches the first queue.
    std::array<short, 32768> pcm;
    pcm.fill(2345);
    SDL_RWwrite(file, pcm.data(), sizeof(pcm), 1);
    REQUIRE(SDL_RWseek(file, 44 + declaredBytes - 1, RW_SEEK_SET) > 0);
    SDL_RWwrite(file, "\0", 1, 1); SDL_RWclose(file);
    const ScopedLeakCheck allocations;
    auto stream = MusicStream::open(path.string().c_str(), 22050, 2);
    REQUIRE(stream);
    // The complete PCM would require 1 GiB; all queue and decoder allocations
    // together must stay below 1 MiB, regardless of catalog/track length.
    CHECK(allocations.bytesDelta() < 1024 * 1024);
    std::array<short, MusicStream::ChunkBytes / 2 * MusicStream::PrebufferChunks> out{};
    CHECK(stream->read(std::as_writable_bytes(std::span(out))) == sizeof(out));
    for (short sample : out) CHECK(sample == 2345);
}

TEST_CASE("AudioBackend/Microsoft ADPCM music decodes and loops as resident PCM") {
    AudioDevice device;
    REQUIRE(gos_CreateAudio());
    const auto path = std::filesystem::temp_directory_path() /
        ("mc2-stream-adpcm-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove(path); } } cleanup{path};
    SDL_RWops* file = SDL_RWFromFile(path.string().c_str(), "wb");
    REQUIRE(file);
    SDL_RWwrite(file, "RIFF", 1, 4); SDL_WriteLE32(file, 90);
    SDL_RWwrite(file, "WAVEfmt ", 1, 8); SDL_WriteLE32(file, 50);
    SDL_WriteLE16(file, 2); SDL_WriteLE16(file, 1); SDL_WriteLE32(file, 22050);
    SDL_WriteLE32(file, 44100); SDL_WriteLE16(file, 8); SDL_WriteLE16(file, 4);
    SDL_WriteLE16(file, 32); SDL_WriteLE16(file, 4); SDL_WriteLE16(file, 7);
    const std::array<short, 14> coefficients{256,0, 512,-256, 0,0, 192,64, 240,0, 460,-208, 392,-232};
    for (short coefficient : coefficients) SDL_WriteLE16(file, static_cast<Uint16>(coefficient));
    SDL_RWwrite(file, "fact", 1, 4); SDL_WriteLE32(file, 4); SDL_WriteLE32(file, 4);
    SDL_RWwrite(file, "data", 1, 4); SDL_WriteLE32(file, 8);
    const Uint8 predictor = 0, codes = 0;
    SDL_RWwrite(file, &predictor, 1, 1); SDL_WriteLE16(file, 16);
    SDL_WriteLE16(file, 1000); SDL_WriteLE16(file, 1000); SDL_RWwrite(file, &codes, 1, 1);
    SDL_RWclose(file);
    auto stream = MusicStream::open(path.string().c_str(), 22050, 2);
    REQUIRE(stream);
    stream->setLoop(true);
    std::array<short, 8 * MusicStream::PrebufferChunks> out{};
    CHECK(stream->read(std::as_writable_bytes(std::span(out))) == sizeof(out));
    for (short sample : out) CHECK(sample == 1000);
}
