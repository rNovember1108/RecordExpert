#include "audio_source_factory.h"

#include <algorithm>
#include <cctype>

#include "dshow_audio_source.h"
#include "mixed_audio_source.h"

namespace re {
namespace {

// 把后端名归一化为小写 ASCII，使 "DSHOW"/"DShow" 等写法都能命中。
std::string Normalize(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return s;
}

}  // namespace

std::unique_ptr<IAudioSource> AudioSourceFactory::Create(
    const std::string& backend, const AudioOptions& options) {
    const std::string name = Normalize(backend);

    if (name == kDShow) {
        return std::make_unique<DShowAudioSource>(options);
    }

    if (name == kMixed) {
        return std::make_unique<MixedAudioSource>(options);
    }

    return nullptr;
}

}  // namespace re
