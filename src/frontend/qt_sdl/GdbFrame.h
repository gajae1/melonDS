// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>
#include "debug/GdbStub.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <cfenv>
#include <mutex>
#include "libco/libco.h"
#endif

// Preserve the interpreter's C++ stack while its owning emulation thread
// services UI messages. Only that thread may run, cancel, or destroy a frame.
class GdbFrame
{
    struct Cancelled {};
    inline static thread_local GdbFrame* Current = nullptr;
    void* Root = nullptr;
    void* Child = nullptr;
#ifndef _WIN32
    std::fenv_t RootEnv{}, ChildEnv{};
    inline static std::mutex CreationMutex;
#endif
#ifdef _WIN32
    bool Converted = false;
#endif
    bool Done = true;
    bool Suspended = false;
    bool Cancelling = false;
    std::function<unsigned()> Task;
    std::optional<unsigned> Result;
    std::exception_ptr Failure;

    void ToRoot()
    {
#ifdef _WIN32
        SwitchToFiber(Root);
#else
        std::fegetenv(&ChildEnv);
        std::fesetenv(&RootEnv);
        co_switch(Root);
#endif
    }

    void ToChild()
    {
#ifdef _WIN32
        SwitchToFiber(Child);
#else
        std::fegetenv(&RootEnv);
        std::fesetenv(&ChildEnv);
        co_switch(Child);
#endif
    }

    static void Suspend()
    {
        auto& frame = *Current;
        frame.Suspended = true;
        frame.ToRoot();
        frame.Suspended = false;
        if (frame.Cancelling) throw Cancelled{};
    }

    static void RunEntry(GdbFrame& frame)
    {
        for (;;)
        {
            try
            {
                struct RestoreYield
                {
                    void (*Previous)();
                    ~RestoreYield() { Gdb::HostIdle = Previous; }
                } restore{Gdb::HostIdle};
                Gdb::HostIdle = Suspend;
                frame.Result = frame.Task();
            }
            catch (const Cancelled&) {}
            catch (...) { frame.Failure = std::current_exception(); }
            frame.Done = true;
            frame.Suspended = false;
            frame.ToRoot();
        }
    }

#ifdef _WIN32
    static void CALLBACK Entry(void* data) { RunEntry(*static_cast<GdbFrame*>(data)); }
#else
    static void Entry() { RunEntry(*Current); }
#endif

public:
    GdbFrame()
    {
#ifdef _WIN32
        Converted = !IsThreadAFiber();
        Root = Converted ? ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
        if (!Root) throw std::runtime_error("Could not convert the emulation thread to a fiber");
        Child = CreateFiberEx(0, 8 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, Entry, this);
        if (!Child)
        {
            if (Converted) ConvertFiberToThread();
            throw std::runtime_error("Could not allocate the debugger execution stack");
        }
#else
        // libco initializes shared switch code; contexts themselves use TLS.
        std::lock_guard lock(CreationMutex);
        Root = co_active();
        Child = co_create(8 * 1024 * 1024, Entry);
        if (!Child) throw std::runtime_error("Could not allocate the debugger execution stack");
        std::fegetenv(&RootEnv);
        ChildEnv = RootEnv;
#endif
    }

    GdbFrame(const GdbFrame&) = delete;
    GdbFrame& operator=(const GdbFrame&) = delete;

    ~GdbFrame()
    {
        Cancel();
#ifdef _WIN32
        DeleteFiber(Child);
        if (Converted) ConvertFiberToThread();
#else
        co_delete(Child);
#endif
    }

    static void CancelActive() { if (Current) Current->Cancel(); }
    static bool IsSuspended() { return Current && Current->Suspended; }

    void Cancel()
    {
        if (!Done && Suspended)
        {
            Cancelling = true;
            ToChild(); // Unwind on the original stack while its core is alive.
        }
    }

    std::optional<unsigned> Run(std::function<unsigned()> task, const std::function<bool()>& pump)
    {
        struct RestoreHost
        {
            GdbFrame* Previous;
            void (*PreviousCancel)();
            ~RestoreHost() { Current = Previous; Gdb::HostCancel = PreviousCancel; }
        } restore{Current, Gdb::HostCancel};
        Task = std::move(task);
        Done = false;
        Cancelling = false;
        Result.reset();
        Failure = nullptr;
        Current = this;
        Gdb::HostCancel = CancelActive;
        try
        {
            ToChild();
            while (!Done)
                if (pump() && !Done) ToChild();
        }
        catch (...)
        {
            Cancel();
            Task = {};
            throw;
        }
        Task = {};
        if (Failure) std::rethrow_exception(Failure);
        return Result;
    }
};
