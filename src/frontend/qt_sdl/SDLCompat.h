// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MELONDS_SDLCOMPAT_H
#define MELONDS_SDLCOMPAT_H

// Single include point for SDL in the Qt frontend. Sources are written against
// the SDL3 API surface. In an SDL2 build this header maps the SDL3 names used
// here back to their SDL2 equivalents so both builds share one source.
//
// Only APIs with identical semantics are mapped. Where SDL3 actually changed
// the model -- audio device streams, audio device enumeration, WAV conversion
// -- the call sites carry a real #ifdef MELONDS_SDL3 implementation instead.

#ifdef MELONDS_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL2/SDL.h>

// --- types -----------------------------------------------------------------
typedef SDL_mutex SDL_Mutex;
typedef SDL_cond SDL_Condition;
typedef SDL_GameController SDL_Gamepad;
typedef void SDL_SharedObject; // SDL3 gives the shared-library handle a type
// SDL_JoystickID already exists in SDL2 (Sint32 instance id).

// --- condition variables ----------------------------------------------------
#define SDL_CreateCondition SDL_CreateCond
#define SDL_DestroyCondition SDL_DestroyCond
#define SDL_SignalCondition SDL_CondSignal
#define SDL_BroadcastCondition SDL_CondBroadcast
// SDL3 returns bool (true = woken); SDL2 returns 0 or SDL_MUTEX_TIMEDOUT.
// Compat wrappers that bottom out in an SDL2 call are macros, not static
// inlines: test harnesses re-#define the SDL2 name to inject fakes, and an
// inline binds the inner call before their #define exists.
#define SDL_WaitConditionTimeout(cond, mutex, ms) (SDL_CondWaitTimeout(cond, mutex, (Uint32)(ms)) == 0)

// --- joystick ---------------------------------------------------------------
#define SDL_UpdateJoysticks SDL_JoystickUpdate
#define SDL_CloseJoystick SDL_JoystickClose
#define SDL_GetJoystickAxis SDL_JoystickGetAxis
#define SDL_GetJoystickHat SDL_JoystickGetHat
#define SDL_GetJoystickSerial SDL_JoystickGetSerial
#define SDL_GetNumJoystickAxes SDL_JoystickNumAxes
#define SDL_GetNumJoystickBalls SDL_JoystickNumBalls
#define SDL_GetNumJoystickHats SDL_JoystickNumHats
#define SDL_GetNumJoystickButtons SDL_JoystickNumButtons
#define SDL_GUIDToString SDL_JoystickGetGUIDString
#define SDL_GetJoystickID SDL_JoystickInstanceID

static inline bool SDL_GetJoystickButton(SDL_Joystick* joystick, int button)
{
    return SDL_JoystickGetButton(joystick, button) != 0;
}
static inline bool SDL_JoystickConnected(SDL_Joystick* joystick)
{
    return SDL_JoystickGetAttached(joystick) == SDL_TRUE;
}

// SDL3 enumerates joysticks as a list of instance IDs; SDL2 enumerates device
// indices. These helpers present the SDL3 ID-based model on SDL2. Callers must
// hold SDL_LockJoysticks() so the index<->ID mapping stays stable.
static inline int SDLCompat_JoystickIndexFromID(SDL_JoystickID id)
{
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i)
        if (SDL_JoystickGetDeviceInstanceID(i) == id) return i;
    return -1;
}
static inline SDL_JoystickID* SDL_GetJoysticks(int* count)
{
    const int n = SDL_NumJoysticks();
    SDL_JoystickID* ids = (SDL_JoystickID*)SDL_malloc(sizeof(SDL_JoystickID) * (n > 0 ? n : 1));
    if (ids)
        for (int i = 0; i < n; ++i) ids[i] = SDL_JoystickGetDeviceInstanceID(i);
    if (count) *count = ids ? n : 0;
    return ids;
}
#define SDL_OpenJoystick(id) SDL_JoystickOpen(SDLCompat_JoystickIndexFromID(id))
static inline const char* SDL_GetJoystickNameForID(SDL_JoystickID id)
{
    const int index = SDLCompat_JoystickIndexFromID(id);
    return index < 0 ? nullptr : SDL_JoystickNameForIndex(index);
}
static inline SDL_JoystickGUID SDL_GetJoystickGUIDForID(SDL_JoystickID id)
{
    const int index = SDLCompat_JoystickIndexFromID(id);
    return index < 0 ? SDL_JoystickGUID{} : SDL_JoystickGetDeviceGUID(index);
}
static inline void SDL_SetJoystickEventsEnabled(bool enabled)
{
    SDL_JoystickEventState(enabled ? SDL_ENABLE : SDL_DISABLE);
}

// --- gamepad ----------------------------------------------------------------
#define SDL_CloseGamepad SDL_GameControllerClose
#define SDL_RumbleGamepad SDL_GameControllerRumble

static inline bool SDL_IsGamepad(SDL_JoystickID id)
{
    const int index = SDLCompat_JoystickIndexFromID(id);
    return index >= 0 && SDL_IsGameController(index) == SDL_TRUE;
}
#define SDL_OpenGamepad(id) SDL_GameControllerOpen(SDLCompat_JoystickIndexFromID(id))
#define SDL_GamepadHasSensor(gamepad, type) (SDL_GameControllerHasSensor(gamepad, type) == SDL_TRUE)
#define SDL_SetGamepadSensorEnabled(gamepad, type, enabled) (SDL_GameControllerSetSensorEnabled(gamepad, type, (enabled) ? SDL_TRUE : SDL_FALSE) == 0)
#define SDL_GetGamepadSensorData(gamepad, type, data, num_values) (SDL_GameControllerGetSensorData(gamepad, type, data, num_values) == 0)

// --- audio sample formats ----------------------------------------------------
#define SDL_AUDIO_S16   AUDIO_S16SYS
#define SDL_AUDIO_S16LE AUDIO_S16LSB

#endif // MELONDS_SDL3

// --- calls whose result contract changed, one real body per version ----------
static inline bool SDLCompat_Init(Uint32 flags)
{
#ifdef MELONDS_SDL3
    return SDL_Init(flags);
#else
    return SDL_Init(flags) == 0;
#endif
}
#ifdef MELONDS_SDL3
static inline bool SDLCompat_GamepadHasRumble(SDL_Gamepad* gamepad)
{
    return SDL_GetBooleanProperty(SDL_GetGamepadProperties(gamepad),
        SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}
#else
#define SDLCompat_GamepadHasRumble(gamepad) (SDL_GameControllerHasRumble(gamepad) == SDL_TRUE)
#endif

#endif // MELONDS_SDLCOMPAT_H
