// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef AUDIOCLOCKCORRECTION_H
#define AUDIOCLOCKCORRECTION_H

#include <algorithm>
#include <cmath>
#include <cstdint>

// Callback-side delivery record. Clear lastTick/lastFrames on callback-stream
// restart; cumulative counters may remain. Record under the existing audio lock.
// No allocation, logging, SDL, or controller work.
struct AudioClockDelivery
{
    uint64_t requested = 0, supplied = 0, callbacks = 0, lastTick = 0, gapSerial = 0;
    unsigned lastFrames = 0;

    // startTick/frequency: same monotonic performance clock (ticks, ticks/s)
    // as the producer. rate: output frames/s.
    void Record(unsigned requestedFrames, unsigned suppliedFrames, uint64_t startTick, double frequency, double rate)
    {
        if (lastTick != 0)
        {
            bool gap = true;
            if (startTick > lastTick && frequency > 0.0 && rate > 0.0 && std::isfinite(frequency) && std::isfinite(rate))
            {
                double dt = double(startTick - lastTick) / frequency;
                double limit = std::max(0.050, 3.0 * double(lastFrames) / rate);
                gap = !(dt <= limit);
            }
            if (gap) gapSerial++;
        }
        requested += requestedFrames;
        supplied += suppliedFrames;
        callbacks++;
        lastTick = startTick;
        lastFrames = requestedFrames;
    }
};

// Producer-only relative-phase controller. Not thread-safe; never touch it from
// the audio callback. Tracks relative producer/demand phase without a queue target.
class AudioClockCorrection
{
public:
    enum class Status { Inactive, Qualifying, Active, Suspended };
    enum class Reason { None, Saturated, PhaseError };

    struct Observation
    {
        double callbackTime;        // seconds, same clock as Update's now
        unsigned callbackFrames;    // L: last callback's requested frames
        uint64_t requested, supplied, dropped, gapSerial;
        int queued, capacity;       // stereo frames; capacity = usable SPU capacity
    };

    AudioClockCorrection() { Reset(); }

    // Clears everything including the Suspended latch; qualification restarts.
    void Reset(double rate = 0, double fps = 0)
    {
        rate_ = rate;
        fps_ = fps;
        normalDur_ = (fps > 0.0 && std::isfinite(fps)) ? 1.0 / fps : 0.0;
        haveFrame_ = false;
        lastFrame_ = 0;
        reason_ = Reason::None;
        status_ = Status::Qualifying;
        DiscardEpoch();
    }

    // Once per actual completed producer frame. now: seconds at frame completion.
    // obs == nullptr: try-lock missed. Returns the correction (1 = unity).
    double Update(double now, const Observation* o)
    {
        if (status_ == Status::Suspended) return 1.0;
        if (!ConfigValid())
        {
            DiscardEpoch();
            return 1.0;
        }
        if (!std::isfinite(now))
        {
            DiscardEpoch();
            return 1.0;
        }

        bool frameBad = false;
        if (haveFrame_)
        {
            if (now <= lastFrame_) // nonmonotonic producer time
            {
                lastFrame_ = now;
                normalDur_ = 1.0 / fps_;
                DiscardEpoch();
                return 1.0;
            }
            double gap = now - lastFrame_;
            if (gap > std::max(0.033, 2.0 * normalDur_))
            {
                frameBad = true;
                normalDur_ = 1.0 / fps_;
            }
            else if (gap <= 1.5 / fps_)
                normalDur_ = gap;
        }
        haveFrame_ = true;
        lastFrame_ = now;

        if (!o)
        {
            if (frameBad || (haveEpoch_ && now - usableTime_ > 0.050))
                DiscardEpoch();
            return c_;
        }

        // Basic validity. Failures discard the epoch and do not start a new one.
        if (!std::isfinite(o->callbackTime) || o->callbackFrames == 0 || o->capacity <= 0 ||
            o->queued < 0 || o->queued > o->capacity || o->supplied > o->requested ||
            o->callbackTime > now || (haveEpoch_ && o->callbackTime < prevCallbackTime_))
        {
            DiscardEpoch();
            return 1.0;
        }

        // Callback request plus one nominal frame must fit; never resize the queue.
        double L = double(o->callbackFrames);
        if (L + std::ceil(rate_ / fps_) > double(o->capacity))
        {
            DiscardEpoch();
            status_ = Status::Inactive;
            return 1.0;
        }

        double age = now - o->callbackTime;
        if (age > std::max(0.050, 3.0 * L / rate_))
        {
            DiscardEpoch();
            return 1.0;
        }

        uint64_t M = o->requested - o->supplied;
        double x = 0;
        if (haveEpoch_)
        {
            bool restart = frameBad || now - usableTime_ > 0.050 || o->gapSerial != prevGap_ ||
                           o->requested < prevRequested_ || o->supplied < prevSupplied_ ||
                           o->dropped < prevDropped_ || M < M0_ || o->dropped < D0_;
            if (!restart)
            {
                x = Phase(*o, M, age);
                restart = std::fabs(x - prevX_) > rate_ * 0.005 + 4.0;
            }
            if (restart)
                haveEpoch_ = false;
        }
        if (!haveEpoch_)
        {
            BeginEpoch(now, *o, M, age);
            return 1.0;
        }

        double dt = now - prevTime_;
        if (!(dt > 0.0)) { DiscardEpoch(); return 1.0; }
        prevTime_ = usableTime_ = now;
        prevX_ = x;
        prevCallbackTime_ = o->callbackTime;
        prevRequested_ = o->requested; prevSupplied_ = o->supplied; prevDropped_ = o->dropped;
        prevGap_ = o->gapSerial;

        if (status_ != Status::Active)
        {
            sum_ += x * dt;
            duration_ += dt;
            if (duration_ >= 1.0)
            {
                a_ = sum_ / duration_;
                y_ = x - a_;
                satDuration_ = 0;
                status_ = Status::Active;
            }
            return 1.0;
        }

        if (std::fabs(x - a_) > rate_ * 0.010) return Suspend(Reason::PhaseError);

        double alpha = 1.0 - std::exp(-dt);
        y_ += alpha * ((x - a_) - y_);
        double z = std::copysign(std::max(std::fabs(y_) - 1.0, 0.0), y_);
        double uRaw = z / (rate_ * 4.0);
        if (std::fabs(uRaw) >= 0.001)
        {
            satDuration_ += dt;
            if (satDuration_ >= 2.0) return Suspend(Reason::Saturated);
        }
        else
            satDuration_ = 0;

        double u = std::clamp(uRaw, -0.001, 0.001);
        double step = 0.000100 * dt;
        c_ += std::clamp((1.0 + u) - c_, -step, step);
        c_ = std::clamp(c_, 0.999, 1.001);
        return c_;
    }

    Status GetStatus() const { return status_; }
    Reason GetReason() const { return reason_; }
    double Correction() const { return c_; }

private:
    bool ConfigValid() const
    {
        return std::isfinite(rate_) && rate_ > 0.0 && std::isfinite(fps_) && fps_ > 0.0;
    }

    double Phase(const Observation& o, uint64_t M, double age) const
    {
        // x = q + (D-D0) - (M-M0) + L - F*age; deltas converted separately.
        return double(o.queued) + double(o.dropped - D0_) - double(M - M0_) +
               double(o.callbackFrames) - rate_ * age;
    }

    void BeginEpoch(double now, const Observation& o, uint64_t M, double age)
    {
        M0_ = M;
        D0_ = o.dropped;
        prevRequested_ = o.requested; prevSupplied_ = o.supplied; prevDropped_ = o.dropped;
        prevGap_ = o.gapSerial;
        prevCallbackTime_ = o.callbackTime;
        prevTime_ = usableTime_ = now;
        prevX_ = Phase(o, M, age);
        sum_ = duration_ = a_ = y_ = satDuration_ = 0;
        c_ = 1.0;
        haveEpoch_ = true;
        status_ = Status::Qualifying;
    }

    void DiscardEpoch()
    {
        haveEpoch_ = false;
        c_ = 1.0;
        sum_ = duration_ = a_ = y_ = satDuration_ = 0;
        if (status_ != Status::Suspended)
            status_ = ConfigValid() ? Status::Qualifying : Status::Inactive;
    }

    double Suspend(Reason r)
    {
        DiscardEpoch();
        status_ = Status::Suspended;
        reason_ = r;
        return 1.0;
    }

    double rate_ = 0, fps_ = 0, normalDur_ = 0;
    bool haveFrame_ = false, haveEpoch_ = false;
    double lastFrame_ = 0;
    Status status_ = Status::Inactive;
    Reason reason_ = Reason::None;
    double c_ = 1.0;

    uint64_t M0_ = 0, D0_ = 0, prevRequested_ = 0, prevSupplied_ = 0, prevDropped_ = 0, prevGap_ = 0;
    double prevCallbackTime_ = 0, prevTime_ = 0, usableTime_ = 0, prevX_ = 0;
    double sum_ = 0, duration_ = 0, a_ = 0, y_ = 0, satDuration_ = 0;
};

#endif
