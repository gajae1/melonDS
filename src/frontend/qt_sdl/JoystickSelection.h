// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef JOYSTICKSELECTION_H
#define JOYSTICKSELECTION_H

#include <SDL2/SDL.h>
#include <string>
#include <utility>
#include <vector>

// The caller holds the frontend joystick mutex. SDL's lock also keeps device
// indices stable between enumeration, identity resolution and opening a handle.
struct JoystickListLock
{
    JoystickListLock() { SDL_LockJoysticks(); }
    ~JoystickListLock() { SDL_UnlockJoysticks(); }
};

struct JoystickDevice
{
    int index = -1;
    SDL_JoystickID instance = -1;
    std::string guid, serial, name;
};

inline std::vector<JoystickDevice> ListJoysticks()
{
    std::vector<JoystickDevice> devices;
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
    {
        JoystickDevice device;
        device.index = i;
        device.instance = SDL_JoystickGetDeviceInstanceID(i);
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
        device.guid = guid;
        if (const char* name = SDL_JoystickNameForIndex(i)) device.name = name;
        // Serial is available since SDL 2.0.14, below the existing rumble API
        // floor. SDL paths are implementation dependent (and may be reused for
        // a different device); they are not a portable physical identity.
        if (SDL_Joystick* joystick = SDL_JoystickOpen(i))
        {
            if (const char* serial = SDL_JoystickGetSerial(joystick)) device.serial = serial;
            SDL_JoystickClose(joystick);
        }
        devices.push_back(std::move(device));
    }
    return devices;
}

struct JoystickSelection
{
    enum class Status { Connected, Disabled, Missing, Ambiguous };
    int legacyIndex = 0;
    JoystickDevice device;
    Status status = Status::Missing;
    bool allowLegacy = true;
    bool requireSelection = false;

    bool SameIdentity(const JoystickSelection& other) const
    {
        return legacyIndex == other.legacyIndex && device.guid == other.device.guid &&
               device.serial == other.device.serial;
    }

    void Select(int index, const std::vector<JoystickDevice>& devices)
    {
        *this = {};
        legacyIndex = index;
        if (index >= 0 && index < static_cast<int>(devices.size())) device = devices[index];
        Resolve(devices);
    }

    int Resolve(const std::vector<JoystickDevice>& devices)
    {
        status = Status::Missing;
        if (device.guid.empty())
        {
            const bool migrate = allowLegacy;
            allowLegacy = false;
            if (legacyIndex == -1) status = Status::Disabled;
            if (!migrate || legacyIndex < 0 || legacyIndex >= static_cast<int>(devices.size())) return -1;
            // A legacy number is used once, while it still refers to a present
            // device. Never clamp it to the first controller.
            device = devices[legacyIndex];
        }

        int match = -1;
        int sessionMatch = -1;
        int matches = 0;
        for (const auto& candidate : devices)
        {
            if (candidate.guid != device.guid) continue;
            if (candidate.instance == device.instance && device.instance >= 0)
            {
                sessionMatch = candidate.index;
            }
            if (!device.serial.empty() && candidate.serial != device.serial) continue;
            match = candidate.index;
            ++matches;
        }
        if (matches > 1) requireSelection = true;
        if (sessionMatch >= 0)
        {
            status = Status::Connected;
            return sessionMatch;
        }
        if (!matches) return -1;
        // GUID describes a model, not a physical controller. Without a serial,
        // reconnection/restart requires an explicit choice, even if only one
        // same-model controller is currently visible. Never take its sibling.
        if (device.serial.empty() || requireSelection)
        {
            status = Status::Ambiguous;
            return -1;
        }
        device = devices[match];
        status = Status::Connected;
        return match;
    }
};

#endif
