// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef RENDER_COST_H
#define RENDER_COST_H

// GR-14: opt-in render/present cost measurement.
//
// Enabled with MELONDS_RENDER_DIAGNOSTICS=1 (same opt-in style as
// MELONDS_AUDIO_DIAGNOSTICS). When disabled, every instrumentation point
// reduces to one boolean check and no GL calls are made.
//
// Stage semantics:
// - CPU stages record one sample per emu frame (or per present call in the
//   frontend meters): the frame's summed nanoseconds for that stage. Stages
//   that did not run in a frame record no sample, so their medians describe
//   only frames where they ran.
// - GPU stages record one sample per measured span: GPU elapsed nanoseconds
//   between the span's start/end commands, retrieved asynchronously by
//   polling GL_QUERY_RESULT_AVAILABLE. There is no glFinish and no forced
//   synchronous retrieval. When the current context cannot provide timer
//   queries, GPU stages stay unmeasured and reports say gpu=unsup.
// - All state is fixed-size bounded rings. Reports are one line per
//   RenderCostReportInterval frames plus one final line at destruction.
// - Stages are non-overlapping: nested stages (aux texture upload inside the
//   final pass, software present upload inside present issue) are subtracted
//   from their parent at sample time. Unlisted residual CPU work (register
//   diffing, aux staging copies) is intentionally not attributed to a stage.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// CPU/Vulkan accounting must also compile without an OpenGL loader.
#if defined(OGLRENDERER_ENABLED) || defined(PLATFORMOGL_H) || defined(__glad_h_)
#include "PlatformOGL.h"
#endif

namespace melonDS
{

constexpr int RenderCostReportInterval = 256;

inline bool RenderCostEnabled()
{
    // Same opt-in value as MELONDS_AUDIO_DIAGNOSTICS: exactly "1".
#if defined(_MSC_VER)
    char value[4] {};
    size_t len = 0;
    if (getenv_s(&len, value, sizeof(value), "MELONDS_RENDER_DIAGNOSTICS") != 0) return false;
    return std::strcmp(value, "1") == 0;
#else
    const char* value = std::getenv("MELONDS_RENDER_DIAGNOSTICS");
    return value && std::strcmp(value, "1") == 0;
#endif
}

inline std::uint64_t RenderCostNowNs()
{
    return (std::uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Fixed-size ring of nanosecond samples.
struct RenderCostStage
{
    static constexpr int MaxSamples = 256;

    std::uint64_t Samples[MaxSamples] {};
    int Count = 0;              // samples currently in the ring
    int Next = 0;               // next ring write position
    int IntervalSamples = 0;    // samples recorded since the last report
    std::uint64_t IntervalBytes = 0;
    std::uint64_t IntervalDropped = 0;

    void Record(std::uint64_t ns)
    {
        Samples[Next] = ns;
        Next = (Next + 1) % MaxSamples;
        if (Count < MaxSamples) ++Count;
        ++IntervalSamples;
    }
};

struct RenderCostSummary
{
    bool Any = false;
    std::uint64_t Median = 0;
    std::uint64_t P95 = 0;
};

inline RenderCostSummary RenderCostSummarize(const RenderCostStage& stage)
{
    RenderCostSummary out;
    if (!stage.Count) return out;
    std::uint64_t tmp[RenderCostStage::MaxSamples];
    std::memcpy(tmp, stage.Samples, sizeof(std::uint64_t) * stage.Count);
    std::sort(tmp, tmp + stage.Count);
    const int n = stage.Count;
    out.Any = true;
    out.Median = tmp[n / 2];
    int idx = (int)((std::int64_t)n * 95 / 100);
    if (idx >= n) idx = n - 1;
    out.P95 = tmp[idx];
    return out;
}

// Line composer writing into a caller-provided buffer; truncates safely.
struct RenderCostLine
{
    char* Buf;
    size_t Size;
    int Len = 0;

    RenderCostLine(char* buf, size_t size) : Buf(buf), Size(size) {}

    void Append(const char* fmt, ...)
    {
        if (!Size || Len < 0 || (size_t)Len >= Size - 1) return;
        va_list args;
        va_start(args, fmt);
        int written = std::vsnprintf(Buf + Len, Size - (size_t)Len, fmt, args);
        va_end(args);
        if (written > 0)
        {
            Len += written;
            if ((size_t)Len >= Size) Len = (int)Size - 1;
        }
    }

    // "median/p95" in milliseconds, or "-" when the stage has no samples.
    void Ratio(const RenderCostStage& stage)
    {
        RenderCostSummary sum = RenderCostSummarize(stage);
        if (!sum.Any) { Append("-"); return; }
        Append("%.3f/%.3f", sum.Median / 1e6, sum.P95 / 1e6);
    }

    void Bytes(std::uint64_t bytes)
    {
        if (bytes >= 1024 * 1024) Append("%.1fMB", bytes / 1048576.0);
        else Append("%.0fKB", bytes / 1024.0);
    }
};

// Vulkan host accounting is one aligned, bounded cohort of completed RunFrame
// (or explicit microbenchmark) intervals. A transition charges elapsed time to
// exactly one stage, so nested readback/conversion never inflates capture cost.
// GPU observations are a separate axis, NOT part of the host sum. Storage is
// allocated by Device only when the existing diagnostics opt-in is enabled.
class RenderCostVulkanMeter
{
public:
    enum HostStage {
        Prepare3D, TextureUpload, Record3D, RecordFullReadback, ContextCopy, RecordDisplay,
        SubmitOther, WaitOther, SubmitUpload, WaitUpload, Submit3D, Wait3D,
        SubmitFullReadback, WaitFullReadback, SubmitDisplay, WaitDisplay,
        NativeCopy, FullCopy, DisplayCopy, NativeConvert, ScaledConvert, Scan2D, Capture, CaptureCopy, NativeCapture,
        LCDC, CpuDisplay, Fallback, Diagnostic, Residual, HostCount
    };
    enum GpuStage { GpuUpload, Gpu3D, GpuNative, GpuFull, GpuCompose, GpuDisplayTransfer, GpuOther, GpuCount };
    enum Traffic {
        NativeReadbackBytes, FullReadbackBytes, DisplayReadbackBytes, NativeCopyBytes,
        FullCopyBytes, DisplayCopyBytes, ContextCopyBytes, UploadCopyBytes,
        ImageUploadBytes, OverrideUploadBytes, CaptureCopyBytes, ComposedRows, TrafficCount
    };
    static constexpr int MaxFrames = RenderCostStage::MaxSamples;
    struct Snapshot {
        std::array<std::uint64_t, HostCount> HostNs{}, HostCalls{};
        std::array<std::uint64_t, GpuCount> GpuNs{}, GpuCalls{}, GpuDropped{};
        std::array<std::uint64_t, TrafficCount> Bytes{}, Events{}, Units{};
        std::uint64_t Total = 0;
    };
    // Device updates this only for optional diagnostic capability/results.
    const char* GpuState = "unmeasured";
    std::uint64_t Abandoned = 0;

    bool Active() const { return FrameActive; }
    int Count() const { return FrameCount; }
    const Snapshot& Sum() const { return Aggregate; }
    const Snapshot& Last() const { return History[(Next + MaxFrames - 1) % MaxFrames]; }

    void Begin(std::uint64_t now)
    {
        if (FrameActive) ++Abandoned;
        Current = {};
        FrameActive = true;
        CurrentStage = Residual;
        Current.HostCalls[Residual] = 1;
        Start = Tick = now;
    }
    HostStage Enter(HostStage stage, std::uint64_t now)
    {
        const auto previous = CurrentStage;
        Charge(now);
        CurrentStage = stage;
        ++Current.HostCalls[stage];
        return previous;
    }
    void Leave(HostStage previous, std::uint64_t now)
    {
        Charge(now);
        CurrentStage = previous;
    }
    void End(std::uint64_t now)
    {
        if (!FrameActive) return;
        Charge(now);
        Current.Total = now - Start;
        FrameActive = false;
        auto& evicted = History[Next];
        // Every field uses the SAME ring, including zero-work frames. Means
        // can be reconciled with total; conditional medians cannot be summed.
        Update(Aggregate.HostNs, evicted.HostNs, Current.HostNs);
        Update(Aggregate.HostCalls, evicted.HostCalls, Current.HostCalls);
        Update(Aggregate.GpuNs, evicted.GpuNs, Current.GpuNs);
        Update(Aggregate.GpuCalls, evicted.GpuCalls, Current.GpuCalls);
        Update(Aggregate.GpuDropped, evicted.GpuDropped, Current.GpuDropped);
        Update(Aggregate.Bytes, evicted.Bytes, Current.Bytes);
        Update(Aggregate.Events, evicted.Events, Current.Events);
        Update(Aggregate.Units, evicted.Units, Current.Units);
        Aggregate.Total = Aggregate.Total - evicted.Total + Current.Total;
        evicted = Current;
        Next = (Next + 1) % MaxFrames;
        FrameCount = std::min(FrameCount + 1, MaxFrames);
        Totals.Record(Current.Total);
        if (++Frames % RenderCostReportInterval == 0) ReportDue = true;
    }
    void Transfer(Traffic kind, std::uint64_t bytes, std::uint64_t units = 0)
    {
        if (!FrameActive) return;
        Current.Bytes[kind] += bytes;
        Current.Units[kind] += units;
        ++Current.Events[kind];
    }
    void Gpu(GpuStage stage, std::uint64_t ns)
    {
        if (!FrameActive) return;
        Current.GpuNs[stage] += ns;
        ++Current.GpuCalls[stage];
    }
    void Drop(GpuStage stage)
    {
        if (FrameActive) ++Current.GpuDropped[stage];
    }
    bool TakeReport() { const bool due = ReportDue; ReportDue = false; return due; }
    void Discard() { if (FrameActive) ++Abandoned; FrameActive = false; }
    void Reset()
    {
        for (auto& frame : History) frame = {};
        Current = Aggregate = {};
        Totals = {};
        FrameCount = Next = 0;
        Frames = Abandoned = 0;
        FrameActive = ReportDue = false;
    }
    void Report(char* buffer, size_t size, const char* label) const
    {
        if (size) buffer[0] = 0;
        RenderCostLine line(buffer, size);
        line.Append("GR14 vk[%s] frames=%d abandoned=%llu", label, FrameCount, (unsigned long long)Abandoned);
        if (!FrameCount) { line.Append(" host=unmeasured gpu=%s", GpuState); return; }
        line.Append(" total_mean_ms=%.6f total_p50_p95_ms=", Aggregate.Total / (1e6 * FrameCount));
        line.Ratio(Totals);
        static constexpr const char* hosts[HostCount] = {
            "prepare3d", "texture_upload", "record3d", "record_full_rb", "context_copy", "record_display",
            "submit_other", "wait_other", "submit_upload", "wait_upload", "submit3d", "wait3d",
            "submit_full_rb", "wait_full_rb", "submit_display", "wait_display",
            "native_memcpy", "full_memcpy", "display_memcpy", "native_convert", "scaled_convert", "native2d", "capture_sidecar", "capture_memcpy",
            "capture_native", "lcdc", "cpu_display", "fallback", "diagnostic", "residual"};
        line.Append(" | host_mean_ms(calls):");
        for (int i = 0; i < HostCount; ++i)
        {
            line.Append(" %s=", hosts[i]);
            if (!Aggregate.HostCalls[i]) line.Append("unused");
            else line.Append("%.6f(%llu)", Aggregate.HostNs[i] / (1e6 * FrameCount),
                (unsigned long long)Aggregate.HostCalls[i]);
        }
        static constexpr const char* gpu[GpuCount] = {"upload", "3d", "native_extract", "full_readback", "compose", "display_readback", "other"};
        line.Append(" | gpu=%s gpu_ms_per_frame(observations,dropped):", GpuState);
        for (int i = 0; i < GpuCount; ++i)
        {
            line.Append(" %s=", gpu[i]);
            if (!Aggregate.GpuCalls[i]) line.Append("unmeasured");
            else line.Append("%.6f", Aggregate.GpuNs[i] / (1e6 * FrameCount));
            line.Append("(%llu,%llu)", (unsigned long long)Aggregate.GpuCalls[i], (unsigned long long)Aggregate.GpuDropped[i]);
        }
        static constexpr const char* traffic[TrafficCount] = {
            "native_shader_readback", "full_image_readback", "display_image_readback", "native_memcpy", "full_memcpy",
            "display_memcpy", "context_memcpy", "upload_memcpy", "image_upload", "cpu_override_upload", "capture_memcpy", "composed_rows"};
        line.Append(" | bytes_per_frame(events,units):");
        for (int i = 0; i < TrafficCount; ++i)
        {
            line.Append(" %s=%.3f(%llu,%llu)", traffic[i], double(Aggregate.Bytes[i]) / FrameCount,
                (unsigned long long)Aggregate.Events[i], (unsigned long long)Aggregate.Units[i]);
        }
    }
private:
    template<size_t N> static void Update(std::array<std::uint64_t, N>& sum,
        const std::array<std::uint64_t, N>& old, const std::array<std::uint64_t, N>& value)
    {
        for (size_t i = 0; i < N; ++i) sum[i] = sum[i] - old[i] + value[i];
    }
    void Charge(std::uint64_t now) { Current.HostNs[CurrentStage] += now - Tick; Tick = now; }
    std::array<Snapshot, MaxFrames> History{};
    Snapshot Current{}, Aggregate{};
    RenderCostStage Totals;
    HostStage CurrentStage = Residual;
    std::uint64_t Start = 0, Tick = 0, Frames = 0;
    int FrameCount = 0, Next = 0;
    bool FrameActive = false, ReportDue = false;
};

class RenderCostVulkanScope
{
public:
    RenderCostVulkanScope(RenderCostVulkanMeter* meter, RenderCostVulkanMeter::HostStage stage)
        : Meter(meter && meter->Active() ? meter : nullptr)
    {
        if (Meter) Previous = Meter->Enter(stage, RenderCostNowNs());
    }
    ~RenderCostVulkanScope() { if (Meter) Meter->Leave(Previous, RenderCostNowNs()); }
    RenderCostVulkanScope(const RenderCostVulkanScope&) = delete;
    RenderCostVulkanScope& operator=(const RenderCostVulkanScope&) = delete;
private:
    RenderCostVulkanMeter* Meter;
    RenderCostVulkanMeter::HostStage Previous = RenderCostVulkanMeter::Residual;
};
class RenderCostVulkanFrame
{
public:
    explicit RenderCostVulkanFrame(RenderCostVulkanMeter* meter)
        : Meter(meter && !meter->Active() ? meter : nullptr)
    {
        if (Meter) Meter->Begin(RenderCostNowNs());
    }
    ~RenderCostVulkanFrame() { if (Meter) Meter->End(RenderCostNowNs()); }
    RenderCostVulkanFrame(const RenderCostVulkanFrame&) = delete;
    RenderCostVulkanFrame& operator=(const RenderCostVulkanFrame&) = delete;
private:
    RenderCostVulkanMeter* Meter;
};

#if defined(PLATFORMOGL_H)
// Timestamp pairs belong to one context. Query names are opaque; slots use
// array indices separately. No elapsed-query target is left active, so nested
// rendering spans and another profiler cannot end one another's queries.
class RenderCostGpuSpans
{
public:
    static constexpr int MaxPending = 32;
    bool Enabled = false;

    int Begin(RenderCostStage& stage)
    {
        if (!Enabled || !Init()) return -1;
        for (int i = 0; i < MaxPending; ++i)
        {
            if (Pending[i].Stage) continue;
            Pending[i] = {&stage, false};
            glQueryCounter(Queries[2 * i], GL_TIMESTAMP);
            return i;
        }
        ++stage.IntervalDropped;
        return -1;
    }

    void End(int slot)
    {
        if (slot < 0) return;
        glQueryCounter(Queries[2 * slot + 1], GL_TIMESTAMP);
        Pending[slot].Ended = true;
    }

    void Poll()
    {
        if (!Enabled || !Created) return;
        for (int i = 0; i < MaxPending; ++i)
        {
            if (!Pending[i].Ended) continue;
            GLuint available = 0;
            glGetQueryObjectuiv(Queries[2 * i + 1], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) continue;
            GLuint64 begin = 0, end = 0;
            glGetQueryObjectui64v(Queries[2 * i], GL_QUERY_RESULT, &begin);
            glGetQueryObjectui64v(Queries[2 * i + 1], GL_QUERY_RESULT, &end);
            if (end >= begin) Pending[i].Stage->Record(end - begin);
            else ++Pending[i].Stage->IntervalDropped;
            Pending[i] = {};
        }
    }

    // Called only with the owning context current, like the renderer's other
    // GL resource teardown. A failed MakeCurrent leaves these IDs owned until
    // a later successful teardown, rather than leaking a pool on each retry.
    void Shutdown()
    {
        if (Created) glDeleteQueries(2 * MaxPending, Queries);
        for (auto& pending : Pending) pending = {};
        Created = Initialized = TimerSupported = false;
    }

    const char* State() const
    {
        if (!Initialized) return "unmeasured";
        return TimerSupported ? "on" : "unsup";
    }

private:
    bool Init()
    {
        if (Initialized) return Created;
        Initialized = true;
        // glad loads these entry points with GL 3.3. GL 3.2-only contexts
        // remain CPU-only even if an unloaded extension supports timers.
        if (!GLAD_GL_VERSION_3_3 || !glQueryCounter || !glGetQueryObjectui64v)
            return false;
        GLint bits = 0;
        glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
        if (!bits) return false;
        glGenQueries(2 * MaxPending, Queries);
        for (GLuint query : Queries)
        {
            if (query) continue;
            glDeleteQueries(2 * MaxPending, Queries);
            return false;
        }
        TimerSupported = Created = true;
        return true;
    }

    struct PendingSpan
    {
        RenderCostStage* Stage = nullptr;
        bool Ended = false;
    };
    GLuint Queries[2 * MaxPending] {};
    PendingSpan Pending[MaxPending] {};
    bool Created = false, Initialized = false, TimerSupported = false;
};

// Renderer-side meter (GLRenderer). Frame = DrawScanline(0) .. VBlank.
class RenderCostMeter
{
public:
    bool Enabled = false;

    void SetEnabled(bool on) { Enabled = on; Gpu.Enabled = on; }

    // Per-frame CPU accumulators (ns).
    std::uint64_t AccScan2D = 0, AccSprites = 0, Acc3D = 0;
    std::uint64_t AccFinalPass = 0, AccUpload = 0;  // upload is nested in the final pass
    std::uint64_t AccCapture = 0, AccReadback = 0;
    std::uint64_t UploadBytes = 0, ReadbackBytes = 0;
    bool UploadRan = false, CaptureRan = false, ReadbackRan = false;

    RenderCostStage Scan2D, Sprites, ThreeD, FinalPass, Capture, Upload, Readback;
    RenderCostStage Gpu3D, GpuFinalPass, GpuCapture;
    RenderCostGpuSpans Gpu;

    std::uint64_t Frames = 0;
    std::uint64_t AbandonedFrames = 0;
    bool FrameActive = false;
    bool ReportDue = false;

    void FrameBegin()
    {
        if (!Enabled) return;
        Gpu.Poll();
        if (FrameActive)
        {
            // The previous frame never reached VBlank (reset/abort); its
            // partial costs are not a full-frame sample.
            ++AbandonedFrames;
            ClearFrame();
        }
        FrameActive = true;
    }

    void FrameEnd()
    {
        if (!Enabled) return;
        FrameActive = false;
        Gpu.Poll();

        Scan2D.Record(AccScan2D);
        Sprites.Record(AccSprites);
        ThreeD.Record(Acc3D);
        FinalPass.Record(AccFinalPass > AccUpload ? AccFinalPass - AccUpload : 0);
        if (UploadRan) { Upload.Record(AccUpload); Upload.IntervalBytes += UploadBytes; }
        if (CaptureRan) Capture.Record(AccCapture);
        if (ReadbackRan) { Readback.Record(AccReadback); Readback.IntervalBytes += ReadbackBytes; }

        ++Frames;
        if ((Frames % RenderCostReportInterval) == 0) ReportDue = true;
        ClearFrame();
    }

    bool TakeReport() { bool due = ReportDue; ReportDue = false; return due; }

    // Timing helpers: a zero start timestamp is a no-op (disabled).
    std::uint64_t Start() const { return Enabled ? RenderCostNowNs() : 0; }
    static void Add(std::uint64_t& acc, std::uint64_t start)
    {
        if (start) acc += RenderCostNowNs() - start;
    }
    std::uint64_t UploadStart() const { return Start(); }
    void UploadEnd(std::uint64_t start, std::uint64_t bytes)
    {
        if (!start) return;
        AccUpload += RenderCostNowNs() - start;
        UploadBytes += bytes;
        UploadRan = true;
    }
    std::uint64_t ReadbackStart() const { return Start(); }
    void ReadbackEnd(std::uint64_t start, std::uint64_t bytes)
    {
        if (!start) return;
        AccReadback += RenderCostNowNs() - start;
        ReadbackBytes += bytes;
        ReadbackRan = true;
    }

    // Composes the report line and resets interval counters. Percentiles are
    // milliseconds (median/p95) over the bounded ring.
    void Report(char* buf, size_t size)
    {
        RenderCostLine line(buf, size);
        line.Append("GR14 render[%d frames]:", Scan2D.Count);
        line.Append(" 2d=");  line.Ratio(Scan2D);   line.Append("(n=%d)", Scan2D.IntervalSamples);
        line.Append(" spr="); line.Ratio(Sprites);  line.Append("(n=%d)", Sprites.IntervalSamples);
        line.Append(" 3d="); line.Ratio(ThreeD); line.Append("(n=%d)", ThreeD.IntervalSamples);
        line.Append(" fp=");  line.Ratio(FinalPass); line.Append("(n=%d)", FinalPass.IntervalSamples);
        line.Append(" cap="); line.Ratio(Capture);  line.Append("(n=%d)", Capture.IntervalSamples);
        line.Append(" upl="); line.Ratio(Upload);   line.Append("(n=%d,", Upload.IntervalSamples);
        line.Bytes(Upload.IntervalBytes); line.Append(")");
        line.Append(" rb=");  line.Ratio(Readback); line.Append("(n=%d,", Readback.IntervalSamples);
        line.Bytes(Readback.IntervalBytes); line.Append(")");
        line.Append(" | gpu3d="); line.Ratio(Gpu3D);        line.Append("(n=%d)", Gpu3D.IntervalSamples);
        line.Append(" gpufp=");   line.Ratio(GpuFinalPass); line.Append("(n=%d)", GpuFinalPass.IntervalSamples);
        line.Append(" gpucap=");  line.Ratio(GpuCapture);   line.Append("(n=%d)", GpuCapture.IntervalSamples);
        line.Append(" gpudrop=%llu gpu=%s",
            (unsigned long long)(Gpu3D.IntervalDropped + GpuFinalPass.IntervalDropped + GpuCapture.IntervalDropped),
            Gpu.State());

        ResetIntervals();
    }

private:
    void ClearFrame()
    {
        AccScan2D = AccSprites = Acc3D = AccFinalPass = AccUpload = AccCapture = AccReadback = 0;
        UploadBytes = ReadbackBytes = 0;
        UploadRan = CaptureRan = ReadbackRan = false;
    }

    void ResetIntervals()
    {
        for (RenderCostStage* s : {&Scan2D, &Sprites, &ThreeD, &FinalPass, &Capture, &Upload, &Readback,
                                   &Gpu3D, &GpuFinalPass, &GpuCapture})
        {
            s->IntervalSamples = 0;
            s->IntervalBytes = 0;
            s->IntervalDropped = 0;
        }
    }
};

// GL panel meter: one sample set per drawScreen call.
class RenderCostPresentMeter
{
public:
    bool Enabled = false;

    void SetEnabled(bool on) { Enabled = on; Gpu.Enabled = on; }

    std::uint64_t AccIssue = 0, AccWait = 0, AccUpload = 0;  // upload is nested in issue
    std::uint64_t UploadBytes = 0;
    bool UploadRan = false;

    RenderCostStage Issue, Wait, Upload, GpuPresent;
    RenderCostGpuSpans Gpu;

    std::uint64_t Frames = 0;
    bool ReportDue = false;

    void FrameBegin()
    {
        if (!Enabled) return;
        Gpu.Poll();
        ClearFrame();
    }

    void FrameEnd()
    {
        if (!Enabled) return;
        Issue.Record(AccIssue > AccUpload ? AccIssue - AccUpload : 0);
        Wait.Record(AccWait);
        if (UploadRan) { Upload.Record(AccUpload); Upload.IntervalBytes += UploadBytes; }
        ++Frames;
        if ((Frames % RenderCostReportInterval) == 0) ReportDue = true;
        ClearFrame();
    }

    bool TakeReport() { bool due = ReportDue; ReportDue = false; return due; }

    std::uint64_t Start() const { return Enabled ? RenderCostNowNs() : 0; }
    static void Add(std::uint64_t& acc, std::uint64_t start)
    {
        if (start) acc += RenderCostNowNs() - start;
    }
    std::uint64_t UploadStart() const { return Start(); }
    void UploadEnd(std::uint64_t start, std::uint64_t bytes)
    {
        if (!start) return;
        AccUpload += RenderCostNowNs() - start;
        UploadBytes += bytes;
        UploadRan = true;
    }

    void Report(char* buf, size_t size, unsigned windowId)
    {
        RenderCostLine line(buf, size);
        line.Append("GR14 present[%d frames win%u]:", Wait.Count, windowId);
        line.Append(" issue="); line.Ratio(Issue); line.Append("(n=%d)", Issue.IntervalSamples);
        line.Append(" wait=");  line.Ratio(Wait);  line.Append("(n=%d)", Wait.IntervalSamples);
        // Present stalls: frames whose wait exceeded 1.5x the median wait
        // within the same ring.
        int stalls = 0;
        if (Wait.Count)
        {
            std::uint64_t med = RenderCostSummarize(Wait).Median;
            std::uint64_t threshold = med + med / 2;
            for (int i = 0; i < Wait.Count; ++i)
                if (Wait.Samples[i] > threshold) ++stalls;
        }
        line.Append(" stalls=%d", stalls);
        line.Append(" upl="); line.Ratio(Upload); line.Append("(n=%d,", Upload.IntervalSamples);
        line.Bytes(Upload.IntervalBytes); line.Append(")");
        line.Append(" gpupres="); line.Ratio(GpuPresent); line.Append("(n=%d)", GpuPresent.IntervalSamples);
        line.Append(" gpudrop=%llu gpu=%s",
            (unsigned long long)GpuPresent.IntervalDropped, Gpu.State());

        ResetIntervals();
    }

private:
    void ClearFrame()
    {
        AccIssue = AccWait = AccUpload = 0;
        UploadBytes = 0;
        UploadRan = false;
    }

    void ResetIntervals()
    {
        for (RenderCostStage* s : {&Issue, &Wait, &Upload, &GpuPresent})
        {
            s->IntervalSamples = 0;
            s->IntervalBytes = 0;
            s->IntervalDropped = 0;
        }
    }
};

#endif // PLATFORMOGL_H: GL meters never enter a Vulkan-only/headless dependency.

// Native paint uses disjoint host spans. Presenter details are nested within
// PresentNs and must not be added to paint totals a second time. Neither axis
// measures GPU/display completion. Existing Copy history remains conditional.
struct RenderCostNativeMeter
{
    bool Enabled = false;
    RenderCostStage Copy;
    std::uint64_t Frames = 0, Paints = 0, LastGeneration = 0;
    std::uint64_t IntervalPaints = 0, GenerationChanges = 0;
    std::uint64_t PaintNs = 0, CopyNs = 0, PainterNs = 0, PresentNs = 0;
    std::uint64_t ActualCalls = 0, Submits = 0, Skips = 0, Failures = 0;
    std::uint64_t StagingBytes = 0, TransferBytes = 0, UploadNs = 0, SubmitNs = 0, QueuePresentNs = 0;
    unsigned WindowWidth = 0, WindowHeight = 0;
    bool ReportDue = false, PresenterMeasured = false;

    std::uint64_t Start() const { return Enabled ? RenderCostNowNs() : 0; }
    void Window(unsigned width, unsigned height)
    {
        if (Enabled) { WindowWidth = width; WindowHeight = height; }
    }
    void RecordPainter(std::uint64_t start)
    {
        if (start) PainterNs += RenderCostNowNs() - start;
    }
    void RecordCopy(std::uint64_t start, std::uint64_t bytes = 0, std::uint64_t generation = 0)
    {
        if (!start) return;
        const auto elapsed = RenderCostNowNs() - start;
        Copy.Record(elapsed);
        Copy.IntervalBytes += bytes;
        CopyNs += elapsed;
        GenerationChanges += generation != LastGeneration;
        LastGeneration = generation;
        ++Frames;
    }
    template<class Counters> void RecordPresent(std::uint64_t start, unsigned width, unsigned height,
        const Counters& before, const Counters& after)
    {
        if (!start) return;
        PresentNs += RenderCostNowNs() - start;
        Window(width, height);
        PresenterMeasured |= after.Enabled;
        ActualCalls += after.Calls - before.Calls;
        Submits += after.Submits - before.Submits;
        Skips += after.Skips - before.Skips;
        Failures += after.Failures - before.Failures;
        StagingBytes += after.StagingBytes - before.StagingBytes;
        TransferBytes += after.TransferBytes - before.TransferBytes;
        UploadNs += after.UploadNs - before.UploadNs;
        SubmitNs += after.SubmitNs - before.SubmitNs;
        QueuePresentNs += after.QueuePresentNs - before.QueuePresentNs;
    }
    void PaintEnd(std::uint64_t start)
    {
        if (!start) return;
        PaintNs += RenderCostNowNs() - start;
        ++IntervalPaints;
        if (++Paints % RenderCostReportInterval == 0) ReportDue = true;
    }
    bool TakeReport() { bool due = ReportDue; ReportDue = false; return due; }
    void Report(char* buf, size_t size, unsigned windowId)
    {
        if (size) buf[0] = 0;
        RenderCostLine line(buf, size);
        line.Append("GR14 present-native[win%u] paints=%llu copies=%d window_pixels=%ux%u generation=%llu changes=%llu",
            windowId, (unsigned long long)IntervalPaints, Copy.IntervalSamples, WindowWidth, WindowHeight,
            (unsigned long long)LastGeneration, (unsigned long long)GenerationChanges);
        if (IntervalPaints)
        {
            const double denominator = 1e6 * IntervalPaints;
            line.Append(" host_mean_ms: paint=%.6f qimage_copy=%.6f qpaint_setup_draw=%.6f present_return=%.6f residual=%.6f",
                PaintNs / denominator, CopyNs / denominator, PainterNs / denominator, PresentNs / denominator,
                (double(PaintNs) - CopyNs - PainterNs - PresentNs) / denominator);
        }
        line.Append(" copy_bytes=%llu copy_conditional_p50_p95_ms=", (unsigned long long)Copy.IntervalBytes);
        line.Ratio(Copy);
        if (PresenterMeasured)
        {
            line.Append(" present_calls=%llu actual_submits=%llu skipped_returns=%llu failed_returns=%llu staging_bytes=%llu submitted_bytes=%llu",
                (unsigned long long)ActualCalls, (unsigned long long)Submits, (unsigned long long)Skips,
                (unsigned long long)Failures, (unsigned long long)StagingBytes, (unsigned long long)TransferBytes);
            if (ActualCalls) line.Append(" present_detail_ms_per_call: staging_copy=%.6f submit_api=%.6f queue_present_api=%.6f",
                UploadNs / (1e6 * ActualCalls), SubmitNs / (1e6 * ActualCalls), QueuePresentNs / (1e6 * ActualCalls));
        }
        else line.Append(" presenter=unmeasured");
        line.Append(" gpu_completion=unmeasured");
        Copy.IntervalSamples = 0;
        Copy.IntervalBytes = 0;
        IntervalPaints = GenerationChanges = PaintNs = CopyNs = PainterNs = PresentNs = 0;
        ActualCalls = Submits = Skips = Failures = StagingBytes = TransferBytes = 0;
        UploadNs = SubmitNs = QueuePresentNs = 0;
        PresenterMeasured = false;
    }
};

}

#endif // RENDER_COST_H
