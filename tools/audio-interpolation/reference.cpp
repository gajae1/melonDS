// SPDX-License-Identifier: GPL-3.0-or-later
// Offline r8brain reference generation; never linked into the emulator.
#include "CDSPFIRFilter.h"
#include "AudioInterpolationBank.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

static void Word(std::ostream& out, uint32_t word)
{
    for (unsigned i = 0; i < 4; ++i) out.put(char(word >> (i * 8)));
}

static void Value(std::ostream& out, double value)
{
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite reference value");
    const auto bits = std::bit_cast<uint64_t>(value);
    Word(out, uint32_t(bits));
    Word(out, uint32_t(bits >> 32));
}

static std::vector<double> Step(unsigned period, unsigned mix, double& maxAdjustment)
{
    const unsigned grid = std::min(period, 256u);
    auto& filter = r8b::CDSPFIRFilterCache::getLPFilter(
        1.0 / (grid * std::max(1.0, double(mix) / period)),
        8, 96, r8b::fprMinPhase, grid, 1);
    r8b::CDSPRealFFTKeeper fft(filter.getBlockLenBits() + 1);
    std::vector<double> taps(fft->getLen());
    std::copy_n(filter.getKernelBlock(), taps.size(), taps.data());
    fft->inverse(taps.data());
    taps.resize(filter.getKernelLen());
    filter.unref();

    std::vector<double> gains(grid, 0);
    for (size_t i = 0; i < taps.size(); ++i) gains[i % grid] += taps[i];
    for (double gain : gains)
    {
        maxAdjustment = std::max(maxAdjustment, std::abs(gain - 1));
        if (!std::isfinite(gain) || std::abs(gain - 1) > 1e-4)
            throw std::runtime_error("Polyphase DC correction exceeds the reference budget");
    }
    std::vector<double> response(taps.size() + grid);
    for (size_t i = 0; i < response.size(); ++i)
        response[i] = (i < taps.size() ? taps[i] / gains[i % grid] : 0)
                    + (i >= grid ? response[i - grid] : 0);
    return response;
}

static void Generate(unsigned mix, const std::filesystem::path& path)
{
    if ((mix != 352 && mix != 512) || std::filesystem::exists(path))
        throw std::runtime_error("Invalid interval or output already exists");
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create reference output");
    Word(out, 0x31524648); Word(out, mix); Word(out, mix);
    double maxAdjustment = 0;
    for (unsigned period = 1; period <= mix; ++period)
    {
        const auto response = Step(period, mix, maxAdjustment);
        Word(out, period); Word(out, uint32_t(response.size()));
        for (double value : response) Value(out, value);
    }
    out.close();
    if (!out) throw std::runtime_error("Reference write failed");
    std::cout << "{\"mix\":" << mix << ",\"max_phase_dc_adjustment\":" << maxAdjustment << "}\n";
}

static auto ReadBank(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Missing coefficient bank");
    std::vector<melonDS::u8> bytes{std::istreambuf_iterator<char>(in), {}};
    return melonDS::AudioInterpolationBank::Create(bytes);
}

static void Compare(const std::filesystem::path& generated, const std::filesystem::path& frozen)
{
    const auto a = ReadBank(generated), b = ReadBank(frozen);
    const unsigned mix = a->MixInterval();
    if (mix != b->MixInterval()) throw std::runtime_error("Mismatched bank intervals");
    double moments = 0, steps = 0;
    for (unsigned period = 1; period <= mix; ++period)
    {
        if (a->SupportClocks(period) != b->SupportClocks(period))
            throw std::runtime_error("Support length changed");
        const auto ac = a->Coefficients(period), bc = b->Coefficients(period);
        if (ac.size() != bc.size()) throw std::runtime_error("Moment count changed");
        for (size_t row = 0; row < ac.size(); ++row)
        {
            double bound = 0;
            for (unsigned k = 0; k < 16; ++k) bound += std::abs(ac[row][k] - bc[row][k]);
            moments = std::max(moments, bound);
        }
        if (period > 256)
            for (uint64_t age = 0; age <= a->SupportClocks(period); ++age)
                steps = std::max(steps, std::abs(a->Step(period, age) - b->Step(period, age)));
    }
    // This is a regeneration-difference budget, not the filter's fitting error
    // or a bound on complete audio streams. The production bank stays frozen.
    std::cout << "{\"mix\":" << mix << ",\"max_moment_l1_delta\":" << moments
              << ",\"max_loaded_step_delta\":" << steps << "}\n";
    if (moments > 1e-12 || steps > 1e-12)
        throw std::runtime_error("Regeneration differs from the frozen bank beyond 1e-12");
}

int main(int argc, char** argv) try
{
    std::cout << std::setprecision(17);
    if (argc != 4) throw std::runtime_error("Usage: reference kernels MIX OUTPUT | compare GENERATED FROZEN");
    if (std::string(argv[1]) == "kernels") Generate(unsigned(std::stoul(argv[2])), argv[3]);
    else if (std::string(argv[1]) == "compare") Compare(argv[2], argv[3]);
    else throw std::runtime_error("Unknown operation");
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
