// SPDX-License-Identifier: GPL-3.0-or-later
// Real preparation, QFile/libarchive/zstd, Qt worker/event loop and extracted
// Window handlers. Delays replace host read/decoder boundaries only. The UI's
// emulation dispatch is recorded; CartReplacement covers the real cart/save swap.
#include <QApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <zstd.h>
#include "ROMPreparation.h"
#include "ArchiveUtil.h"
#include "Platform.h"
using namespace melonDS;
using std::unique_ptr;
using std::make_unique;
static std::atomic<int> rawReads{0}, archiveReads{0}, headers{0}, decoderCalls{0};
static std::atomic<bool> delayReads{true}, delayHeaders{false}, delayDecoder{false};
static bool blockRead = false;
static std::atomic<size_t> largestRead{0}, largestExtract{0};
static void Require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
namespace melonDS::Platform
{
FileHandle* OpenPreparedFile(const std::string& path, FileMode)
{
    auto file = make_unique<QFile>(QString::fromStdString(path));
    return file->open(QIODevice::ReadOnly) ? reinterpret_cast<FileHandle*>(file.release()) : nullptr;
}
bool ClosePreparedFile(FileHandle* file) { delete reinterpret_cast<QFile*>(file); return true; }
u64 PreparedFileLength(FileHandle* file) { return reinterpret_cast<QFile*>(file)->size(); }
u64 ReadPreparedFile(void* data, u64 size, u64 count, FileHandle* file)
{
    ++rawReads;
    if (blockRead && rawReads.load() == 1) QThread::msleep(250);
    largestRead.store(std::max(largestRead.load(), size_t(size * count)));
    if (delayReads) QThread::msleep(10);
    const auto read = reinterpret_cast<QFile*>(file)->read(static_cast<char*>(data), size * count);
    return read < 0 ? u64(-1) : read / size;
}
}
static la_ssize_t DelayedExtract(archive* reader, void* data, size_t count)
{
    ++archiveReads;
    largestExtract.store(std::max(largestExtract.load(), count));
    if (delayReads) QThread::msleep(10);
    return archive_read_data(reader, data, count);
}
static int DelayedHeader(archive* reader, archive_entry** entry)
{
    ++headers;
    if (delayHeaders) QThread::msleep(10);
    return archive_read_next_header(reader, entry);
}
static size_t DelayedDecode(ZSTD_DStream* stream, ZSTD_outBuffer* output, ZSTD_inBuffer* input)
{
    ++decoderCalls;
    if (delayDecoder) QThread::msleep(10);
    return ZSTD_decompressStream(stream, output, input);
}
#define OpenFile OpenPreparedFile
#define CloseFile ClosePreparedFile
#define FileLength PreparedFileLength
#define FileRead ReadPreparedFile
#define archive_read_data DelayedExtract
#define archive_read_next_header DelayedHeader
#define ZSTD_decompressStream DelayedDecode
#include "ArchiveUtil.cpp"
#include "ROMPreparation.cpp"
#undef ZSTD_decompressStream
#undef archive_read_next_header
#undef archive_read_data
#undef FileRead
#undef FileLength
#undef CloseFile
#undef OpenFile

struct FixtureConfig
{
    int writes = 0;
    QString folder;
    void SetQString(const char*, const QString& value) { ++writes; folder = value; }
};
struct FixtureThread
{
    int applies = 0;
    bool gba = false, boot = false;
    QStringList source;
    QByteArray bytes;
    bool reject = false;
    std::function<void(const std::shared_ptr<ROMPreparation::Data>&)> beforeApply;
    int bootROM(const QStringList& path, QString& error, const std::shared_ptr<ROMPreparation::Data>& data)
    { boot = true; return insertCart(path, false, error, data); }
    int insertCart(const QStringList& path, bool isGBA, QString& error, const std::shared_ptr<ROMPreparation::Data>& data)
    {
        if (beforeApply) beforeApply(data);
        if (data->Stop.stop_requested()) { error.clear(); return 0; }
        if (reject) { error = "Generated apply failure"; return 0; }
        Require(!data->Stop.stop_requested(), "Cancelled result reached apply");
        Require(QThread::currentThread() == qApp->thread(), "Apply ran off the UI thread");
        ++applies; gba = isGBA; source = path;
        bytes = QByteArray(reinterpret_cast<const char*>(data->Bytes.get()), data->Length);
        data->Bytes.reset();
        return 1;
    }
};
class MainWindow;
struct FixtureInstance
{
    MainWindow* window = nullptr;
    void doOnAllWindows(const std::function<void(MainWindow*)>& fn) { fn(window); }
};
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    enum class ROMAction { BootDS, InsertDS, InsertGBA, Drop };
    FixtureInstance instance;
    FixtureInstance* emuInstance = &instance;
    FixtureThread thread;
    FixtureThread* emuThread = &thread;
    FixtureConfig globalCfg;
    QList<QString> recentFileList;
    int recentWrites = 0, cartUpdates = 0, firmwareBoots = 0;
    bool closeInProgress = false, closed = false;
    ROMPreparation::Controller romPreparation{this};
    QPointer<QProgressDialog> romProgress;
    QPointer<QInputDialog> romMemberDialog;
    QPointer<MainWindow> romCloseWaiter;
    ROMAction romAction = ROMAction::BootDS;
    bool romRememberFolder = false, romRememberRecent = false;
    bool romApplying = false, romClosePending = false;
    QStringList reselectedROM;
    ROMAction reselectedAction = ROMAction::BootDS;
    bool reselectedRememberFolder = false;
    QStringList nextPreloadROM;
    bool bootAfterPreload = false;
    MainWindow()
    {
        instance.window = this;
        connect(&romPreparation, &ROMPreparation::Controller::ready, this, &MainWindow::finishROMPreparation);
        connect(&romPreparation, &ROMPreparation::Controller::idle, this, [this] {
            if (romCloseWaiter && !romApplying)
            {
                auto waiter = romCloseWaiter;
                romCloseWaiter.clear();
                waiter->close();
            }
        });
    }
    ~MainWindow() override { cancelROMPreparation(); }
    void cancelROMPreparation();
    void cancelROMPreparations();
    bool deferROMClose();
    void showROMProgress();
    void startROMPreparation(QStringList files, ROMAction action, bool rememberFolder = false);
    void finishROMPreparation(const ROMPreparation::Result& result);
    void pickFileFromArchive(const ROMPreparation::Result& result);
    QStringList splitArchivePath(const QString& filename, bool useMemberSyntax);
    bool preloadROMs(QStringList file, QStringList gbafile, bool boot);
    bool verifySetup() { return true; }
    void onBootFirmware() { ++firmwareBoots; }
    void updateRecentFilesMenu() { ++recentWrites; }
    void updateCartInserted(bool) { ++cartUpdates; }
    void closeEvent(QCloseEvent* event) override
    {
        if (deferROMClose()) { event->ignore(); return; }
        closed = true;
        QMainWindow::closeEvent(event);
    }
};
#include "romWindowCancel.inc"
#include "romWindowCancelAll.inc"
#include "romWindowClose.inc"
#include "romWindowProgress.inc"
#include "romWindowStart.inc"
#include "romWindowFinish.inc"
#include "romWindowPick.inc"
#include "romWindowSplit.inc"
#include "romWindowPreload.inc"

static void WriteFixture(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    Require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "Fixture write failed");
}
static void ArchiveFile(const QString& path, const QByteArray& bytes, int members)
{
    auto* writer = archive_write_new();
    archive_write_set_format_zip(writer);
    Require(archive_write_open_filename(writer, path.toUtf8().constData()) == ARCHIVE_OK, "Fixture archive open");
    for (int i = 0; i < members; ++i)
    {
        auto* entry = archive_entry_new();
        const auto name = QString("member%1.nds").arg(i).toUtf8();
        archive_entry_set_pathname(entry, name.constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, bytes.size());
        Require(archive_write_header(writer, entry) == ARCHIVE_OK, "Fixture archive header");
        Require(archive_write_data(writer, bytes.constData(), bytes.size()) == bytes.size(), "Fixture archive body");
        archive_entry_free(entry);
    }
    Require(archive_write_free(writer) == ARCHIVE_OK, "Fixture archive close");
}
int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc != 2) return 2;
    const QString mode = argv[1];
    try
    {
        // Time the controlled I/O after warming Qt's font/theme/dialog caches.
        // Cold native widget construction is independent of the ROM reader and
        // can itself exceed 250 ms on the Windows offscreen platform.
        {
            QProgressDialog progress("Preparing ROM…", "Cancel", 0, 0);
            QInputDialog member; member.setComboBoxItems({"member0.nds", "member1.nds"});
            QMessageBox conflict(QMessageBox::Question, "Choose game files", "Generated save conflict",
                                 QMessageBox::Ok | QMessageBox::Cancel);
            progress.show(); member.show(); conflict.show();
            QEventLoop ready;
            QTimer::singleShot(0, &ready, &QEventLoop::quit);
            ready.exec();
        }
        QTemporaryDir dir;
        const auto path = dir.filePath("generated.nds"), zip = dir.filePath("generated.zip");
        const auto second = dir.filePath("second.gba");
        const QByteArray payload(8 * 1024 * 1024, 'x');
        WriteFixture(path, payload); WriteFixture(second, "second generated GBA");
        if (mode.contains("extract") || mode.contains("member")) ArchiveFile(zip, payload, 2);
        if (mode == "cancel-list") ArchiveFile(zip, "small", 100);
        const auto compressedPath = dir.filePath("generated.nds.zst");
        if (mode == "cancel-decode" || mode == "success-zstd")
        {
            QByteArray compressed(ZSTD_compressBound(payload.size()), '\0');
            const auto count = ZSTD_compress(compressed.data(), compressed.size(), payload.constData(), payload.size(), 1);
            Require(!ZSTD_isError(count), "Fixture compression"); compressed.resize(count);
            WriteFixture(compressedPath, compressed);
        }
        if (mode == "close-destruction")
        {
            QPointer<MainWindow> owner = new MainWindow;
            owner->setAttribute(Qt::WA_DeleteOnClose);
            owner->show();
            int attempts = 0;
            owner->thread.beforeApply = [&](const auto&) { ++attempts; };
            QEventLoop loop;
            QObject::connect(owner, &QObject::destroyed, &loop, &QEventLoop::quit);
            QTimer close; close.setInterval(2);
            bool deferred = false;
            QObject::connect(&close, &QTimer::timeout, &loop, [&] {
                if (owner && rawReads >= 2 && !deferred)
                {
                    deferred = !owner->close() && owner->romClosePending;
                    close.stop();
                }
            });
            QTimer::singleShot(3000, &loop, &QEventLoop::quit);
            owner->startROMPreparation({path}, MainWindow::ROMAction::BootDS, true);
            close.start(); loop.exec();
            const bool destroyed = !owner;
            if (owner) delete owner;
            Require(deferred && destroyed && attempts == 0, "Window/worker destruction allowed a late apply or lost close");
            printf("{\"case\":\"close-destruction\",\"status\":\"PASS\",\"window_destroyed\":true,\"applies\":0}\n");
            return 0;
        }
        MainWindow window;
        window.show();
        window.thread.reject = mode == "apply-failure";
        blockRead = mode == "os-blocked-close";
        if (mode == "late-completion") delayReads = false;
        bool modalVisited = false;
        if (mode.startsWith("modal-"))
        {
            delayReads = false;
            window.thread.beforeApply = [&](const std::shared_ptr<ROMPreparation::Data>& data) {
                if (modalVisited) return;
                modalVisited = true;
                QElapsedTimer modalElapsed; modalElapsed.start();
                QMessageBox conflict(QMessageBox::Question, "Choose game files", "Generated save conflict",
                                     QMessageBox::Ok | QMessageBox::Cancel, &window);
                const auto constructedAt = modalElapsed.elapsed();
                std::stop_callback onCancel(data->Stop, [&] {
                    QMetaObject::invokeMethod(&conflict, &QDialog::reject, Qt::QueuedConnection);
                });
                QTimer::singleShot(0, &conflict, [&] {
                    Require(window.romApplying && window.thread.applies == 0, "Modal test missed the apply boundary");
                    if (mode == "modal-close")
                    {
                        Require(!window.close() && window.romClosePending, "Modal close failed to defer");
                    }
                    else window.startROMPreparation({second}, MainWindow::ROMAction::Drop, true);
                });
                conflict.exec();
                fprintf(stderr, "modal UI: construct=%lld total=%lld ms (ROM already read)\n", (long long)constructedAt, (long long)modalElapsed.elapsed());
                Require(data->Stop.stop_requested(), "Modal close/reselection did not cancel original result");
            };
        }
        delayHeaders = mode == "cancel-list";
        delayDecoder = mode == "cancel-decode";
        if (mode == "cancel-decode" || mode == "success-zstd") delayReads = false;
        QStringList source{path};
        if (mode.contains("extract")) source = {zip, "member0.nds"};
        if (mode == "cancel-list" || mode.contains("member")) source = {zip};
        if (mode == "read-failure") source = {dir.filePath("missing.nds")};
        if (mode == "cancel-decode" || mode == "success-zstd") source = {compressedPath};
        QElapsedTimer elapsed; elapsed.start();
        QEventLoop loop;
        QTimer ticker; ticker.setInterval(2);
        qint64 lastTick = 0, maxGap = 0, cancelledAt = -1;
        bool triggered = false;
        int messages = 0;
        QObject::connect(&ticker, &QTimer::timeout, &loop, [&] {
            const auto now = elapsed.elapsed(); maxGap = std::max(maxGap, now - lastTick); lastTick = now;
            for (auto* widget : QApplication::topLevelWidgets())
                if (auto* message = qobject_cast<QMessageBox*>(widget); message && message->isVisible() && !mode.startsWith("modal-"))
                { ++messages; message->accept(); }
            const bool reading = mode == "os-blocked-close" ? rawReads.load() >= 1 : mode == "cancel-list" ? headers.load() >= 2 :
                mode == "cancel-decode" ? decoderCalls.load() >= 2 :
                mode.contains("extract") ? archiveReads.load() >= 2 : rawReads.load() >= 2;
            if (!triggered && ((mode.contains("member") && window.romMemberDialog) || reading))
            {
                if (mode.startsWith("cancel"))
                {
                    Require(window.romProgress || window.romMemberDialog, "No cancellable preparation UI");
                    // Use the actual progress Cancel button or member rejection.
                    if (window.romMemberDialog) window.romMemberDialog->reject();
                    else window.romProgress->findChild<QPushButton*>()->click();
                    triggered = true; cancelledAt = now;
                }
                else if (mode == "reselect" || mode == "reselect-member")
                {
                    window.startROMPreparation({second}, MainWindow::ROMAction::Drop, true);
                    triggered = true;
                }
                else if (mode == "close" || mode == "os-blocked-close")
                {
                    Require(!window.close(), "Close did not defer an active worker");
                    Require(window.romClosePending && !window.closed, "Close lost lifetime guard");
                    triggered = true; cancelledAt = now;
                }
                else if (mode == "success-member")
                {
                    window.romMemberDialog->setTextValue("member1.nds");
                    window.romMemberDialog->accept(); triggered = true;
                }
            }
            if (!window.romPreparation.busy() && !window.romMemberDialog &&
                (window.thread.applies || triggered || messages || window.closed)) loop.quit();
        });
        QTimer::singleShot(8000, &loop, &QEventLoop::quit);
        ticker.start();
        window.startROMPreparation(source, MainWindow::ROMAction::BootDS, true);
        const auto startUIMs = elapsed.elapsed();
        if (mode == "late-completion")
        {
            // Deliberately hold UI delivery until the actual worker is finished,
            // then reselect before its already-queued finished event is delivered.
            auto* worker = window.romPreparation.findChild<QThread*>();
            Require(worker && worker->wait(2000), "Could not queue a completed old result");
            Require(window.thread.applies == 0, "Worker applied while UI delivery was held");
            window.startROMPreparation({second}, MainWindow::ROMAction::Drop, true);
            elapsed.restart();
        }
        loop.exec();
        fprintf(stderr, "timing %s: elapsed=%lld max-gap=%lld cancel-at=%lld raw=%d extract=%d applied=%d\n", argv[1],
                (long long)elapsed.elapsed(), (long long)maxGap, (long long)cancelledAt, rawReads.load(), archiveReads.load(), window.thread.applies);
        fprintf(stderr, "initial progress UI: %lld ms\n", (long long)startUIMs);
        Require(elapsed.elapsed() < 8000, "Preparation timed out");
        Require(maxGap < 250, "Preparation blocked UI event delivery");
        Require(largestRead <= 65536 && largestExtract <= 65536, "Unbounded read/extract call");
        if (mode.startsWith("cancel") || mode == "close" || mode == "os-blocked-close" || mode == "modal-close" || mode.endsWith("failure"))
        {
            Require(window.thread.applies == 0 && window.cartUpdates == 0, "Cancellation/failure applied a cart");
            Require(window.globalCfg.writes == 0 && window.recentWrites == 0 && window.recentFileList.empty(), "Cancellation/failure changed configuration or recent files");
            Require(!window.romPreparation.busy(), "Cancelled worker was not joined");
            if (mode == "close" || mode == "os-blocked-close" || mode == "modal-close") Require(window.closed, "Deferred close did not finish");
            if (mode == "os-blocked-close") Require(elapsed.elapsed() - cancelledAt >= 150, "Blocked read limitation was not exercised");
            if (mode.startsWith("cancel")) Require(triggered && messages == 0, "Cancellation became an error");
        }
        else
        {
            Require(window.thread.applies == 1, "Stale or missing completion");
            if (mode.startsWith("reselect") || mode == "modal-reselect" || mode == "late-completion")
            {
                Require(window.thread.source == QStringList{second} && window.thread.gba, "Stale source/member/console applied");
                Require(window.thread.bytes == "second generated GBA" && window.recentWrites == 0, "Reselection lost bytes or wrote DS recent list");
            }
            else
            {
                Require(window.thread.bytes == payload && window.recentWrites == 1, "Successful read lost bytes/recent update");
                if (mode == "success-member") Require(window.thread.source == QStringList{zip, "member1.nds"}, "Member identity lost");
            }
            Require(window.globalCfg.writes == 1, "Folder was not committed exactly once after success");
        }
        if (mode.startsWith("modal-")) Require(modalVisited, "Modal gate was not exercised");
        printf("{\"case\":\"%s\",\"status\":\"PASS\",\"elapsed_ms\":%lld,\"max_ui_tick_gap_ms\":%lld,\"cancel_to_idle_ms\":%lld,\"raw_reads\":%d,\"extract_reads\":%d,\"applies\":%d}\n",
            argv[1], (long long)elapsed.elapsed(), (long long)maxGap, cancelledAt < 0 ? -1LL : (long long)(elapsed.elapsed() - cancelledAt), rawReads.load(), archiveReads.load(), window.thread.applies);
        return 0;
    }
    catch (const std::exception& error) { fprintf(stderr, "%s: %s\n", argv[1], error.what()); return 1; }
}
#include "ROMPreparationUI.moc"
