#ifndef RE_CAPTURE_AUDIO_SOURCE_FACTORY_H
#define RE_CAPTURE_AUDIO_SOURCE_FACTORY_H

#include <memory>
#include <string>

#include "i_audio_source.h"

namespace re {

class Mp4Writer;

// AudioSourceFactory 按运行时后端名创建具体的 IAudioSource（简单工厂 + 策略模式）。
// 与 VideoSourceFactory 同构：协调器只依赖抽象接口，切换/新增音频后端收敛在本工厂
// 一处。后端名来自 RecorderOptions.audio_backend（运行时参数，无需重编）。
class AudioSourceFactory {
public:
    // 已登记的音频后端名。
    static constexpr const char* kDShow = "dshow";  // FFmpeg DirectShow 麦克风采集。
    static constexpr const char* kMixed = "mixed";  // 麦克风+系统声音混音。

    // 按 backend 名创建音频源；名称大小写不敏感。未登记的后端返回 nullptr。
    // writer 为所有源共享的输出上下文，生命周期由调用方保证长于返回对象。
    static std::unique_ptr<IAudioSource> Create(const std::string& backend,
                                                const AudioOptions& options,
                                                Mp4Writer* writer);
};

}  // namespace re

#endif  // RE_CAPTURE_AUDIO_SOURCE_FACTORY_H
