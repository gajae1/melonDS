// SPDX-License-Identifier: GPL-3.0-or-later
// Reuse the existing real core/save-manager frontend fixture and current-source
// extractors. Only resource selection changes to supply a synthetic DSi NAND.
#define main CartReplacementMain
#include "CartReplacement.cpp"
#undef main

std::optional<DSi_NAND::NANDImage> MakeDSiBootNAND();
void FailDSiBootHardwareRead(DSi_NAND::NANDImage& image, unsigned successfulReads);

struct BootSettings : FixtureSettings
{
    bool GetBool(const string& key) const { return key == "Emu.DirectBoot"; }
};

struct BootLoader : CartLoader
{
    BootSettings globalCfg;
    std::optional<DSi_NAND::NANDImage> incomingNAND;
    unsigned clearedBackups = 0;
    BootLoader()
    {
        globalCfg.mode = 1;
        nds->GetNDSCart()->GetHeader().UnitCode = 2;
        nds->Start();
    }
    unique_ptr<DSiBIOSImage> loadDSiARM7BIOS() { return make_unique<DSiBIOSImage>(); }
    unique_ptr<DSiBIOSImage> loadDSiARM9BIOS() { return make_unique<DSiBIOSImage>(); }
    std::optional<DSi_NAND::NANDImage> loadNAND(const DSiBIOSImage&) { return std::move(incomingNAND); }
    void ejectGBACart() { nds->EjectGBACart(); }
    void clearBackupState() { ++clearedBackups; }
    bool loadROMData(const QStringList& paths, unique_ptr<u8[]>& data, u32& length,
                     string& base, string& name) noexcept
    {
        if (!CartLoader::loadROMData(paths, data, length, base, name)) return false;
        auto* header = reinterpret_cast<NDSHeader*>(data.get());
        header->UnitCode = 2;
        header->DSiRegionMask = RegionFree;
        return true;
    }
    bool updateConsole(bool directBoot = false) noexcept;
    bool bootToMenu(QString& errorstr);
    bool reset(const AssetIdentity::Selection& dsAssets = {}, const AssetIdentity::Selection& gbaAssets = {});
    bool loadROM(QStringList filepath, bool reset, QString& errorstr, const AssetIdentity::Selection& assets = {}, const std::shared_ptr<ROMPreparation::Data>& prepared = {});
};

#define EmuInstance BootLoader
#include "cartUpdateConsole.inc"
#include "cartReset.inc"
#include "cartLoadROM.inc"
#include "cartBootToMenu.inc"
#undef EmuInstance

static bool Prepare(BootLoader& loader, const QString& directory, int failure)
{
    loader.baseROMDir = loader.incomingDir = directory.toStdString();
    loader.ndsSave = make_unique<SaveManager>((directory + "/current.sav").toStdString());
    Queue(*loader.ndsSave, QByteArray(8192, '\x6B'));
    loader.incomingNAND = MakeDSiBootNAND();
    {
        DSi_NAND::NANDMount mount(*loader.incomingNAND);
        DSi_NAND::DSiSerialData serial{};
        DSi_NAND::DSiHardwareInfoN hardware{};
        if (!mount.ImportFile("0:/sys/HWINFO_S.dat", serial.Bytes, sizeof(serial))) return false;
        if (failure != 1 && !mount.ImportFile("0:/sys/HWINFO_N.dat", hardware.data(),
                failure == 2 ? hardware.size()-1 : hardware.size())) return false;
    }
    if (failure >= 3)
        FailDSiBootHardwareRead(*loader.incomingNAND, failure == 4 ? 1 : 0);
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (std::string_view(argv[1]).starts_with("legacy-"))
    {
        argv[1] += 7;
        return CartReplacementMain(argc, argv);
    }
    QCoreApplication app(argc, argv);
    const string mode = argv[1];
    if (mode == "firmware")
    {
        QTemporaryDir directory(QDir::currentPath() + "/dsi-boot-menu-XXXXXX");
        if (!directory.isValid()) return 2;
        BootLoader loader;
        if (!Prepare(loader, directory.path(), 1)) return 2;
        QString error;
        // Explicit firmware preparation must ignore the configured direct-boot
        // preference. This checks dispatch only, without executing firmware.
        const bool accepted = loader.bootToMenu(error);
        const bool pass = accepted && error.isEmpty() &&
            loader.nds->ARM9Read16(0x02FFFC40) == 0;
        std::printf("frontend explicit firmware preparation accepted=%d: %s\n",
            accepted, pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    const bool retain = mode == "retain";
    const bool late = mode == "late";
    if (!retain && !late && mode != "success") return 2;
    for (bool resetting : {false, true})
    for (int failure = retain ? 1 : late ? 4 : 0; failure <= (retain ? 3 : late ? 4 : 0); ++failure)
    {
        QTemporaryDir directory(QDir::currentPath() + "/dsi-boot-front-XXXXXX");
        if (!directory.isValid()) return 2;
        BootLoader loader;
        if (!Prepare(loader, directory.path(), failure)) return 2;
        auto* previousCore = loader.nds;
        auto* previousCart = loader.nds->GetNDSCart();
        auto* previousSave = loader.ndsSave.get();
        const auto previousName = loader.baseROMName;
        const auto previousPath = loader.ndsSave->GetPath();
        QString error;
        const bool accepted = resetting ? loader.reset() : loader.loadROM({"incoming.nds"}, true, error);
        bool pass = accepted == (failure == 0) &&
            ReadSaveFile(previousPath) == QByteArray(8192, '\x6B');
        if (retain)
            pass &= loader.nds == previousCore && loader.nds->GetNDSCart() == previousCart &&
                loader.ndsSave.get() == previousSave && loader.ndsSave->GetPath() == previousPath &&
                loader.baseROMName == previousName && loader.nds->IsRunning() &&
                loader.nds->ARM9Read32(0x02001000) == 0xDEADBEEF && loader.clearedBackups == 0;
        else if (late)
        {
            pass &= !loader.nds->IsRunning() && loader.nds->ARM9Read16(0x02FFFC40) == 0 &&
                loader.clearedBackups == 1;
            // Reset retains its identity; an already-committed load must keep
            // the new cart and matching save identity together while stopped.
            pass &= loader.baseROMName == (resetting ? "current.nds" : "incoming.nds") &&
                loader.ndsSave->GetPath() == (directory.path() +
                    (resetting ? "/current.sav" : "/incoming.sav")).toStdString();
        }
        else
            pass &= loader.nds->ConsoleType == 1 && loader.nds->ARM9Read16(0x02FFFC40) == 1;
        if (!resetting && failure) pass &= !error.isEmpty();
        std::printf("frontend %s failure=%d accepted=%d: %s\n",
            resetting ? "reset" : "load", failure, accepted, pass ? "PASS" : "FAIL");
        if (!pass) return 1;
    }
    return 0;
}
