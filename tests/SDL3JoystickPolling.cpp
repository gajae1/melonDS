// SPDX-License-Identifier: GPL-3.0-or-later
// Actual extracted input/open/close/button methods and a real SDL3 virtual pad.
// No window, emulation, physical controller or latency measurement.
#include "SDLCompat.h"
#include "JoystickSelection.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>

static void Check(bool ok, const char* what)
{
    if (!ok) throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

class EmuInstance
{
public:
    static constexpr int HK_MAX = 1;
    int joystickID = -1;
    JoystickSelection joystickSelection;
    std::vector<SDL_JoystickID> joystickTopology;
    Uint32 joystickLastOpen = 0;
    SDL_Joystick* joystick = nullptr;
    SDL_Gamepad* controller = nullptr;
    bool hasRumble = false, hasAccelerometer = false, hasGyroscope = false, isRumbling = false;
    std::shared_ptr<SDL_Mutex> joyMutex{SDL_CreateMutex(), SDL_DestroyMutex};
    std::array<int, 12> joyMapping;
    std::array<int, HK_MAX> hkJoyMapping{0};
    std::atomic<unsigned> keyInputMask{0xFFF}, keyHotkeyMask{0};
    unsigned joyInputMask = 0xFFF, inputMask = 0xFFF, joyHotkeyMask = 0;
    unsigned hotkeyMask = 0, lastHotkeyMask = 0, hotkeyPress = 0, hotkeyRelease = 0;
    EmuInstance()
    {
        Check(bool(joyMutex), "mutex");
        joyMapping.fill(-1); joyMapping[0] = 0; joyMapping[6] = 0x1FFFF;
    }
    ~EmuInstance() { closeJoystick(); }
    void openJoystick();
    void closeJoystick();
    bool joystickButtonDown(int val);
    void inputProcess();
    void setJoystick(int id);
};
#include "joystickOpen.inc"
#include "joystickClose.inc"
#include "joystickButton.inc"
#include "joystickProcess.inc"
#include "joystickSet.inc"

struct VirtualPad
{
    SDL_JoystickID id = 0;
    unsigned updates = 0;
    bool down = false, sensors = false;
    float sample = 42;
    static void SDLCALL Update(void* data)
    {
        auto& p = *static_cast<VirtualPad*>(data);
        ++p.updates;
        if (auto* j = SDL_GetJoystickFromID(p.id))
        {
            SDL_SetJoystickVirtualButton(j, 0, p.down);
            SDL_SetJoystickVirtualAxis(j, 0, p.down ? 20000 : 0);
            if (p.sensors)
            {
                const float values[] = {p.sample, 2, 3};
                SDL_SendJoystickVirtualSensorData(j, SDL_SENSOR_ACCEL, SDL_GetTicksNS(), values, 3);
            }
        }
    }
    static bool SDLCALL Sensors(void* data, bool enabled)
    {
        static_cast<VirtualPad*>(data)->sensors = enabled; return true;
    }
    void Attach()
    {
        SDL_VirtualJoystickSensorDesc sensor{SDL_SENSOR_ACCEL, 60};
        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.name = "melonDS polling regression";
        desc.nbuttons = 1; desc.button_mask = 1 << SDL_GAMEPAD_BUTTON_SOUTH;
        desc.naxes = 1; desc.axis_mask = 1 << SDL_GAMEPAD_AXIS_LEFTX;
        desc.nsensors = 1; desc.sensors = &sensor;
        desc.userdata = this; desc.Update = Update; desc.SetSensorsEnabled = Sensors;
        id = SDL_AttachVirtualJoystick(&desc);
        Check(id != 0, "attach virtual gamepad");
    }
    void Detach() { Check(SDL_DetachVirtualJoystick(id), "detach"); id = 0; }
    ~VirtualPad() { if (id) SDL_DetachVirtualJoystick(id); }
};

int main()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) return 2;
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
    int result = 0;
    try
    {
        VirtualPad pad;
        pad.Attach();
        EmuInstance input;
        const auto devices = ListJoysticks();
        bool selected = false;
        for (const auto& d : devices)
            if (d.instance == pad.id) { input.joystickSelection.Select(d.index, devices); selected = true; }
        Check(selected, "virtual pad selection");
        input.inputProcess();
        Check(input.controller && input.hasAccelerometer && pad.sensors, "gamepad/sensor open");
        float values[3]{};
        Check(SDL_GetGamepadSensorData(input.controller, SDL_SENSOR_ACCEL, values, 3) && values[0] == pad.sample,
              "newly enabled sensor must update during the first input call");
        for (bool down : {true, true, false, false})
        {
            const bool wasDown = !(input.inputMask & 1);
            pad.down = down; pad.sample += 1;
            const auto before = pad.updates;
            input.inputProcess();
            std::printf("steady updates=%u down=%d\n", pad.updates - before, down);
            Check(pad.updates - before == 1, "duplicate steady-state backend poll");
            Check(bool(input.inputMask & 1) == !down && bool(input.inputMask & (1 << 6)) == !down,
                  "button/axis state");
            Check(bool(input.hotkeyPress) == (down && !wasDown) && bool(input.hotkeyRelease) == (!down && wasDown),
                  "hotkey edges");
            Check(SDL_GetGamepadSensorData(input.controller, SDL_SENSOR_ACCEL, values, 3) && values[0] == pad.sample,
                  "steady sensor data");
        }
        pad.Detach(); input.inputProcess();
        Check(!input.joystick && !input.controller && input.inputMask == 0xFFF, "detach neutral state");
        pad.Attach(); pad.down = true; pad.sample = 99;
        input.inputProcess();
        // Virtual pads have no serial: the production identity policy requires
        // an explicit selection instead of silently taking a same-model pad.
        Check(!input.controller && input.joystickSelection.status == JoystickSelection::Status::Ambiguous,
              "serial-less reconnect must remain unresolved");
        for (const auto& d : ListJoysticks())
            if (d.instance == pad.id) input.setJoystick(d.index);
        input.inputProcess();
        Check(input.controller && !(input.inputMask & 1), "reconnect input");
        Check(SDL_GetGamepadSensorData(input.controller, SDL_SENSOR_ACCEL, values, 3) && values[0] == 99,
              "reconnect sensor refresh");
        std::puts("SDL3 polling, input edges, sensor first/steady/reconnect: PASS");
    }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); result = 1; }
    SDL_Quit();
    return result;
}
