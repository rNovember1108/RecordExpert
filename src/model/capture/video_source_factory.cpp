#include "video_source_factory.h"

#include <algorithm>
#include <cctype>

#include "composite_video_source.h"
#include "dshow_video_source.h"
#include "gdi_grab_video_source.h"
#include "Mp4Writer.h"

namespace re {
namespace {

// 把后端名归一化为小写 ASCII，使 "GDIGRAB"/"GdiGrab" 等写法都能命中。
std::string Normalize(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return s;
}

}  // namespace

std::unique_ptr<IVideoSource> VideoSourceFactory::Create(
    const std::string& backend, const VideoOptions& options, Mp4Writer* writer) {
    const std::string name = Normalize(backend);

    if (name == kGdiGrab) {
        return std::make_unique<GdiGrabVideoSource>(options, writer);
    }

    if (name == kDShow) {
        return std::make_unique<DShowVideoSource>(options, writer);
    }

    if (name == kComposite) {
        return std::make_unique<CompositeVideoSource>(options, writer);
    }

    return nullptr;
}

}  // namespace re
