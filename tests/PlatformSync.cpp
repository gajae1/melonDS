// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only host synchronization. No SDL/Qt, audio devices or network access.
#include "Platform.h"
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <thread>
namespace melonDS::Platform {
struct Mutex { std::mutex value; };
Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* m) { delete m; }
void Mutex_Lock(Mutex* m) { m->value.lock(); }
void Mutex_Unlock(Mutex* m) { m->value.unlock(); }
bool Mutex_TryLock(Mutex* m) { return m->value.try_lock(); }
struct Thread { std::thread value; explicit Thread(std::function<void()> f) : value(std::move(f)) {} };
Thread* Thread_Create(std::function<void()> f) { return new Thread(std::move(f)); }
void Thread_Wait(Thread* t) { if (t && t->value.joinable()) t->value.join(); }
void Thread_Free(Thread* t) { Thread_Wait(t); delete t; }
struct Semaphore { std::mutex mutex; std::condition_variable ready; int count = 0; };
Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* s) { delete s; }
void Semaphore_Reset(Semaphore* s) { std::lock_guard lock(s->mutex); s->count = 0; }
void Semaphore_Post(Semaphore* s, int count) {
    { std::lock_guard lock(s->mutex); s->count += count; }
    s->ready.notify_all();
}
void Semaphore_Wait(Semaphore* s) {
    std::unique_lock lock(s->mutex);
    s->ready.wait(lock, [s] { return s->count > 0; }); --s->count;
}
bool Semaphore_TryWait(Semaphore* s, int milliseconds) {
    std::unique_lock lock(s->mutex);
    if (!s->ready.wait_for(lock, std::chrono::milliseconds(milliseconds), [s] { return s->count > 0; })) return false;
    --s->count; return true;
}
void Log(LogLevel level, const char* fmt, ...) {
    if (level < LogLevel::Warn) return;
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
}
}
