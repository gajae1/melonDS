// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef AUDIOOUTPUTRAMP_H
#define AUDIOOUTPUTRAMP_H

#include <algorithm>
#include <cstdint>

// Callback-owned recovery for missing host samples. It adds no buffering and
// leaves continuously supplied audio unchanged; it cannot recover lost sound.
class AudioOutputRamp
{
public:
    void Init(int sampleRate)
    {
        RampFrames = std::max(1, sampleRate / 1000); // one millisecond
        Reset();
    }

    void Reset()
    {
        Last[0] = Last[1] = 0;
        Remaining = 0;
        Starved = false;
    }

    void Process(int16_t* samples, int supplied, int requested)
    {
        if (requested <= 0) return;
        if (supplied > 0 && Starved) Transition(false);
        for (int i = 0; i < supplied && Remaining; ++i)
        {
            --Remaining;
            for (int ch = 0; ch < 2; ++ch)
                samples[2 * i + ch] = static_cast<int16_t>(
                    (int64_t(samples[2 * i + ch]) * (RampFrames - Remaining) +
                     int64_t(From[ch]) * Remaining) / RampFrames);
        }
        if (supplied > 0)
        {
            Last[0] = samples[2 * (supplied - 1)];
            Last[1] = samples[2 * (supplied - 1) + 1];
        }
        if (supplied < requested)
        {
            if (!Starved) Transition(true);
            int i = supplied;
            for (; i < requested && Remaining; ++i)
            {
                --Remaining;
                for (int ch = 0; ch < 2; ++ch)
                    samples[2 * i + ch] = Last[ch] = static_cast<int16_t>(
                        int64_t(From[ch]) * Remaining / RampFrames);
            }
            if (i < requested)
            {
                std::fill_n(samples + 2 * i, 2 * (requested - i), int16_t{0});
                Last[0] = Last[1] = 0;
            }
        }
    }

    void FadeIn()
    {
        Reset();
        Starved = true;
    }

    // Crossfade from the last sample actually submitted, including silence.
    void FadeFromLast() { Transition(false); }

private:
    void Transition(bool starved)
    {
        From[0] = Last[0];
        From[1] = Last[1];
        Remaining = RampFrames;
        Starved = starved;
    }
    int RampFrames = 48, Remaining = 0;
    int16_t Last[2]{}, From[2]{};
    bool Starved = false;
};
#endif
