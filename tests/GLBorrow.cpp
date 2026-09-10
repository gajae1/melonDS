// SPDX-License-Identifier: GPL-3.0-or-later
// Exact production borrow case + dispatcher acknowledgement/wait tail and public
// borrowGL/returnGL definitions. Unrelated message cases are removed at build
// time; GL release is an observation stub. Synchronization uses real Qt objects.
#include <QCoreApplication>
#include <QMutex>
#include <QQueue>
#include <QSemaphore>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace
{
constexpr int GateDeadlineMs = 5000;

[[noreturn]] void FixtureFailure(const char* reason)
{
    std::fprintf(stderr, "GLBorrow fixture failure: %s\n", reason);
    std::fflush(nullptr);
    // Emergency bound only. Expected baseline RED is cleaned up and returns 1.
    // Never destroy a QThread/mutex while its worker is still waiting on it.
    std::_Exit(2);
}

void Await(QSemaphore& gate, const char* reason)
{
    if (!gate.tryAcquire(1, GateDeadlineMs)) FixtureFailure(reason);
}

struct Probe
{
    bool Early = false;
    QSemaphore ContinueAfterAck;
    QSemaphore Stage;
    std::atomic<unsigned> Releases{0};
    std::atomic<unsigned> Acknowledgements{0};
    std::atomic<unsigned> WaitCalls{0};
    std::atomic<bool> ReleasedBeforeAck{false};
    std::atomic<bool> UIReturned{false};
    std::atomic<bool> HandlerReturned{false};
};

struct ReleaseBoundary
{
    Probe* Observation;
    int releaseGL() { ++Observation->Releases; return -1; }
};

struct ObservedAcknowledgement
{
    Probe* Observation;
    QSemaphore Real;

    void release()
    {
        Observation->ReleasedBeforeAck = Observation->Releases == 1;
        ++Observation->Acknowledgements;
        Real.release();
        // Hold the worker AFTER the real completion acknowledgement, before
        // the unchanged handler reaches its mutex/condition-variable wait.
        if (Observation->Early)
            Await(Observation->ContinueAfterAck, "UI did not release the acknowledgement barrier");
    }
    void acquire() { Await(Real, "borrowGL completion was not acknowledged"); }
};

struct ObservedCondition
{
    Probe* Observation = nullptr;
    QWaitCondition Real;

    bool wait(QMutex* mutex)
    {
        ++Observation->WaitCalls;
        Observation->Stage.release();
        // The handler still holds this REAL QMutex. returnGL must acquire the
        // same mutex, so it cannot wake us between this notification and Qt's
        // atomic unlock-and-wait. This proves the normal case's ordering.
        const bool woke = Real.wait(mutex, static_cast<unsigned long>(GateDeadlineMs));
        if (!woke) FixtureFailure("condition wait exceeded cleanup bound");
        return woke;
    }
    void wakeAll() { Real.wakeAll(); }
};
}

// This small envelope isolates only queue publication/waiting and native GL
// release. The production handler's mutexes, acknowledgement placement, borrow
// predicate (when present), wait loop, and return method are not reimplemented.
class EmuThread final : public QThread
{
public:
    enum MessageType { msg_BorrowGL };
    struct Message { MessageType type; };

    explicit EmuThread(Probe& probe)
        : Observation(probe), Release{&probe}, emuInstance(&Release), msgSemaphore{&probe}
    {
        glBorrowCond.Observation = &probe;
    }

    bool borrowGL();
    void reportGLFailure(int) { FixtureFailure("unexpected release failure"); }
    int msgResult = 0;
    void returnGL();
    void handleMessages();

    // Generated verbatim from EmuThread.h, through the next signals: boundary.
    // Only the condition's type is substituted to observe real Qt waiting.
#define QWaitCondition ObservedCondition
#include "glBorrowState.inc"
#undef QWaitCondition

private:
    void sendMessage(MessageType type)
    {
        msgMutex.lock();
        msgQueue.enqueue({type});
        msgMutex.unlock();
        RequestReady.release();
    }
    void waitMessage() { msgSemaphore.acquire(); }
    void run() override
    {
        Await(RequestReady, "no borrow request reached the worker");
        handleMessages();
        Observation.HandlerReturned = true;
        Observation.Stage.release();
    }

    Probe& Observation;
    ReleaseBoundary Release;
    ReleaseBoundary* emuInstance;
    ObservedAcknowledgement msgSemaphore;
    QSemaphore RequestReady;
    QMutex msgMutex;
    QQueue<Message> msgQueue;
};

#include "glBorrowHandler.inc"
#include "glBorrowRequest.inc"
#include "glBorrowReturn.inc"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    if (mode != "early" && mode != "waiting") return 2;

    Probe probe;
    probe.Early = mode == "early";
    EmuThread worker(probe);
    worker.start();
    worker.borrowGL();

    bool passed = probe.ReleasedBeforeAck && probe.Acknowledgements == 1;
    bool cleanupReturn = false;
    if (probe.Early)
    {
        // The handler is held inside the acknowledgement hook: no wait exists.
        worker.returnGL();
        probe.UIReturned = true;
        probe.ContinueAfterAck.release();
        Await(probe.Stage, "handler neither returned nor entered its wait");

        // Observable liveness contract: an already completed return must not
        // leave the handler blocked waiting for another UI notification.
        if (!probe.HandlerReturned)
        {
            passed = false;
            std::fprintf(stderr, "Completed returnGL was lost: handler entered a wait afterwards\n");
            // Record RED BEFORE cleanup. The wait hook announces while holding
            // the mutex, so this second return cannot race ahead of Qt's wait.
            cleanupReturn = true;
            worker.returnGL();
        }
    }
    else
    {
        Await(probe.Stage, "normal handler did not reach its wait");
        if (probe.HandlerReturned || probe.WaitCalls == 0)
        {
            passed = false;
            std::fprintf(stderr, "Handler completed before the UI returned its GL ownership\n");
        }
        worker.returnGL();
        probe.UIReturned = true;
    }

    if (!worker.wait(GateDeadlineMs)) FixtureFailure("worker did not finish after return/cleanup");
    passed &= probe.HandlerReturned && probe.UIReturned;
    if (probe.Early) passed &= probe.WaitCalls == 0;
    else passed &= probe.WaitCalls != 0;

    std::printf("GLBorrow %s: released_before_ack=%d acknowledgements=%u waits=%u "
                "handler_returned=%d cleanup_return=%d result=%s\n",
                argv[1], static_cast<int>(probe.ReleasedBeforeAck.load()),
                probe.Acknowledgements.load(), probe.WaitCalls.load(),
                static_cast<int>(probe.HandlerReturned.load()), cleanupReturn,
                passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
