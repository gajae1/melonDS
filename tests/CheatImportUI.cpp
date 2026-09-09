// SPDX-License-Identifier: GPL-3.0-or-later
// The actual Qt dialog receives generated database results. File decoding has
// its own ARDatabaseInput regression; no user database or window is opened here.
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <cstdio>
#include <memory>
#include "CheatImportDialog.h"

static bool emptyResult = true;
namespace melonDS
{
ARDatabaseDAT::ARDatabaseDAT(const std::string&) {}
ARDatabaseEntryList ARDatabaseDAT::GetEntriesByGameCode(u32 gamecode)
{
    ARDatabaseEntryList entries;
    if (emptyResult) return entries;
    entries.reserve(2);
    for (u32 checksum : {0x12345678u, 0xabcdef01u})
    {
        auto& entry = entries.emplace_back();
        entry.GameCode = gamecode;
        entry.Checksum = checksum;
        entry.Name = "Generated game";
        entry.RootCat = {};
        entry.RootCat.Children.emplace_back(ARCode{
            .Parent = &entry.RootCat, .Name = "Generated cheat", .Description = "",
            .Enabled = false, .Code = {0x02000100, 0x11223344}});
    }
    return entries;
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    melonDS::ARDatabaseDAT db("generated");
    for (bool empty : {true, false})
    {
        emptyResult = empty;
        auto dialog = std::make_unique<CheatImportDialog>(nullptr, &db, 0x454d4147, 0x12345678);
        dialog->setAttribute(Qt::WA_DeleteOnClose, false);
        auto* buttons = dialog->findChild<QDialogButtonBox*>("buttonBox");
        auto* entries = dialog->findChild<QComboBox*>("cbEntryList");
        if (!buttons || !entries) return 2;
        if (empty)
        {
            check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(), "Empty database result allowed destructive import");
            dialog->accept();
            check(dialog->result() != QDialog::Accepted, "Empty result was accepted programmatically");
        }
        else
        {
            check(entries->count() == 1 && buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                  "Matching valid database entry was unavailable");
            entries->setCurrentIndex(-1);
            check(!buttons->button(QDialogButtonBox::Ok)->isEnabled(), "Missing selection still allowed import");
            entries->setCurrentIndex(0);
            dialog->accept();
            check(dialog->result() == QDialog::Accepted, "Valid selected entry could not be accepted");
        }
    }
    std::printf("Cheat import selection: %d failures\n", failures);
    return failures ? 1 : 0;
}
