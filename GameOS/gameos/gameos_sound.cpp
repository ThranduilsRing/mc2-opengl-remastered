#include "gameos.hpp"
#include "gameos_music_cache.h"
#include "gos_profiler.h"
#include <memory>
#include <utility>
#include <string>
#include <unordered_map>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include "utils/vec.h"
#include "mc2fmt.h"
#include "utils/mc2log.h"

static const DWORD INVALID_AUDIO_ID = -1;
static const DWORD MAX_VOLUME = 254;

class gosAudio {

    public:

        static gosAudio* makeAudio(SDL_AudioFormat fmt, int channels, int freq, std::shared_ptr<uint8_t[]> pdata, uint32_t size) {
            gosAudio* audio = new gosAudio(fmt, channels, freq, std::move(pdata), size);
            audio->mix_chunk_.allocated = 0;
            audio->mix_chunk_.abuf = audio->pdata_.get();
            audio->mix_chunk_.alen = size;
            audio->mix_chunk_.volume = 50; // just something default 
            return audio;
        }

        static void destroyAudio(gosAudio* paudio) {
            delete paudio;
        }

    private:

        gosAudio(SDL_AudioFormat fmt, int channels, int freq, std::shared_ptr<uint8_t[]> pdata, uint32_t size):
            fmt_(fmt),
            channels_(channels),
            freq_(freq),
            pdata_(std::move(pdata)),
            len_(size)
            {
            }

        ~gosAudio() {
            pdata_.reset();
        }

        SDL_AudioFormat fmt_;
        int channels_;
        int freq_;
        std::shared_ptr<uint8_t[]> pdata_;
        uint32_t len_;

    public:
        const std::shared_ptr<uint8_t[]>& data() const { return pdata_; }
        Mix_Chunk mix_chunk_;

};

class SoundEngine {

    public:
        struct CachedMusic {
            std::shared_ptr<uint8_t[]> data;
            uint32_t size;
        };
        // Only configured music is retained; voices and arbitrary streams keep their existing lifetime.
        static constexpr size_t MUSIC_CACHE_LIMIT = 256u * 1024u * 1024u;
        std::unordered_map<std::string, CachedMusic> musicCache;
        size_t musicCacheBytes = 0;
        SoundEngine():frequency_(0), format_(0), channels_(0), chunksize_(0), is_initialized_(false) {} 
        bool init(int frequency, bool b_fmt_16_bit, bool b_fmt_signed, bool b_stereo);
        void destroy();

        size_t addAudio(gosAudio* audio) {
            gosASSERT(audio);
            audioList_.push_back(audio);
            return audioList_.size()-1;
        }

        gosAudio* getAudio(DWORD audio_id) {
            // TODO: return default audio
            if(audio_id == INVALID_AUDIO_ID) {
                return nullptr;
            }
            gosASSERT(audioList_.size() > audio_id);
            gosASSERT(audioList_[audio_id] != 0);
            return audioList_[audio_id];
        }

        void deleteAudio(gosAudio* audio) {
            gosAudio::destroyAudio(audio);
            std::vector<gosAudio*>::iterator it = std::remove(audioList_.begin(), audioList_.end(), audio);
            audioList_.erase(it, audioList_.end());
        }

        SDL_AudioFormat getFormat() const { return format_; }
        int getNumChannels() const { return channels_; }
        int getFrequency() const { return frequency_; }

        gosAudio_ChannelInfo* getChannelsInfo() { return channel_info_; }
        const gosAudio_ChannelInfo* getChannelsInfo() const { return channel_info_; }

        gosAudio_ChannelInfo* getChannel(int i) {
            gosASSERT(i<NUM_CHANNELS); return &channel_info_[i];
        }
        const gosAudio_ChannelInfo* getChannel(int i) const {
            gosASSERT(i<NUM_CHANNELS); return &channel_info_[i];
        }

        bool needsConversion(int format, int channels, int freq) const {
            return format != format_ || channels != channels_ || freq != frequency_;
        }

        static const int NUM_CHANNELS = 32;
    private:
        std::vector<gosAudio*> audioList_;
        int frequency_; // Hz
        SDL_AudioFormat format_; // see SDL_audio.h
        int channels_; // mono/stereo
        int chunksize_;

        bool is_initialized_;

        gosAudio_ChannelInfo channel_info_[NUM_CHANNELS];

};


bool SoundEngine::init(int frequency, bool b_fmt_16_bit, bool b_fmt_signed, bool b_stereo) {

    if(SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        STOP(("Failed to initialize SoundEngine: MSDL_InitSubSystem: %s\n", SDL_GetError()));
    }

    int initted = Mix_Init(0);
    (void)initted;

    frequency_ = frequency;
    if(b_fmt_16_bit) {
        format_ = b_fmt_signed ? AUDIO_S16LSB : AUDIO_U16LSB;
    } else {
        format_ = b_fmt_signed ? AUDIO_S8 : AUDIO_U8;
    }
    channels_ = b_stereo ? 2 : 1;
    chunksize_ = 1024;

    int rv = Mix_OpenAudio(frequency_, format_, channels_, chunksize_);
    if(rv == -1) {
        PAUSE(("Failed to initialize SoundEngine: Mix_OpenAudio: %s\n", Mix_GetError()));
    }


    {
        int audio_rate;
        Uint16 audio_format;
        int audio_channels;
        int audio_buffers = chunksize_;

        Mix_QuerySpec(&audio_rate, &audio_format, &audio_channels);             
        mc2::fmt::print_out("Opened audio at {:d} Hz {:d} bit {} ({}), {:d} bytes audio buffer\n", mc2::fmt::as_i32(audio_rate), mc2::fmt::as_i32((audio_format&0xFF)), mc2::fmt::safe((audio_channels > 2) ? "surround" : (audio_channels > 1) ? "stereo" : "mono"), mc2::fmt::safe((audio_format&0x1000) ? "Big-Endian" : "Little-Endian"), mc2::fmt::as_i32(audio_buffers));  
    }

    int num_allocated_chanels = Mix_AllocateChannels(NUM_CHANNELS);
    gosASSERT(NUM_CHANNELS == num_allocated_chanels);

    memset(&channel_info_, 0, sizeof(channel_info_));
    for(int i=0;i<NUM_CHANNELS;++i) {
        channel_info_[i].ePlayMode = gosAudio_Stop;
        //Mix_Volume(i, 127);
    }

    return rv == -1 ? false : true;
}

void SoundEngine::destroy() {

    for(int i=0; i<NUM_CHANNELS;++i) {
        Mix_HaltChannel(i);
    }

    std::vector<gosAudio*>::iterator it = audioList_.begin();
    for(;it!=audioList_.end();++it) {
            gosAudio::destroyAudio(*it);
    }
    audioList_.clear();
    musicCache.clear();
    musicCacheBytes = 0;

    Mix_CloseAudio();
    Mix_Quit();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    is_initialized_ = false;
}

SoundEngine* g_sound_engine = nullptr;

////////////////////////////////////////////////////////////////////////////////
// possibly pass parameters
bool gos_CreateAudio() {
    if(!g_sound_engine) {
        g_sound_engine = new SoundEngine();
        return g_sound_engine->init(44100/2, true, true, true);
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////
void gos_DestroyAudio() {
    if(g_sound_engine) {
        g_sound_engine->destroy();
        delete g_sound_engine;
        g_sound_engine = nullptr;
    }
}

#define USE_MIX_LOAD

//////////////////////////////////////////////////////////////////////////////////
// Creates a resource to be played later
//
void __stdcall gosAudio_CreateResource( HGOSAUDIO* hgosaudio, enum gosAudio_ResourceType res_type, const char* file_name, gosAudio_Format* ga_wf, void* data, int size, bool only2D)
{
    gosASSERT(hgosaudio);
    gosASSERT((res_type == gosAudio_UserMemory && data && ga_wf) || (res_type == gosAudio_StreamedFile && file_name));

	//WORD  wFormatTag;				// Waveform-audio format type. 1=PCM, 2=Microsoft ADPCM.
	//WORD  nChannels; 				// 1=Mono, 2=Stereo.
    //DWORD nSamplesPerSec;			// Sample rate, 11025Hz, 22050Hz or 44100Hz.
	//DWORD nAvgBytesPerSec;          // Normally, nBlockAlign * nSamplesPerSec
	//WORD  nBlockAlign; 				// Normally, wBitsPerSample / 8 * nChannels
    //WORD  wBitsPerSample;			// Bits per sample for the wFormatTag format type. If wFormatTag is 1 (PCM), then wBitsPerSample should be equal to 8 or 16.
	//WORD  cbSize;					// Size, in bytes, of extra format information appended to the end of the WAVEFORMATEX structure. For PCM's, this should be set to 0. For ADPCM, this should be set to 32.

    uint8_t* databuf = nullptr;
    uint32_t src_datasize = 0;

    SDL_AudioFormat src_fmt = 0;
    int src_channels = 0;
    int src_freq = 0;
    bool from_file = false;

#ifdef USE_MIX_LOAD
    Mix_Chunk* chunk = nullptr;
#endif

    const SDL_AudioFormat dst_fmt = g_sound_engine->getFormat();
    const int dst_channels = g_sound_engine->getNumChannels();
    const int dst_freq = g_sound_engine->getFrequency();

    if(res_type == gosAudio_StreamedFile) {
        ZoneScopedN("Audio.Stream.LoadFile");

        if (auto cached = g_sound_engine->musicCache.find(file_name); cached != g_sound_engine->musicCache.end()) {
            const auto& [path, music] = *cached;
            auto* audio = gosAudio::makeAudio(dst_fmt, dst_channels, dst_freq, music.data, music.size);
            g_sound_engine->addAudio(audio);
            *hgosaudio = audio;
            return;
        }

        from_file = true;

#ifndef USE_MIX_LOAD
        SDL_AudioSpec spec;
        SDL_AudioSpec* pspec;
        pspec = SDL_LoadWAV(file_name, &spec, &databuf, &src_datasize);
        if(pspec == nullptr) {
            PAUSE(("gosAudio_CreateResource: Failed to load %s, %s\n", file_name, SDL_GetError()));
            *hgosaudio = nullptr;
            return;
        }

        src_fmt = spec.format;
        src_freq = spec.freq;
        src_channels = spec.channels;

        switch(spec.format)
        {
            case AUDIO_U8: mc2::fmt::print_out("audio_u8\n"); break;
            case AUDIO_S8: mc2::fmt::print_out("audio_s8\n"); break;
            case AUDIO_S16LSB: mc2::fmt::print_out("audio_u16lsb\n"); break;
            case AUDIO_U16LSB: mc2::fmt::print_out("AUDIO_s16lsb\n"); break;
            default: STOP(("Unsupported audio format\n"));
        }
#else
        chunk = Mix_LoadWAV(file_name);
        if(nullptr == chunk) {
            MC2_LOG(Audio, Debug, "gosAudio_CreateResource: Failed to load {}, {}", mc2::fmt::safe(file_name), mc2::fmt::safe(Mix_GetError()));
            *hgosaudio = nullptr;
            return;
        }
        databuf = chunk->abuf;
        src_datasize = chunk->alen;
        // Mix library always loads data in our output format
        src_freq = dst_freq;
        src_fmt = dst_fmt;
        src_channels = dst_channels;
#endif

    } else {
        gosASSERT(res_type == gosAudio_UserMemory);
        src_freq = ga_wf->nSamplesPerSec;
        // WAV spec: 8-bit PCM is UNSIGNED (silence=128), 16-bit PCM is signed.
        src_fmt = ga_wf->wBitsPerSample==8 ? AUDIO_U8 : AUDIO_S16LSB;
        src_channels = ga_wf->nChannels;
        databuf = static_cast<uint8_t*>(data);
        src_datasize = size;
    }

    uint32_t num_datapoints = src_datasize /(src_channels * ((SDL_AUDIO_MASK_BITSIZE & src_fmt) / 8));
    uint32_t audio_data_size = num_datapoints * dst_channels * ((SDL_AUDIO_MASK_BITSIZE & dst_fmt) / 8);
    double mul_k = static_cast<double>(dst_freq) / static_cast<double>(src_freq);
    audio_data_size =static_cast<uint32_t>((audio_data_size * mul_k));

    auto audioDataOwner = std::make_unique_for_overwrite<uint8_t[]>(audio_data_size);
    uint8_t* audio_data = audioDataOwner.get();
    memset(audio_data, 0, audio_data_size);
    std::unique_ptr<uint8_t[]> tmpReqBufOwner;
    uint8_t* tmp_req_buf = nullptr;

    const bool needs_conversion = g_sound_engine->needsConversion(src_fmt, src_channels, src_freq);
    if(needs_conversion) {

		SDL_AudioCVT cvt = { 0 };

        if(0 > SDL_BuildAudioCVT(&cvt,
                    src_fmt, src_channels, src_freq,
                    dst_fmt, dst_channels, dst_freq)) {

            PAUSE(("SDL_BuildAudioCVT: Failed to build conversion structure : %s\n", SDL_GetError()));
            audioDataOwner.reset();
            if(from_file)
                SDL_FreeWAV(databuf);
            *hgosaudio = nullptr;
            return;
        }

        cvt.len = src_datasize;

        gosASSERT(cvt.needed);
        const uint32_t tmp_req_buf_size = cvt.len * cvt.len_mult;

        if(src_datasize >= tmp_req_buf_size) {
            cvt.buf = databuf;
        } else {
            tmpReqBufOwner = std::make_unique_for_overwrite<uint8_t[]>(tmp_req_buf_size);
            tmp_req_buf = tmpReqBufOwner.get();
            memcpy(tmp_req_buf, databuf, src_datasize);
            cvt.buf = tmp_req_buf;
        }

        // conversion is in-place
        if(0 > SDL_ConvertAudio(&cvt)) {
            PAUSE(("Failed to convert audio: %s\n", SDL_GetError()));
            audioDataOwner.reset();
            if(from_file)
                SDL_FreeWAV(databuf);
            *hgosaudio = nullptr;
            tmpReqBufOwner.reset();
            return;
        }

		// for some reason len_cvt may be less than required :-/
        gosASSERT(cvt.len_cvt <= static_cast<int>(audio_data_size));
		if(cvt.len_cvt < static_cast<int>(audio_data_size))
            MC2_LOG(Audio, Debug, "cvt.len_cvt != audio_data_len {:d} != {:d}", mc2::fmt::as_i32(cvt.len_cvt), mc2::fmt::as_i32(audio_data_size));
		if (cvt.len_cvt < static_cast<int>(audio_data_size))
			audio_data_size = cvt.len_cvt;

        // if conversion took place in original buffer
        if(audio_data != cvt.buf) {
            memcpy(audio_data, cvt.buf, audio_data_size);
        }

    } else {
        memcpy(audio_data, databuf, audio_data_size);
    }

    tmpReqBufOwner.reset();

#ifndef USE_MIX_LOAD
    if(from_file)
        SDL_FreeWAV(databuf);
#else
    if(from_file)
        Mix_FreeChunk(chunk);
#endif

    gosAudio* paudio = gosAudio::makeAudio(dst_fmt, dst_channels, dst_freq, std::move(audioDataOwner), audio_data_size);
    if(!paudio) {
        PAUSE(("makeAudio: Failed to create audio resource\n"));
        return;
    }
    g_sound_engine->addAudio(paudio);
    *hgosaudio = paudio;
}

bool gosAudio_PreloadMusic(std::string_view path) {
    ZoneScopedN("Audio.Music.Predecode");
    if (!g_sound_engine || path.empty()) return false;
    const std::string file_name(path);
    if (g_sound_engine->musicCache.contains(file_name)) return true;
    if (g_sound_engine->musicCacheBytes >= SoundEngine::MUSIC_CACHE_LIMIT) return false;
    HGOSAUDIO handle = nullptr;
    gosAudio_CreateResource(&handle, gosAudio_StreamedFile, file_name.c_str());
    if (!handle) return false;
    const auto size = handle->mix_chunk_.alen;
    const bool fits = size <= SoundEngine::MUSIC_CACHE_LIMIT - g_sound_engine->musicCacheBytes;
    if (fits) {
        g_sound_engine->musicCache.emplace(file_name, SoundEngine::CachedMusic{handle->data(), size});
        g_sound_engine->musicCacheBytes += size;
    }
    gosAudio_DestroyResource(&handle);
    return fits;
}

void gosAudio_ClearMusicCache() {
    if (!g_sound_engine) return;
    g_sound_engine->musicCache.clear();
    g_sound_engine->musicCacheBytes = 0;
}

//////////////////////////////////////////////////////////////////////////////////
// Destroy a resource; any sounds currently playing using the ResourceID will be
//  stopped.
//  Any memory the SoundAPI associated with the resource will be freed.
//
void __stdcall gosAudio_DestroyResource(HGOSAUDIO* handle)
{
    if (!handle || !*handle) return;
    auto* audio = static_cast<gosAudio*>(*handle);
    *handle = nullptr;
    // Engine shutdown already freed its complete resource list.
    if (!g_sound_engine) return;
    auto* channels = g_sound_engine->getChannelsInfo();
    for (int i = 0; i < SoundEngine::NUM_CHANNELS; ++i) {
        if (channels[i].hAudio == audio) {
            Mix_HaltChannel(i);
            channels[i].hAudio = nullptr;
            channels[i].ePlayMode = gosAudio_Stop;
        }
    }
    // Resources may be unbound, or shared by several channels. Free exactly once.
    g_sound_engine->deleteAudio(audio);
}

//////////////////////////////////////////////////////////////////////////////////
// This prepares the channel for a specific type of sound playback. Optimally,
// allocate only the properties that will need modification. Use a bitwise'd group
// of gosAudio_Properties to set what is needed.
//
void __stdcall gosAudio_AllocateChannelSliders( int Channel, DWORD properties)
{
    gosASSERT(g_sound_engine);
    gosASSERT(g_sound_engine->NUM_CHANNELS > Channel);
    gosAudio_ChannelInfo* ci = g_sound_engine->getChannel(Channel);
    if(!ci)
        return;

    ci->dwProperties = properties;
    // do some real work here?
}

//////////////////////////////////////////////////////////////////////////////////
// Prepare a channel to play a resource of any type.
//
void __stdcall gosAudio_AssignResourceToChannel( int Channel, HGOSAUDIO hgosaudio)
{
    gosASSERT(g_sound_engine);
    gosASSERT(g_sound_engine->NUM_CHANNELS > Channel);
    gosAudio_ChannelInfo* ci = g_sound_engine->getChannel(Channel);
    if(!ci)
        return;

    // this should only assign resource to channel but not play it
    gosAudio* audio = static_cast<gosAudio*>(hgosaudio);
    ci->hAudio = audio;
}

//////////////////////////////////////////////////////////////////////////////////
// Get and Set functions only operate if a channel has the property enabled
//  Channel number -1 used in SetVolume and SetPanning will alter the windows master
//  volume and balance
void __stdcall gosAudio_SetChannelSlider( int Channel, enum gosAudio_Properties prop, float value1, float value2, float value3)
{
    gosASSERT(g_sound_engine);
    gosASSERT(g_sound_engine->NUM_CHANNELS > Channel);
    gosAudio_ChannelInfo* ci = g_sound_engine->getChannel(Channel);
    if(!ci)
        return;

    switch(prop) {
        case gosAudio_Common: break; //?
        case gosAudio_Volume:
        {
            ci->fVolume = saturate(value1);
            Mix_Volume(Channel, static_cast<DWORD>((ci->fVolume*MAX_VOLUME)));
            //printf("Mix_Volume(%d, %d)\n", Channel, (DWORD)(ci->fVolume*MAX_VOLUME));
            break;
        }
        case gosAudio_Panning:
        {
            const float pan = std::isfinite(value1) ? std::clamp(value1, -1.0f, 1.0f) : 0.0f;
            ci->fPanning = pan;
            // SDL panning attenuates stereo sides independently of Mix_Volume.
            // Center leaves both sides at full gain; either extreme silences only its opposite side.
            const auto left = static_cast<Uint8>(255.0f * (pan > 0.0f ? 1.0f - pan : 1.0f));
            const auto right = static_cast<Uint8>(255.0f * (pan < 0.0f ? 1.0f + pan : 1.0f));
            Mix_SetPanning(Channel, left, right);
            break;
        }
        case gosAudio_Frequency:
        {
            PAUSE(("Cannot change frequency on a specific channel\n"));
            break;
        }
        default:
            PAUSE(("Slider not supported yet\n"));
    }
}

void __stdcall gosAudio_GetChannelSlider( int Channel, enum gosAudio_Properties prop, float* value1, float* value2, float* value3)
{
    gosASSERT(g_sound_engine);
    gosASSERT(g_sound_engine->NUM_CHANNELS > Channel);
    gosAudio_ChannelInfo* ci = g_sound_engine->getChannel(Channel);
    if(!ci)
        return;

    gosASSERT(value1);

    switch(prop) {
        case gosAudio_Common: break; //?
        case gosAudio_Volume: *value1 = ci->fVolume; break;
        case gosAudio_Panning: *value1 = ci->fPanning; break; // -1 ... +1
        case gosAudio_Frequency:
        {
            PAUSE(("Cannot change frequency on a specific channel\n"));
            break;
        }
        default:
            PAUSE(("Slider not supported yet\n"));
    }
}

//////////////////////////////////////////////////////////////////////////////////
// Play, Loop, Stop, Pause, or Continue a particular channel
//
void __stdcall gosAudio_SetChannelPlayMode( int Channel, enum gosAudio_PlayMode ga_pm )
{
    gosASSERT(g_sound_engine);
    gosASSERT(g_sound_engine->NUM_CHANNELS > Channel);
    gosAudio_ChannelInfo* ci = g_sound_engine->getChannel(Channel);
    if(!ci)
        return;

    ci->ePlayMode = ga_pm;

    switch(ga_pm) {
        case gosAudio_PlayOnce:
        case gosAudio_Loop:
        {
            gosAudio* audio = static_cast<gosAudio*>(ci->hAudio);
            if(audio) {
                int loops = ci->ePlayMode == gosAudio_Loop  ? -1 : 0;
                Mix_PlayChannel(Channel, &audio->mix_chunk_, loops);
                // SDL removes effects when a chunk finishes or is halted; GOS sliders persist.
                gosAudio_SetChannelSlider(Channel, gosAudio_Panning, ci->fPanning);
            }
            break;
        }
        case gosAudio_Pause:
        {
            Mix_Pause(Channel);
            break;
        }
        case gosAudio_Continue:
        {
            Mix_Resume(Channel);
            break;
        }
        case gosAudio_Stop:
        {
            Mix_HaltChannel(Channel);
            break;
        }
    }
}
//////////////////////////////////////////////////////////////////////////////////
// Determine the current play mode of a channel
//
gosAudio_PlayMode __stdcall gosAudio_GetChannelPlayMode( int Channel )
{
    gosASSERT(g_sound_engine);
    gosASSERT(g_sound_engine->NUM_CHANNELS > Channel);
    gosAudio_ChannelInfo* ci = g_sound_engine->getChannel(Channel);
    if(!ci) {
        return gosAudio_PlayOnce;
    }
    int playing = Mix_Playing(Channel);
    return playing ? ci->ePlayMode : gosAudio_Stop;
}

