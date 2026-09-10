// SPDX-License-Identifier: GPL-3.0-or-later
// Current production methods are extracted at build time. SDL virtual devices,
// opens/closes, button/hat/axis sampling and rumble callbacks are real.
// SDL2 virtual devices cannot expose motion sensors: only sensor capability and
// enable results are supplied at that boundary. Open failures are injected there
// too. Handle observers report stale use without dereferencing freed SDL memory.
// No emulation thread, physical controller or motion sample is exercised here.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
#include "types.h"
#include "Platform.h"
// Virtual SDL2 devices cannot report serials. Supply only that identity
// boundary for serial reconnect cases; enumeration/handles/input remain SDL.
static std::map<SDL_JoystickID, std::string> deviceSerials;
static const char* DeviceSerial(SDL_Joystick* joystick)
{
    const auto found = deviceSerials.find(SDL_JoystickInstanceID(joystick));
    return found == deviceSerials.end() ? SDL_JoystickGetSerial(joystick) : found->second.c_str();
}
#define SDL_JoystickGetSerial DeviceSerial
#include "../src/frontend/qt_sdl/JoystickSelection.h"
#undef SDL_JoystickGetSerial

static int failures = 0;
static void Check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
}
static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(std::string(message) + ": " + SDL_GetError());
}

static std::vector<SDL_Joystick*> joysticks;
static std::vector<SDL_GameController*> controllers;
static bool failJoystickOpen = false, failControllerOpen = false;
static SDL_JoystickID sensorDevice = -1;
static int staleUses = 0;

static bool Live(SDL_GameController* controller)
{
    if (std::find(controllers.begin(), controllers.end(), controller) != controllers.end()) return true;
    ++staleUses;
    return false;
}
static SDL_Joystick* OpenJoystick(int index)
{
    if (failJoystickOpen) { SDL_SetError("injected joystick open failure"); return nullptr; }
    auto* joystick = SDL_JoystickOpen(index);
    if (joystick) joysticks.push_back(joystick);
    return joystick;
}
static void CloseJoystick(SDL_Joystick* joystick)
{
    const auto found = std::find(joysticks.begin(), joysticks.end(), joystick);
    if (found == joysticks.end()) { ++staleUses; return; }
    joysticks.erase(found);
    SDL_JoystickClose(joystick);
}
static SDL_GameController* OpenController(int index)
{
    if (failControllerOpen) { SDL_SetError("injected controller open failure"); return nullptr; }
    auto* controller = SDL_GameControllerOpen(index);
    if (controller) controllers.push_back(controller);
    return controller;
}
static void CloseController(SDL_GameController* controller)
{
    if (!Live(controller)) return;
    controllers.erase(std::find(controllers.begin(), controllers.end(), controller));
    SDL_GameControllerClose(controller);
}
static SDL_bool HasRumble(SDL_GameController* controller)
{
    return Live(controller) ? SDL_GameControllerHasRumble(controller) : SDL_FALSE;
}
static SDL_bool HasSensor(SDL_GameController* controller, SDL_SensorType type)
{
    if (!Live(controller)) return SDL_FALSE;
    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == sensorDevice)
        return SDL_TRUE;
    return SDL_GameControllerHasSensor(controller, type);
}
static int EnableSensor(SDL_GameController* controller, SDL_SensorType type, SDL_bool enabled)
{
    if (!Live(controller)) return -1;
    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == sensorDevice) return 0;
    return SDL_GameControllerSetSensorEnabled(controller, type, enabled);
}
static int Rumble(SDL_GameController* controller, Uint16 low, Uint16 high, Uint32 duration)
{
    return Live(controller) ? SDL_GameControllerRumble(controller, low, high, duration) : -1;
}
static int SensorData(SDL_GameController* controller, SDL_SensorType type, float* data, int count)
{
    if (!Live(controller)) return -1;
    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) != sensorDevice)
        return SDL_GameControllerGetSensorData(controller, type, data, count);
    for (int i = 0; i < count; ++i) data[i] = float(i + 1);
    return 0;
}

// Two independent hotkeys suffice to exercise merging and edge transitions;
// this fixture supplies host state, not the application's hotkey catalogue.
constexpr int HK_MAX = 2;
struct JoystickInput
{
    int joystickID = 0;
    JoystickSelection joystickSelection;
    std::vector<SDL_JoystickID> joystickTopology;
    Uint32 joystickLastOpen = 0;
    SDL_Joystick* joystick = nullptr;
    SDL_GameController* controller = nullptr;
    bool hasRumble = false, hasAccelerometer = false, hasGyroscope = false, isRumbling = false;
    std::shared_ptr<SDL_mutex> joyMutex{SDL_CreateMutex(), SDL_DestroyMutex};
    std::array<int, 12> joyMapping;
    std::array<int, HK_MAX> hkJoyMapping;
    std::atomic<melonDS::u32> keyInputMask{0xFFF}, keyHotkeyMask{0};
    melonDS::u32 joyInputMask = 0xFFF, inputMask = 0xFFF, joyHotkeyMask = 0;
    melonDS::u32 hotkeyMask = 0, lastHotkeyMask = 0, hotkeyPress = 0, hotkeyRelease = 0;

    JoystickInput()
    {
        Require(joyMutex != nullptr, "Fixture SDL mutex creation failed");
        joyMapping.fill(-1);
        hkJoyMapping.fill(-1);
        joyMapping[0] = 0;
        joyMapping[4] = 0x101; // Hat 0, up.
        joyMapping[6] = 0x1FFFF; // Axis 0, positive; no button binding.
        hkJoyMapping[0] = 0;
    }
    ~JoystickInput() { closeJoystick(); }
    void setJoystick(int id);
    void setJoystickSelection(const JoystickSelection& selection);
    JoystickSelection getJoystickSelection();
    void openJoystick();
    void closeJoystick();
    bool joystickButtonDown(int val);
    void inputProcess();
    void inputRumbleStart(melonDS::u32 len_ms);
    void inputRumbleStop();
    float inputMotionQuery(melonDS::Platform::MotionQueryType type);
};

#define EmuInstance JoystickInput
#define SDL_JoystickOpen OpenJoystick
#define SDL_JoystickClose CloseJoystick
#define SDL_GameControllerOpen OpenController
#define SDL_GameControllerClose CloseController
#define SDL_GameControllerHasRumble HasRumble
#define SDL_GameControllerHasSensor HasSensor
#define SDL_GameControllerSetSensorEnabled EnableSensor
#define SDL_GameControllerRumble Rumble
#define SDL_GameControllerGetSensorData SensorData
#include "joystickSet.inc"
#include "joystickRestore.inc"
#include "joystickGetSelection.inc"
#include "joystickOpen.inc"
#include "joystickClose.inc"
#include "joystickButton.inc"
#include "joystickProcess.inc"
#include "joystickRumbleStart.inc"
#include "joystickRumbleStop.inc"
#include "joystickMotion.inc"
#undef SDL_GameControllerGetSensorData
#undef SDL_GameControllerRumble
#undef SDL_GameControllerSetSensorEnabled
#undef SDL_GameControllerHasSensor
#undef SDL_GameControllerHasRumble
#undef SDL_GameControllerClose
#undef SDL_GameControllerOpen
#undef SDL_JoystickClose
#undef SDL_JoystickOpen
#undef EmuInstance

struct VirtualDevice
{
    SDL_JoystickID id = -1;
    int rumbleStarts = 0;
    VirtualDevice(bool controller, bool rumble, const char* serial = "") { attach(controller, rumble, serial); }
    void attach(bool controller, bool rumble, const char* serial = "")
    {
        SDL_VirtualJoystickDesc desc{};
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = controller ? SDL_JOYSTICK_TYPE_GAMECONTROLLER : SDL_JOYSTICK_TYPE_FLIGHT_STICK;
        desc.naxes = 2;
        desc.nbuttons = 2;
        desc.nhats = 1;
        desc.button_mask = (1 << SDL_CONTROLLER_BUTTON_A) | (1 << SDL_CONTROLLER_BUTTON_B);
        desc.axis_mask = (1 << SDL_CONTROLLER_AXIS_LEFTX) | (1 << SDL_CONTROLLER_AXIS_LEFTY);
        desc.name = controller ? "FS09 virtual controller" : "FS09 virtual flight stick";
        desc.userdata = this;
        if (rumble)
            desc.Rumble = [](void* data, Uint16 low, Uint16 high) -> int {
                if (low || high) ++static_cast<VirtualDevice*>(data)->rumbleStarts;
                return 0;
            };
        const int device = SDL_JoystickAttachVirtualEx(&desc);
        Require(device >= 0, "Fixture virtual device attachment failed");
        id = SDL_JoystickGetDeviceInstanceID(device);
        if (*serial) deviceSerials[id] = serial;
        Require(id >= 0 && SDL_IsGameController(device) == (controller ? SDL_TRUE : SDL_FALSE),
                "Fixture did not create the requested controller/joystick type");
    }
    int index() const
    {
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            if (SDL_JoystickGetDeviceInstanceID(i) == id) return i;
        return -1;
    }
    void detach()
    {
        const int device = index();
        Require(device >= 0 && SDL_JoystickDetachVirtual(device) == 0, "Fixture virtual detach failed");
        deviceSerials.erase(id);
    }
    ~VirtualDevice()
    {
        const int device = index();
        if (device >= 0) SDL_JoystickDetachVirtual(device);
        deviceSerials.erase(id);
    }
};

static void NoCapabilities(const JoystickInput& input)
{
    Check(!input.hasRumble && !input.hasAccelerometer && !input.hasGyroscope && !input.isRumbling,
          "Previous device capabilities or rumble state survived the lifetime transition");
}
static void Closed(const JoystickInput& input)
{
    Check(!input.joystick && !input.controller, "Closing or failed opening retained a device handle");
    NoCapabilities(input);
    Check(joysticks.empty() && controllers.empty(), "Frontend still owns SDL handles after closing/detach/failure");
}
static void PressButton(JoystickInput& input, bool down)
{
    Require(input.joystick != nullptr, "Fixture has no joystick for button input");
    Require(SDL_JoystickSetVirtualButton(input.joystick, 0, down ? SDL_PRESSED : SDL_RELEASED) == 0,
            "Fixture virtual button update failed");
    input.inputProcess(); // Production calls the SDL_JoystickUpdate required by the virtual API.
    Check(bool(input.inputMask & 1) == !down, "Selected virtual device's button input was not sampled correctly");
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string scenario = argv[1];
    if (scenario != "controls" && scenario != "transition" && scenario != "capabilities" &&
        scenario != "detach" && scenario != "open-failure" && scenario != "close" &&
        scenario != "reorder" && scenario != "ambiguous" && scenario != "serial-reconnect" &&
        scenario != "duplicate-serial") return 2;
    SDL_SetMainReady();
    // Only this process's synthetic devices may be opened, polled or rumbled.
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "0");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_DIRECTINPUT_ENABLED, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    if (SDL_NumJoysticks() != 0)
    {
        std::fprintf(stderr, "SKIP: physical devices remain visible; no device will be opened\n");
        SDL_Quit();
        return 77;
    }
    try
    {
        const bool serialCase = scenario == "serial-reconnect" || scenario == "duplicate-serial";
        VirtualDevice pad(true, true, serialCase ? "generated-controller-A" : "");
        std::unique_ptr<VirtualDevice> other;
        JoystickInput input; // Destroy before device callbacks/userdata go away.
        sensorDevice = pad.id;
        input.setJoystick(pad.index());
        Require(input.joystick && input.controller && input.hasRumble &&
                input.hasAccelerometer && input.hasGyroscope, "Fixture initial capabilities were not available");
        input.inputRumbleStart(10000);
        Require(input.isRumbling && pad.rumbleStarts == 1, "Fixture rumble did not reach the virtual callback");

        if (scenario == "controls")
        {
            input.keyInputMask = 0xFFF & ~(1 << 1);
            input.keyHotkeyMask = 2;
            Require(SDL_JoystickSetVirtualHat(input.joystick, 0, SDL_HAT_UP) == 0 &&
                    SDL_JoystickSetVirtualAxis(input.joystick, 0, 24576) == 0, "Fixture hat/axis update failed");
            PressButton(input, true);
            Check(input.inputMask == (0xFFF & ~((1 << 0) | (1 << 1) | (1 << 4) | (1 << 6))) &&
                  input.hotkeyMask == 3 && input.hotkeyPress == 3, "Normal keyboard/joystick mapping or press edge changed");
            PressButton(input, false);
            Check(input.hotkeyMask == 2 && input.hotkeyRelease == 1 && input.hotkeyPress == 0,
                  "Normal hotkey release did not preserve the independent keyboard hotkey");
            input.inputRumbleStop();
            Check(!input.isRumbling, "Normal rumble stop did not release its state");
            using namespace melonDS::Platform;
            Check(input.inputMotionQuery(MotionAccelerationX) == 1.0f &&
                  input.inputMotionQuery(MotionAccelerationY) == -3.0f &&
                  input.inputMotionQuery(MotionAccelerationZ) == 2.0f &&
                  input.inputMotionQuery(MotionRotationX) == 1.0f &&
                  input.inputMotionQuery(MotionRotationY) == -3.0f &&
                  input.inputMotionQuery(MotionRotationZ) == 2.0f,
                  "Selected controller's supplied sensor samples did not follow the normal orientation path");
        }
        else if (scenario == "transition" || scenario == "capabilities")
        {
            other = std::make_unique<VirtualDevice>(scenario == "capabilities", false);
            input.setJoystick(other->index());
            Check(input.joystick && SDL_JoystickInstanceID(input.joystick) == other->id,
                  "Switching devices changed the requested selection");
            Check(bool(input.controller) == (scenario == "capabilities"), "Non-controller selection retained a stale controller");
            NoCapabilities(input);
            Check(SDL_JoystickFromInstanceID(pad.id) == nullptr, "Previous controller/joystick references were not released");
            PressButton(input, true);
            input.setJoystick(pad.index());
            input.inputRumbleStart(10000);
            Check(pad.rumbleStarts == 2, "A prior device's rumble latch blocked rumble after reopening");
            input.inputRumbleStop();
        }
        else if (scenario == "detach")
        {
            input.keyInputMask = 0xFFF & ~(1 << 1);
            input.keyHotkeyMask = 2;
            PressButton(input, true);
            pad.detach();
            input.inputProcess();
            Closed(input);
            Check(SDL_JoystickFromInstanceID(pad.id) == nullptr, "Detached SDL device references survived inputProcess");
            Check(input.inputMask == (0xFFF & ~(1 << 1)) && input.hotkeyMask == 2 && input.hotkeyRelease == 1,
                  "Detach left joystick input pressed or erased keyboard input");
            other = std::make_unique<VirtualDevice>(false, false);
            input.inputProcess();
            Check(!input.joystick,
                  "Detached selection silently retargeted a different device at the same index");
            NoCapabilities(input);
        }
        else if (scenario == "open-failure")
        {
            failJoystickOpen = true;
            input.setJoystick(pad.index());
            Closed(input);
            failJoystickOpen = false;
            input.setJoystick(pad.index());
            failControllerOpen = true;
            input.setJoystick(pad.index());
            Check(input.joystick && !input.controller, "Controller-open failure lost the usable joystick fallback");
            NoCapabilities(input);
            PressButton(input, true);
            failControllerOpen = false;
        }
        else if (scenario == "reorder")
        {
            other = std::make_unique<VirtualDevice>(true, false);
            input.setJoystick(other->index());
            const auto selected = input.getJoystickSelection();
            pad.detach();
            input.inputProcess();
            Check(input.joystickID == 0 && input.joystick && SDL_JoystickInstanceID(input.joystick) == other->id,
                  "Removing the earlier device changed the selected session identity");
            input.setJoystickSelection(selected);
            PressButton(input, true);
        }
        else if (scenario == "ambiguous" || serialCase)
        {
            const char* otherSerial = scenario == "duplicate-serial" ? "generated-controller-A" :
                                      serialCase ? "generated-controller-B" : "";
            other = std::make_unique<VirtualDevice>(true, false, otherSerial);
            JoystickInput second;
            second.setJoystick(other->index());
            // Observe both devices before removal, including duplicate serials.
            input.inputProcess();
            pad.detach();
            input.inputProcess();
            second.inputProcess();
            Check(!input.joystick && second.joystick && SDL_JoystickInstanceID(second.joystick) == other->id,
                  "Disconnect stole the controller assigned to another instance");
            PressButton(second, true);
            input.inputProcess();
            Check(input.inputMask == 0xFFF, "Unresolved instance sampled another instance's input");
            pad.attach(true, true, serialCase ? "generated-controller-A" : "");
            sensorDevice = pad.id;
            input.inputProcess();
            second.inputProcess();
            if (scenario == "serial-reconnect")
            {
                Check(input.joystick && SDL_JoystickInstanceID(input.joystick) == pad.id && input.hasRumble,
                      "Unique serial did not reacquire the intended controller after index reorder");
                auto restarted = input.getJoystickSelection();
                restarted.device.instance = -1;
                input.setJoystickSelection(restarted);
                Check(input.joystick && SDL_JoystickInstanceID(input.joystick) == pad.id,
                      "Saved serial identity selected the other same-model controller");
                input.inputRumbleStart(10000);
                Check(pad.rumbleStarts >= 2 && input.hasAccelerometer && input.hasGyroscope,
                      "Reattached serial controller lost its normal rumble/sensor capabilities");
            }
            else
            {
                Check(!input.joystick && input.getJoystickSelection().status == JoystickSelection::Status::Ambiguous,
                      "Indistinguishable reconnection was not kept explicitly ambiguous");
                input.setJoystick(pad.index());
                PressButton(input, true);
                // Sharing is allowed when explicitly selected, including devices
                // that another instance already has open.
                input.setJoystick(other->index());
                input.inputProcess();
                Check(!(input.inputMask & 1) && !(second.inputMask & 1),
                      "Explicit shared assignment was incorrectly forbidden");
            }
        }
        else // close, including a repeated close while rumbling.
        {
            input.closeJoystick();
            input.closeJoystick();
            Closed(input);
        }
        input.closeJoystick();
        Check(joysticks.empty() && controllers.empty(), "SDL handles remain after test teardown");
        Check(staleUses == 0, "Production used or closed an already-closed SDL handle");
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Fixture error (not a valid red result): %s\n", error.what());
        SDL_Quit();
        return 2;
    }
    SDL_Quit();
    std::printf("Frontend joystick %s: %d failures\n", scenario.c_str(), failures);
    return failures ? 1 : 0;
}
