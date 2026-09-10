// SPDX-License-Identifier: GPL-3.0-or-later
// Production completion slot + real Qt widgets. NAND outcomes and title icon
// rendering are stand-ins here; DSiTitle exercises the actual NAND separately.
#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include "DSi_NAND.h"
#include "ui_TitleManagerDialog.h"
using namespace melonDS;
using Result = DSi_NAND::TitleImportResult;
static void Require(bool ok, const char* message)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

class TitleManagerDialog : public QDialog
{
public:
    Ui::TitleManagerDialog widgets;
    Ui::TitleManagerDialog* ui = &widgets;
    bool validNand = true;
    bool* nand = &validNand;
    struct Mount
    {
        Result result;
        unsigned imports = 0, deletes = 0;
        std::string recovery = "0:/_install/generated";
        bool ImportTitle(const char*, const DSi_TMD::TitleMetadata&, bool)
        { ++imports; return result == Result::Success || result == Result::InstalledCleanupPending; }
        void DeleteTitle(u32, u32) { ++deletes; }
        Result GetTitleImportResult() const { return result; }
        const std::string& GetTitleImportRecoveryPath() const { return recovery; }
    } nandmount;
    QString importAppPath = "generated.app";
    DSi_TMD::TitleMetadata importTmdData{};
    bool importReadOnly = true;
    explicit TitleManagerDialog(Result result) : nandmount{result}
    {
        ui->setupUi(this);
        const u8 id[] = {0,3,0,4,0x12,0x34,0x56,0x78};
        std::memcpy(importTmdData.TitleId, id, sizeof(id));
        createTitleItem(0x00030004,0x12345678);
        ui->lstTitleList->item(0)->setText("previous title");
    }
    void createTitleItem(u32 category, u32 title)
    {
        auto* row = new QListWidgetItem("installed title",ui->lstTitleList);
        row->setData(Qt::UserRole,QVariant::fromValue(qulonglong((u64(category)<<32)|title)));
    }
    void onImportTitleFinished(int res);
};
#include "CompleteImport.inc"

int main(int argc, char** argv)
{
    QApplication app(argc,argv);
    for (auto outcome : {Result::Success,Result::InvalidInput,Result::Failed,Result::CleanupPending,
                        Result::RollbackFailed,Result::RecoveryRequired,Result::InstalledCleanupPending})
    {
        TitleManagerDialog dialog(outcome);
        dialog.onImportTitleFinished(QDialog::Rejected);
        Require(dialog.nandmount.imports == 0,"cancel did not import");
        bool observed = false;
        if (outcome != Result::Success)
        {
            QTimer::singleShot(0,[&] {
                auto* message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                Require(message != nullptr,"result dialog exists"); observed = true;
                Require(message->icon() == (outcome == Result::InstalledCleanupPending ? QMessageBox::Warning : QMessageBox::Critical),"installed warning versus failure");
                Require(!message->text().isEmpty() && message->detailedText().contains("0:/_install/generated"),"recovery details available");
                message->accept();
            });
        }
        dialog.onImportTitleFinished(QDialog::Accepted);
        Require(dialog.nandmount.imports == 1 && dialog.nandmount.deletes == 0,"completion never deletes title");
        Require(observed == (outcome != Result::Success),"only clean success has no error/warning");
        Require(dialog.ui->lstTitleList->count() == 1,"same-ID import has exactly one row");
        bool installed = outcome == Result::Success || outcome == Result::InstalledCleanupPending;
        Require(dialog.ui->lstTitleList->item(0)->text() == (installed ? "installed title" : "previous title"),"only committed result refreshes row");
        if (outcome == Result::RollbackFailed || outcome == Result::RecoveryRequired)
            Require(!dialog.ui->btnImportTitle->isEnabled() && !dialog.ui->btnDeleteTitle->isEnabled() &&
                !dialog.ui->btnImportTitleData->isEnabled() && !dialog.ui->lstTitleList->isEnabled(),"recovery blocks unsafe further changes");
    }
    std::puts("PASS: actual completion slot/Qt rows, cancel, all seven outcomes and recovery controls");
}
