/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <bit>
#include <cstring>
#include <exception>
#include <utility>
#include "Config.h"
#include "NDS.h"
#include "SPU.h"
#include "Platform.h"
#include "main.h"
#include "AudioSettingsDialog.h"

#include "mic_blow.h"

using namespace melonDS;

// --- AUDIO OUTPUT -----------------------------------------------------------


void EmuInstance::audioInit()
{
    audioVolume = localCfg.GetInt("Audio.Volume");
    audioDSiVolumeSync = localCfg.GetBool("Audio.DSiVolumeSync");
    audioLowPassCutoff = globalCfg.GetInt("Audio.LowPassCutoff");
    audioTimeStretchEnabled = globalCfg.GetBool("Audio.TimeStretch");

    audioMutedToggle = false;
    audioMutedByFastForward = false;
    audioMutedByWindowFocus = false;
    audioSyncCond = SDL_CreateCondition();
    audioSyncLock = SDL_CreateMutex();

    audioFreq = 48000;
    AudioOutput::Settings settings{globalCfg.GetInt("Audio.OutputBackend"),
        globalCfg.GetString("Audio.OutputDevice"), globalCfg.GetInt("Audio.BufferSize")};
    audioBufSize = settings.frames;
    std::string error;
    if (!audioOpenOutput(settings, error))
    {
        Platform::Log(Platform::LogLevel::Error, "Audio init failed: %s\n", error.c_str());
        // A saved endpoint may be absent on another computer/platform. Keep
        // the preference in the config and expose the actual fallback below.
        if ((settings.backend != AudioOutput::SDL || !settings.device.empty()) &&
            !audioOpenOutput({AudioOutput::SDL, {}, settings.frames}, error))
            Platform::Log(Platform::LogLevel::Error, "Fallback audio init failed: %s\n", error.c_str());
    }
    if (audioDevice)
    {
        Platform::Log(Platform::LogLevel::Info, "Audio output backend: %s\n", audioDevice.GetSpec().backend.c_str());
        Platform::Log(Platform::LogLevel::Info, "Audio output frequency: %d Hz\n", audioFreq);
        Platform::Log(Platform::LogLevel::Info, "Audio output buffer size: %d samples\n", audioBufSize);
    }

    audioLowPass.Init(audioFreq);
    audioOutputRamp.Init(audioFreq);
    const char* diagnostics = SDL_getenv("MELONDS_AUDIO_DIAGNOSTICS");
    audioDiagnostics.Enabled = diagnostics && std::strcmp(diagnostics, "1") == 0;

    micStarted = false;
#ifdef MELONDS_SDL3
    micStream = nullptr;
#else
    micDevice = 0;
#endif
    micWavBuffer = nullptr;
    micBuffer = nullptr;

    micLock = SDL_CreateMutex();

    setupMicInputData();
}

bool EmuInstance::audioOpenOutput(const AudioOutput::Settings& settings, std::string& error)
{
    if (!audioDevice.Open(settings, audioCallback, this, error)) return false;
    if (audioTimeStretchEnabled && !audioTimeStretch.Configure(audioDevice.GetSpec().rate, error))
    {
        audioDevice.Close();
        return false;
    }
    // Newly opened devices are stopped. Publish the complete callback state
    // before the emulation thread resumes delivery.
    audioFreq = audioDevice.GetSpec().rate;
    audioBufSize = audioDevice.GetSpec().frames;
    return true;
}

bool EmuInstance::audioSetOutput(const AudioOutput::Settings& requested, std::string& error)
{
    // The UI has stopped the producer and waits for output callbacks to finish.
    // The output owner serializes native Open/Close on its COM thread.
    auto settings = requested;
    settings.frames = std::bit_ceil(static_cast<unsigned>(std::clamp(settings.frames, 32, 1024)));
    error.clear();
    if (audioDevice.IsOpening())
    {
        error = "Audio output recovery is still in progress. Try again shortly.";
        return false;
    }
    if (audioDevice && !audioDevice.NeedsRecovery() && settings == audioDevice.GetSettings()) return true;
    const bool hadDevice = static_cast<bool>(audioDevice);
    const bool restorePrevious = hadDevice && !audioDevice.NeedsRecovery();
    const auto previous = audioDevice.GetSettings();
    const int previousRate = audioFreq;
    if (audioDevice)
    {
        audioDevice.Stop();
        audioReportDiagnostics();
        audioDevice.Close();
    }
    const bool applied = audioOpenOutput(settings, error);
    if (!applied)
    {
        std::string recovery;
        if (restorePrevious && !audioOpenOutput(previous, recovery))
            error += std::string("; previous output could not be reopened: ") + recovery;
    }
    if (audioDevice) audioUpdateOutputState(previousRate);
    return applied;
}

void EmuInstance::audioUpdateOutputState(int previousRate)
{
    if (nds)
    {
        if (audioFreq != previousRate) nds->SPU.SetOutputSampleRate(audioFreq);
        nds->SPU.ResetOutputHistory();
    }
    audioLowPass.Init(audioFreq);
    const int cutoff = audioLowPassCutoff.load(std::memory_order_relaxed);
    audioLowPass.SetCutoffNow(cutoff > 0 ? cutoff : audioLowPass.WideOpenCutoff());
    audioOutputRamp.Init(audioFreq);
    audioOutputRamp.FadeIn();
    const bool diagnostics = audioDiagnostics.Enabled;
    audioDiagnostics = {};
    audioDiagnostics.Enabled = diagnostics;
}

QString EmuInstance::audioOutputDescription() const
{
    // UI reads published state; a worker result is not adopted until ready.
    if (audioDevice.IsOpening()) return QObject::tr("Reconnecting audio output...");
    if (!audioDevice) return QObject::tr("Audio output unavailable");
    const auto& spec = audioDevice.GetSpec();
    QString description = QObject::tr("%1: %2 frames at %3 Hz (%4 ms of audio)")
        .arg(QString::fromStdString(spec.backend)).arg(audioBufSize).arg(audioFreq)
        .arg(audioBufSize * 1000.0 / audioFreq, 0, 'f', 2);
    if (spec.bufferFrames > 0)
        description += QObject::tr("; device capacity %1 frames").arg(spec.bufferFrames);
    if (audioTimeStretchEnabled)
        description += QObject::tr("; pitch preservation adds processing delay");
    if (audioRecoveryFallback)
        description += QObject::tr("; temporary fallback (saved output preference retained)");
    return description;
}

void EmuInstance::audioStartRecovery()
{
    // Brief producer stops detach/adopt state; native work is asynchronous.
    audioRecoveryTimer = std::make_unique<QTimer>();
    audioRecoveryTimer->setInterval(500);
    QObject::connect(audioRecoveryTimer.get(), &QTimer::timeout, audioRecoveryTimer.get(),
        [this] { audioCheckOutput(); });
    audioRecoveryTimer->start();
}

void EmuInstance::audioCheckOutput()
{
    if (deleting) return;
    const bool opening = audioDevice.IsOpening();
    // A dialog opened before recovery owns its output. Once recovery starts,
    // output preview requests are refused until its paused result is adopted.
    if (!opening && (AudioSettingsDialog::currentDlg || !audioDevice.NeedsRecovery())) return;
    if (opening && !audioDevice.IsOpenReady()) return;

    const AudioOutput::Settings preferred{globalCfg.GetInt("Audio.OutputBackend"),
        globalCfg.GetString("Audio.OutputDevice"), globalCfg.GetInt("Audio.BufferSize")};
    emuThread->emuPause(false);
    std::string error;
    bool restored = false;
    bool pending = false;
    if (!opening)
    {
        audioRecoveryPreferred = preferred;
        audioRecoveryTryingFallback = false;
        audioRecoveryPrimaryError.clear();
        audioRecoverySetupError.clear();
        pending = audioDevice.BeginReopen(preferred, audioCallback, this, error);
        audioReportDiagnostics();
    }
    else
    {
        const int previousRate = audioFreq;
        restored = audioDevice.FinishReopen(error);
        if (!audioRecoverySetupError.empty())
            error = std::exchange(audioRecoverySetupError, {});
        if (preferred != audioRecoveryPreferred)
        {
            // Another instance may have changed the shared preference. The old
            // result stays paused and is retired by the same native owner.
            audioRecoveryPreferred = preferred;
            audioRecoveryTryingFallback = false;
            audioRecoveryPrimaryError.clear();
            restored = false;
            pending = audioDevice.BeginReopen(preferred, audioCallback, this, error);
            if (!pending) audioDevice.Close(); // Never start a stale result after allocation failure.
        }
        else
        {
            if (restored && audioTimeStretchEnabled &&
                !audioTimeStretch.Configure(audioDevice.GetSpec().rate, error))
            {
                audioRecoverySetupError = error;
                pending = audioDevice.BeginClose(error);
                // No asynchronous job could be allocated. Preserve ownership
                // and callback safety even when native cleanup must be synchronous.
                if (!pending) audioDevice.Close();
                restored = false;
            }
            if (restored)
            {
                audioFreq = audioDevice.GetSpec().rate;
                audioBufSize = audioDevice.GetSpec().frames;
                audioUpdateOutputState(previousRate);
            }
            else if (!pending && !audioRecoveryTryingFallback &&
                     (preferred.backend != AudioOutput::SDL || !preferred.device.empty()))
            {
                audioRecoveryPrimaryError = error;
                audioRecoveryTryingFallback = true;
                pending = audioDevice.BeginReopen({AudioOutput::SDL, {}, preferred.frames},
                    audioCallback, this, error);
            }
        }
    }
    emuThread->emuUnpause(false);
    if (pending)
    {
        audioRecoveryTimer->setInterval(25);
        return;
    }
    if (restored)
    {
        audioRecoveryFallback = audioDevice.GetSettings().backend != preferred.backend ||
                                audioDevice.GetSettings().device != preferred.device;
        audioRecoveryError.clear();
        audioRecoveryRetryMs = 500;
        Platform::Log(Platform::LogLevel::Info, "Audio output recovered: %s%s\n",
            audioDevice.GetSpec().backend.c_str(), audioRecoveryFallback ? " (temporary default)" : "");
    }
    else
    {
        if (!audioRecoveryPrimaryError.empty())
            error = audioRecoveryPrimaryError + "; default output: " + error;
        if (error != audioRecoveryError)
            Platform::Log(Platform::LogLevel::Error, "Audio output unavailable; will retry: %s\n", error.c_str());
        audioRecoveryError = std::move(error);
        audioRecoveryRetryMs = std::min(audioRecoveryRetryMs * 2, 5000);
    }
    audioRecoveryTimer->setInterval(audioRecoveryRetryMs);
}

bool EmuInstance::changeAudioBuffer(int frames, QString& error)
{
    const auto& current = audioDevice.GetSettings();
    return changeAudioOutput(frames, current.backend, QString::fromStdString(current.device), error);
}

QList<QPair<QString, QString>> EmuInstance::audioOutputDevices(int backend, QString& error)
{
    std::string detail;
    const auto devices = AudioOutput::Enumerate(backend, detail);
    QList<QPair<QString, QString>> result;
    for (const auto& device : devices)
        result.push_back({QString::fromStdString(device.id), device.id.empty()
            ? QObject::tr("System default") : QString::fromStdString(device.name)});
    error = QString::fromStdString(detail);
    return result;
}

bool EmuInstance::changeAudioOutput(int frames, int backend, const QString& device, QString& error)
{
    AudioOutput::Settings settings{backend, device.toStdString(), static_cast<int>(
        std::bit_ceil(static_cast<unsigned>(std::clamp(frames, 32, 1024))))};
    error.clear();
    if (audioDevice.IsOpening())
    {
        error = QObject::tr("Audio output recovery is still in progress. Try again shortly.");
        return false;
    }
    if (audioDevice && !audioDevice.NeedsRecovery() && settings == audioDevice.GetSettings())
    {
        audioRecoveryFallback = false;
        return true;
    }
    emuThread->emuPause(false);
    std::string detail;
    const bool applied = audioSetOutput(settings, detail);
    if (applied)
    {
        audioRecoveryFallback = false;
        audioRecoveryError.clear();
        audioRecoveryRetryMs = 500;
        if (audioRecoveryTimer) audioRecoveryTimer->setInterval(500);
    }
    emuThread->emuUnpause(false);
    error = QString::fromStdString(detail);
    return applied;
}

bool EmuInstance::changeAudioInterpolation(int mode, QString& error)
{
    error.clear();
    if (mode < 0 || mode > int(AudioInterpolation::MinimumPhase))
    {
        error = QObject::tr("Unknown interpolation mode");
        return false;
    }
    if (!nds || nds->SPU.GetInterpolation() == static_cast<AudioInterpolation>(mode)) return true;
    emuThread->emuPause(false);
    audioDevice.Stop();
    bool applied = false;
    try
    {
        nds->SPU.SetInterpolation(static_cast<AudioInterpolation>(mode));
        applied = true;
    }
    catch (const std::exception& failure)
    {
        error = QString::fromUtf8(failure.what());
    }
    emuThread->emuUnpause(false);
    return applied;
}

bool EmuInstance::changeAudioTimeStretch(bool enabled, QString& error)
{
    error.clear();
    if (enabled == audioTimeStretchEnabled) return true;
    emuThread->emuPause(false);
    audioDevice.Stop();
    std::string detail;
    const bool applied = !enabled || audioTimeStretch.Configure(audioFreq, detail);
    if (applied)
    {
        audioTimeStretchEnabled = enabled;
        if (!enabled) audioTimeStretch.Clear();
        if (nds) audioResetOutput();
    }
    emuThread->emuUnpause(false);
    error = QString::fromStdString(detail);
    return applied;
}

void EmuInstance::audioDeInit()
{
    audioDevice.Close();
    audioReportDiagnostics();
    micClose();
    micStarted = false;

    if (audioSyncCond) SDL_DestroyCondition(audioSyncCond);
    audioSyncCond = nullptr;

    if (audioSyncLock) SDL_DestroyMutex(audioSyncLock);
    audioSyncLock = nullptr;

    if (micWavBuffer) delete[] micWavBuffer;
    micWavBuffer = nullptr;

    if (micLock) SDL_DestroyMutex(micLock);
    micLock = nullptr;
}

void EmuInstance::updateAudioMuteByWindowFocus()
{
    audioMutedByWindowFocus = false;
    if (numEmuInstances() < 2) return;

    switch (mpAudioMode)
    {
        case 1: // only instance 1
            if (instanceID > 0) audioMutedByWindowFocus = true;
            break;

        case 2: // only currently focused instance
            audioMutedByWindowFocus = true;
            for (int i = 0; i < kMaxWindows; i++)
            {
                if (!windowList[i]) continue;
                if (windowList[i]->isFocused())
                {
                    audioMutedByWindowFocus = false;
                    break;
                }
            }
            break;
    }
}

void EmuInstance::toggleAudioMute()
{
    audioMutedToggle = !audioMutedToggle;
}

void EmuInstance::updateFastForwardMute(bool fastForward)
{
    audioMutedByFastForward = fastForward && globalCfg.GetBool("MuteFastForward");
}

void EmuInstance::audioTimeStretchFailed()
{
    // Processing is never called by the device callback. Quiesce that callback
    // before replacing its source and recovery ramp after a native failure.
    const bool running = audioDevice.IsRunning();
    audioDevice.Stop();
    audioTimeStretchEnabled = false;
    audioTimeStretch.Clear();
    if (nds) nds->SPU.ResetOutputHistory();
    audioOutputRamp.FadeIn();
    Platform::Log(Platform::LogLevel::Error,
        "Pitch-preserving processing failed; restored the original audio path\n");
    if (running)
    {
        std::string error;
        if (!audioDevice.Start(error))
            Platform::Log(Platform::LogLevel::Error, "Audio recovery failed: %s\n", error.c_str());
    }
}

void EmuInstance::audioSetSpeed(double speed)
{
    if (audioTimeStretchEnabled && !audioTimeStretch.SetSpeed(speed)) audioTimeStretchFailed();
}

void EmuInstance::audioPumpTimeStretch(int maxQueued)
{
    if (!audioTimeStretchEnabled || !nds) return;
    audioTimeStretch.Drain();
    std::array<int16_t, AudioTimeStretch::BlockFrames * 2> input;
    // Leave excess source/native output pending instead of dropping samples or
    // growing a second unbounded queue. audioSync pumps again after consumption.
    while (audioTimeStretch.QueuedFrames() < static_cast<size_t>(std::max(1, maxQueued)) &&
           audioTimeStretch.CanPush())
    {
        const int count = nds->SPU.ReadOutput(input.data(), AudioTimeStretch::BlockFrames);
        if (!count) break;
        if (!audioTimeStretch.Push(input.data(), count)) break;
    }
    if (!audioTimeStretch.Healthy()) audioTimeStretchFailed();
}

void EmuInstance::audioSync(int frameSamples, std::stop_token stopToken)
{
    if (audioIsRunning() && !stopToken.stop_requested())
    {
        // The producer advances a whole emulated frame at once. A small SDL
        // callback can be delivered in a larger backend burst; waiting for
        // less than one callback then stalls production until that burst has
        // already exhausted the queue. Bound lead by a producer frame instead.
        // The device drains continuously while a produced frame refills the
        // queue. Allow a producer frame plus the reported output block as
        // queued slack to tolerate late production; never keep
        // so much that the next produced frame overflows the ring and drops
        // samples, which would be a different kind of underrun.
        const int capacity = nds->SPU.GetOutputCapacity();
        const int maxQueued = std::clamp(frameSamples + audioBufSize,
            frameSamples, std::max(frameSamples, capacity - frameSamples));
        // Register before locking: an already requested stop can invoke this
        // callback synchronously. Destroy it only after releasing the mutex.
        std::stop_callback wakeOnStop(stopToken, [this] {
            SDL_LockMutex(audioSyncLock);
            SDL_SignalCondition(audioSyncCond);
            SDL_UnlockMutex(audioSyncLock);
        });
        audioPumpTimeStretch(maxQueued);
        SDL_LockMutex(audioSyncLock);
        while (!stopToken.stop_requested() && audioIsRunning() &&
               (audioTimeStretchEnabled
                   ? (audioTimeStretch.PendingFrames() >= static_cast<size_t>(maxQueued) || nds->SPU.GetOutputSize() > 0)
                   : nds->SPU.GetOutputSize() >= maxQueued))
        {
            if (!SDL_WaitConditionTimeout(audioSyncCond, audioSyncLock, 500)) break;
            // The wait returns with the mutex held. Only the optional native
            // processor needs to run unlocked; ordinary playback can recheck
            // the queue without another unlock/lock round trip on every wake.
            if (audioTimeStretchEnabled)
            {
                SDL_UnlockMutex(audioSyncLock);
                audioPumpTimeStretch(maxQueued);
                SDL_LockMutex(audioSyncLock);
            }
        }
        SDL_UnlockMutex(audioSyncLock);
    }
}

void EmuInstance::audioCallback(void* data, Uint8* stream, int len)
{
    EmuInstance* inst = (EmuInstance*)data;
    len /= (sizeof(s16) * 2);
    if (len < 1) return;

    // The core resampler already converts to the device rate. Always fill the
    // requested device buffer; changing its length here leaves stale samples.
    const Uint64 started = inst->audioDiagnostics.Begin();
    SDL_LockMutex(inst->audioSyncLock);
    int num_in = inst->audioTimeStretchEnabled
        ? static_cast<int>(inst->audioTimeStretch.Read(reinterpret_cast<s16*>(stream), len))
        : inst->nds->SPU.ReadOutput((s16*) stream, len);
    SDL_SignalCondition(inst->audioSyncCond);
    SDL_UnlockMutex(inst->audioSyncLock);
    inst->audioDiagnostics.Record(len, num_in, started);

    const int cutoff = inst->audioLowPassCutoff.load(std::memory_order_relaxed);
    const double targetHz = cutoff > 0 ? cutoff : inst->audioLowPass.WideOpenCutoff();
    const double blockSeconds = static_cast<double>(len) / inst->audioFreq;

    if (inst->audioMutedByWindowFocus || inst->audioMutedToggle || inst->audioMutedByFastForward)
    {
        inst->audioOutputRamp.Reset();
        // Deliver silence immediately, but retain a recovery transition so
        // unmuting fades in instead of jumping to a full-amplitude sample.
        inst->audioOutputRamp.Process(reinterpret_cast<s16*>(stream), 0, len);
        inst->audioLowPass.ProcessMuted(len, targetHz, blockSeconds);
        inst->audioDiagnostics.FinishProcessing(started);
        return;
    }

    const int volume = inst->audioVolume.load(std::memory_order_relaxed);
    if (volume < 256)
    {
        s16* samples = (s16*) stream;
        for (int i = 0; i < num_in * 2; i++)
            samples[i] = ((s32) samples[i] * volume) >> 8;
    }

    inst->audioOutputRamp.Process(reinterpret_cast<s16*>(stream), num_in, len);
    inst->audioLowPass.Process(reinterpret_cast<s16*>(stream), len, targetHz, blockSeconds);
    inst->audioDiagnostics.FinishProcessing(started);
}

void EmuInstance::audioReportDiagnostics()
{
    // Call only after pausing/closing the device so the counters are stable.
    auto& stats = audioDiagnostics;
    if (!stats.Enabled || stats.Callbacks == stats.LastReportedCallbacks) return;
    stats.LastReportedCallbacks = stats.Callbacks;
    const double tickUs = 1e6 / SDL_GetPerformanceFrequency();
    Platform::Log(Platform::LogLevel::Info,
        "Audio delivery: rate=%d buffer=%d callbacks=%llu requested=%llu supplied=%llu "
        "missing=%llu underruns=%llu empty=%llu max_read_us=%.1f max_gap_us=%.1f "
        "max_processing_us=%.1f core_dropped_total=%llu queue_frames=%d\n",
        audioFreq, audioBufSize, (unsigned long long)stats.Callbacks,
        (unsigned long long)stats.RequestedFrames, (unsigned long long)stats.SuppliedFrames,
        (unsigned long long)(stats.RequestedFrames - stats.SuppliedFrames),
        (unsigned long long)stats.Underruns, (unsigned long long)stats.EmptyCallbacks,
        stats.MaxReadTicks * tickUs, stats.MaxGapTicks * tickUs,
        stats.MaxProcessingTicks * tickUs,
        (unsigned long long)(nds ? nds->SPU.GetOutputDroppedFrames() : 0),
        nds ? nds->SPU.GetOutputSize() : 0);
}


// --- MIC INPUT --------------------------------------------------------------


void EmuInstance::micOpen()
{
#ifdef MELONDS_SDL3
    if (micStream) return;
#else
    if (micDevice) return;
#endif

    SDL_LockMutex(micLock);
    memset(micExtBuffer, 0, sizeof(micExtBuffer));
    micExtBufferWritePos = 0;
    micExtBufferCount = 0;
    micBufferReadPos = 0;
    micSampleFrac = 0;
    SDL_UnlockMutex(micLock);

    if (micInputType != micInputType_External)
    {
#ifdef MELONDS_SDL3
        micStream = nullptr;
#else
        micDevice = 0;
#endif
        return;
    }

#ifdef MELONDS_SDL3
    int numMics = 0;
    SDL_AudioDeviceID* mics = SDL_GetAudioRecordingDevices(&numMics);
    if (!mics || numMics == 0)
    {
        SDL_free(mics);
        return;
    }

    SDL_AudioDeviceID devid = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!micDeviceName.empty())
    {
        devid = 0;
        for (int i = 0; i < numMics; ++i)
        {
            const char* name = SDL_GetAudioDeviceName(mics[i]);
            if (name && micDeviceName == name)
            {
                devid = mics[i];
                break;
            }
        }
    }
    SDL_free(mics);
    if (!devid)
    {
        Platform::Log(Platform::LogLevel::Error,
            "Mic init failed: recording device '%s' not found\n", micDeviceName.c_str());
        return;
    }

    micFreq = 48000;
    micBufSize = 1024;
    // Ask for the device's own rate so the stream does no sample-rate
    // conversion; micResample already handles any source rate.
    SDL_AudioSpec devspec{};
    if (SDL_GetAudioDeviceFormat(devid, &devspec, nullptr) && devspec.freq > 0)
        micFreq = devspec.freq;

    SDL_AudioSpec spec{};
    spec.freq = micFreq;
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");
    micStream = SDL_OpenAudioDeviceStream(devid, &spec, micCallbackSDL3, this);
    if (!micStream)
    {
        Platform::Log(Platform::LogLevel::Error, "Mic init failed: %s\n", SDL_GetError());
    }
    else
    {
        Platform::Log(Platform::LogLevel::Info, "Mic output frequency: %d Hz\n", micFreq);
        Platform::Log(Platform::LogLevel::Info, "Mic output buffer size: %d samples\n", micBufSize);
        // Streams open paused; recording starts on resume.
        SDL_ResumeAudioStreamDevice(micStream);
    }
#else
    int numMics = SDL_GetNumAudioDevices(1);
    if (numMics == 0)
        return;

    micFreq = 48000;
    micBufSize = 1024;
    SDL_AudioSpec whatIwant, whatIget;
    memset(&whatIwant, 0, sizeof(SDL_AudioSpec));
    whatIwant.freq = micFreq;
    whatIwant.format = AUDIO_S16LSB;
    whatIwant.channels = 1;
    whatIwant.samples = micBufSize;
    whatIwant.callback = micCallback;
    whatIwant.userdata = this;
    const char* mic = NULL;
    if (micDeviceName != "")
    {
        mic = micDeviceName.c_str();
    }
    micDevice = SDL_OpenAudioDevice(mic, 1, &whatIwant, &whatIget, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!micDevice)
    {
        Platform::Log(Platform::LogLevel::Error, "Mic init failed: %s\n", SDL_GetError());
    }
    else
    {
        micFreq = whatIget.freq;
        micBufSize = whatIget.samples;
        Platform::Log(Platform::LogLevel::Info, "Mic output frequency: %d Hz\n", micFreq);
        Platform::Log(Platform::LogLevel::Info, "Mic output buffer size: %d samples\n", micBufSize);
        SDL_PauseAudioDevice(micDevice, 0);
    }
#endif
}

void EmuInstance::micClose()
{
#ifdef MELONDS_SDL3
    if (micStream)
        SDL_DestroyAudioStream(micStream);

    micStream = nullptr;
#else
    if (micDevice)
        SDL_CloseAudioDevice(micDevice);

    micDevice = 0;
#endif
}

void EmuInstance::micStart()
{
    micStarted = true;
    micOpen();
}

void EmuInstance::micStop()
{
    micClose();
    micStarted = false;
}

void EmuInstance::micLoadWav(const std::string& name)
{
    SDL_AudioSpec format;
    memset(&format, 0, sizeof(SDL_AudioSpec));

    if (micWavBuffer) delete[] micWavBuffer;
    micWavBuffer = nullptr;
    micWavLength = 0;

    u8* buf;
    u32 len;
    if (!SDL_LoadWAV(name.c_str(), &format, &buf, &len))
        return;

    if (len > 0x4000000)
    {
        SDL_free(buf);
        return;
    }

#ifdef MELONDS_SDL3
    // SDL3 replaces AudioCVT with an audio stream used here as an offline
    // converter: put the whole file, flush the resampler tail, read it back.
    SDL_AudioSpec dst{};
    dst.format = SDL_AUDIO_S16LE;
    dst.channels = 1;
    dst.freq = 47743;

    if (format.format == dst.format && format.channels == dst.channels && format.freq == dst.freq)
    {
        // no conversion needed
        micWavLength = len >> 1;
        micWavBuffer = new s16[micWavLength];
        memcpy(micWavBuffer, buf, len);
    }
    else
    {
        SDL_AudioStream* cvt = SDL_CreateAudioStream(&format, &dst);
        if (!cvt)
        {
            SDL_free(buf);
            return;
        }
        if (!SDL_PutAudioStreamData(cvt, buf, static_cast<int>(len)) ||
            !SDL_FlushAudioStream(cvt))
        {
            SDL_DestroyAudioStream(cvt);
            SDL_free(buf);
            return;
        }
        const int converted = SDL_GetAudioStreamAvailable(cvt);
        if (converted <= 0)
        {
            SDL_DestroyAudioStream(cvt);
            SDL_free(buf);
            return;
        }
        micWavBuffer = new s16[converted >> 1];
        micWavLength = SDL_GetAudioStreamData(cvt, micWavBuffer, converted) >> 1;
        SDL_DestroyAudioStream(cvt);
    }

    SDL_free(buf);
#else
    SDL_AudioCVT cvt;
    int cvtres = SDL_BuildAudioCVT(&cvt,
        format.format, format.channels, format.freq,
        AUDIO_S16LSB, 1, 47743);

    if (cvtres < 0)
    {
        // failure
        SDL_free(buf);
        return;
    }

    if (cvtres == 0)
    {
        // no conversion needed
        micWavLength = len >> 1;
        micWavBuffer = new s16[micWavLength];
        memcpy(micWavBuffer, buf, len);
    }
    else
    {
        // apply conversion
        cvt.len = len;
        cvt.buf = new u8[cvt.len * cvt.len_mult];
        memcpy(cvt.buf, buf, len);

        if (SDL_ConvertAudio(&cvt) < 0)
        {
            delete[] cvt.buf;
            SDL_free(buf);
            return;
        }

        micWavLength = cvt.len_cvt >> 1;
        micWavBuffer = new s16[micWavLength];
        memcpy(micWavBuffer, cvt.buf, cvt.len_cvt);
        delete[] cvt.buf;
    }

    SDL_free(buf);
#endif
}

void EmuInstance::setupMicInputData()
{
    if (micWavBuffer != nullptr)
    {
        delete[] micWavBuffer;
        micWavBuffer = nullptr;
        micWavLength = 0;
    }

    micInputType = globalCfg.GetInt("Mic.InputType");
    micDeviceName = globalCfg.GetString("Mic.Device");
    micWavPath = globalCfg.GetString("Mic.WavPath");

    switch (micInputType)
    {
        case micInputType_Silence:
            micBuffer = nullptr;
            micBufferLength = 0;
            break;
        case micInputType_External:
            micBuffer = micExtBuffer;
            micBufferLength = sizeof(micExtBuffer) / sizeof(s16);
            break;
        case micInputType_Noise:
            micBuffer = (s16*)&mic_blow[0];
            micBufferLength = sizeof(mic_blow) / sizeof(s16);
            break;
        case micInputType_Wav:
            micLoadWav(micWavPath);
            micBuffer = micWavBuffer;
            micBufferLength = micWavLength;
            break;
    }

    micBufferReadPos = 0;
}

int EmuInstance::micReadInput(s16* data, int maxlength)
{
    int type = micInputType;

    bool cmd = hotkeyDown(HK_Mic);

    if ((!micBuffer) ||
        ((type != micInputType_External) && (!cmd)))
    {
        type = micInputType_Silence;
    }

    if (type == micInputType_Silence)
    {
        micBufferReadPos = 0;
        memset(data, 0, maxlength * sizeof(s16));
        return maxlength;
    }

    if (type == micInputType_External)
        SDL_LockMutex(micLock);

    int readlength = 0;
    while (readlength < maxlength)
    {
        int thislen = maxlength - readlength;
        if ((micBufferReadPos + thislen) > micBufferLength)
            thislen = micBufferLength - micBufferReadPos;

        if (type == micInputType_External)
        {
            if (thislen > micExtBufferCount)
                thislen = micExtBufferCount;

            micExtBufferCount -= thislen;
        }

        if (!thislen)
            break;

        memcpy(data, &micBuffer[micBufferReadPos], thislen * sizeof(s16));
        data += thislen;
        micBufferReadPos += thislen;
        if (micBufferReadPos >= micBufferLength)
            micBufferReadPos -= micBufferLength;

        readlength += thislen;
    }

    if (type == micInputType_External)
        SDL_UnlockMutex(micLock);

    return readlength;
}

int EmuInstance::micGetNumSamplesIn(int inlen)
{
    const double fps = curFPS.load(std::memory_order_relaxed);
    float f_len_out = (inlen * 47743.4659091 * (fps/60.0)) / (float)micFreq;
    f_len_out += micSampleFrac;
    int len_out = (int)floor(f_len_out);
    micSampleFrac = f_len_out - len_out;

    return len_out;
}

void EmuInstance::micResample(s16* inbuf, int inlen)
{
    if (inlen <= 0) return;

    int maxlen = sizeof(micExtBuffer) / sizeof(s16);
    int outlen = micGetNumSamplesIn(inlen);

    // alter output length slightly to keep the buffer happy
    if (micExtBufferCount < (maxlen >> 2))
        outlen += 6;
    else if (micExtBufferCount > (3 * (maxlen >> 2)))
        outlen -= 6;

    if (outlen <= 0) return;

    float res_incr = inlen / (float)outlen;
    float res_timer = -0.5;
    int res_pos = 0;

    for (int i = 0; i < outlen; i++)
    {
        if (micExtBufferCount >= maxlen)
            break;

        s16 s1 = inbuf[res_pos];
        // The callback owns only inlen samples; hold the last one at its end.
        s16 s2 = res_pos + 1 < inlen ? inbuf[res_pos + 1] : s1;

        float s = (float)s1 + ((s2 - s1) * res_timer);

        micExtBuffer[micExtBufferWritePos] = (s16)round(s);
        micExtBufferWritePos++;
        if (micExtBufferWritePos >= maxlen)
            micExtBufferWritePos = 0;

        micExtBufferCount++;

        res_timer += res_incr;
        while (res_timer >= 1.0)
        {
            res_timer -= 1.0;
            res_pos++;
        }
    }
}

void EmuInstance::micCallback(void* data, Uint8* stream, int len)
{
    EmuInstance* inst = (EmuInstance*)data;
    s16* input = (s16*)stream;
    len /= sizeof(s16);

    SDL_LockMutex(inst->micLock);
    inst->micResample(input, len);
    SDL_UnlockMutex(inst->micLock);
}

#ifdef MELONDS_SDL3
void EmuInstance::micCallbackSDL3(void* data, SDL_AudioStream* stream, int additional, int)
{
    // Recording streams fire the callback when captured data is available;
    // dequeue exactly what arrived and feed the shared resample path.
    EmuInstance* inst = (EmuInstance*)data;
    additional &= ~1; // whole s16 samples
    if (additional <= 0) return;
    if (inst->micScratch.size() < static_cast<size_t>(additional))
        inst->micScratch.resize(additional);
    const int got = SDL_GetAudioStreamData(stream, inst->micScratch.data(), additional);
    if (got > 0) micCallback(data, inst->micScratch.data(), got);
}
#endif





void EmuInstance::audioUpdateSettings()
{
    audioLowPassCutoff = globalCfg.GetInt("Audio.LowPassCutoff");
    if (micInputType == globalCfg.GetInt("Mic.InputType") &&
        micDeviceName == globalCfg.GetString("Mic.Device") &&
        micWavPath == globalCfg.GetString("Mic.WavPath")) return;
    if (micStarted) micClose();

    setupMicInputData();
    if (micStarted) micOpen();
}

void EmuInstance::audioResetOutput()
{
    // The caller has paused the device; discard every host output history while
    // retaining the user's output rate, volume and current filter cutoff.
    nds->SPU.ResetOutputHistory();
    if (audioTimeStretchEnabled && !audioTimeStretch.Reset()) audioTimeStretchFailed();
    const double cutoff = audioLowPass.Cutoff();
    audioLowPass.Init(audioFreq);
    audioLowPass.SetCutoffNow(cutoff);
    audioOutputRamp.Reset();
}

void EmuInstance::audioEnable()
{
    audioDevice.Stop();
    // Device setup can block. Complete capture setup before allowing playback
    // to consume the retained queue while the producer is still stopped here.
    if (micStarted) micOpen();
    audioStartRequested = static_cast<bool>(audioDevice);
    if (audioDevice)
    {
        // The device transition runs after filtering, so retained filter state
        // cannot bypass the fade. Reset only the source-shortage history here.
        audioOutputRamp.Reset();
        audioDiagnostics.PreviousStart = 0; // paused time is not callback lateness
        audioStartPending();
    }
}

void EmuInstance::audioStartPending()
{
    if (!audioStartRequested || !nds) return;
    const bool ready = audioTimeStretchEnabled
        ? audioTimeStretch.QueuedFrames() != 0 : nds->SPU.GetOutputSize() != 0;
    if (!ready) return;
    // Do not consume an empty source after boot/load/reopen. The producer calls
    // again after producing PCM; no queue growth, sample discard or ongoing
    // underrun suppression is used. Retained PCM resumes immediately.
    audioStartRequested = false;
    std::string error;
    if (!audioDevice.Start(error))
        Platform::Log(Platform::LogLevel::Error, "Audio start failed: %s\n", error.c_str());
}

void EmuInstance::audioDisable()
{
    audioStartRequested = false;
    audioDevice.Stop();
    audioReportDiagnostics();
    if (micStarted) micClose();
}
