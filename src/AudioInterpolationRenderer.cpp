// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioInterpolationRenderer.h"
#include <mutex>

namespace melonDS
{
extern const unsigned char AudioInterpolation352[], AudioInterpolation512[];
extern const std::size_t AudioInterpolation352Size, AudioInterpolation512Size;

std::unique_ptr<AudioInterpolationRenderer> AudioInterpolationRenderer::Prepare()
{
    static std::mutex mutex;
    static std::array<std::weak_ptr<const AudioInterpolationBank>, 2> cached;
    std::array<std::shared_ptr<const AudioInterpolationBank>, 2> banks;
    {
        const std::lock_guard lock(mutex);
        const std::array<std::span<const u8>, 2> data{{
            {AudioInterpolation352, AudioInterpolation352Size},
            {AudioInterpolation512, AudioInterpolation512Size}}};
        for (unsigned i = 0; i < banks.size(); ++i)
        {
            banks[i] = cached[i].lock();
            if (!banks[i])
            {
                banks[i] = AudioInterpolationBank::Create(data[i]);
                cached[i] = banks[i];
            }
        }
    }
    return std::make_unique<AudioInterpolationRenderer>(std::move(banks));
}
}
