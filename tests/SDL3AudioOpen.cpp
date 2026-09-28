// SPDX-License-Identifier: GPL-3.0-or-later
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "Platform.h"
namespace melonDS::Platform { void Log(LogLevel, const char*, ...) {} }

static bool failOpen = false;
static std::string observedHint;
static SDL_AudioStream* OpenStream(SDL_AudioDeviceID device, const SDL_AudioSpec* spec,
                                   SDL_AudioStreamCallback callback, void* userdata)
{
    const char* hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    observedHint = hint ? hint : "";
    if (failOpen) { SDL_SetError("injected stream-open failure"); return nullptr; }
    return SDL_OpenAudioDeviceStream(device, spec, callback, userdata);
}
static bool SetHint(const char* name, const char* value)
{
    const bool result = SDL_SetHint(name, value);
    SDL_SetError("hint write replaced the error");
    return result;
}
#define SDL_OpenAudioDeviceStream OpenStream
#define SDL_SetHint SetHint
#include "AudioOutput.cpp"
#undef SDL_SetHint
#undef SDL_OpenAudioDeviceStream

int main()
{
    if (!SDL_Init(SDL_INIT_AUDIO)) return 2;
    bool passed = true;
    {
        AudioOutput output;
        std::string error;
        auto silent = [](void*, uint8_t* data, int bytes) { std::memset(data, 0, bytes); };
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, nullptr);
        passed &= output.Open({AudioOutput::SDL, {}, 128}, silent, nullptr, error);
        passed &= observedHint == "128" && !SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        passed &= output.Close();

        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "768");
        for (bool fail : {true, false})
        {
            failOpen = fail;
            passed &= output.Open({AudioOutput::SDL, {}, 256}, silent, nullptr, error) == !fail;
            const char* hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
            passed &= observedHint == "256" && hint && std::strcmp(hint, "768") == 0;
            if (fail) passed &= error == "injected stream-open failure";
            passed &= output.Close();
        }
    }
    SDL_Quit();
    std::printf("SDL3 output hint isolation and failure preservation: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
