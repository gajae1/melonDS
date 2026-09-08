// SPDX-License-Identifier: GPL-3.0-or-later
#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include "Config.h"
#include "NDS.h"

using namespace melonDS;

// Exercise the real frontend override with isolated configuration and no files.
struct ProfileState
{
    Config::Table localCfg = Config::GetLocalTable(0);
    int instanceID = 0;
    bool parseMacAddress(void* mac);
    void customizeFirmware(Firmware& firmware, bool overridesettings) noexcept;
};
#define EmuInstance ProfileState
#include "parseMacAddress.inc"
#include "customizeFirmware.inc"
#undef EmuInstance

static QString configDirectory;
namespace melonDS::Platform
{
std::string GetLocalFilePath(const std::string& path)
{
    return (configDirectory + '/' + QString::fromStdString(path)).toStdString();
}
bool CheckFileWritable(const std::string&) { return true; }
bool FileExists(const std::string& path) { return QFileInfo::exists(QString::fromStdString(path)); }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    configDirectory = directory.path();
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    ProfileState profile;
    auto cfg = profile.localCfg.GetTable("Firmware");
    cfg.SetQString("Username", "Profile");

    // The on-console counter wraps at 127. Invalid newer data must not win.
    Firmware firmware(0);
    auto& userData = firmware.GetUserData();
    userData[0].UpdateCounter = 127;
    userData[1].UpdateCounter = 0;
    userData[0].Settings = Firmware::Language::Japanese;
    userData[1].Settings = Firmware::Language::German;
    firmware.UpdateChecksums();
    check(&firmware.GetEffectiveUserData() == &userData[1] &&
          &std::as_const(firmware).GetEffectiveUserData() == &userData[1],
          "Wrapped firmware counter selected the old profile");

    userData[0].UpdateCounter = 4;
    userData[1].UpdateCounter = 5;
    userData[1].Checksum ^= 1;
    check(&firmware.GetEffectiveUserData() == &userData[0], "Invalid newer profile was selected");
    cfg.SetInt("Language", Firmware::Language::French);
    profile.customizeFirmware(firmware, true);
    check((firmware.GetEffectiveUserData().Settings & 7) == Firmware::Language::French,
          "Overriding the valid profile revived stale invalid settings");
    check(!userData[1].ChecksumValid(), "Override unexpectedly repaired the invalid backup profile");

    NDSArgs args;
    args.JIT = std::nullopt;
    args.Firmware = std::move(firmware);
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    nds->SPI.GetFirmwareMem()->SetupDirectBoot();
    check((nds->ARM9Read16(0x027FFCE4) & 7) == Firmware::Language::French,
          "Reset/direct boot lost the overridden language");

    // Chinese and Korean use the extended language byte; legacy games see
    // English, just as they do with the corresponding console firmware.
    for (int console : {0, 1})
    {
        for (int language : {6, 7})
        {
            Firmware generated(console);
            cfg.SetInt("Language", language);
            profile.customizeFirmware(generated, true);
            const auto& data = generated.GetEffectiveUserData();
            check((data.Settings & 7) == 1 && data.ExtendedSettings.Unknown0 == 1 &&
                  static_cast<int>(data.ExtendedSettings.ExtendedLanguage) == language &&
                  (data.ExtendedSettings.SupportedLanguageMask & (1 << language)) &&
                  data.ChecksumValid(), "Extended profile language is missing or invalid");
            const u32 address = (generated.GetHeader().UserSettingsOffset << 3) +
                (&data == &generated.GetUserData()[1] ? 0x100 : 0) + 0x75;
            auto* flash = nds->SPI.GetFirmwareMem();
            flash->SetFirmware(std::move(generated));
            flash->Reset();
            flash->Write(0x03); // Read the profile through the real SPI device.
            flash->Write(address >> 16);
            flash->Write(address >> 8);
            flash->Write(address);
            flash->Write(0);
            check(flash->Read() == language, "SPI flash did not return the requested extended language");
            flash->Release();
        }
    }

    // Regional firmware can have an extended record without the documented
    // header bit. Preserve the original console identity and language mask.
    Firmware regional(0);
    regional.GetHeader().ConsoleType = 0x35;
    for (auto& data : regional.GetUserData())
    {
        data.ExtendedSettings.Unknown0 = 1;
        data.ExtendedSettings.SupportedLanguageMask = 0xAF;
        data.UpdateChecksum();
    }
    profile.customizeFirmware(regional, true);
    check(regional.GetHeader().ConsoleType == 0x35 &&
          regional.GetEffectiveUserData().ExtendedSettings.SupportedLanguageMask == 0xAF,
          "Language override changed the regional firmware identity");
    const Firmware unchanged = regional;
    profile.customizeFirmware(regional, false);
    check(std::memcmp(regional.Buffer(), unchanged.Buffer(), regional.Length()) == 0,
          "Disabled profile override modified the firmware");
    std::printf("Firmware profile, counter wrap and direct boot: %d failures\n", failures);
    return failures ? 1 : 0;
}
