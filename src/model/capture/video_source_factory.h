#ifndef RE_CAPTURE_VIDEO_SOURCE_FACTORY_H
#define RE_CAPTURE_VIDEO_SOURCE_FACTORY_H

#include <memory>
#include <string>

#include "i_video_source.h"

namespace re {

// VideoSourceFactory 按运行时后端名创建具体的 IVideoSource（简单工厂 + 策略模式）。
// 协调器 ScreenRecorder 只依赖抽象接口，不感知任何具体采集方案；切换/新增后端都
// 收敛在本工厂一处。后端名来自 RecorderOptions.video_backend（运行时参数，无需重编）。
//
// 方案 C：Source 完全不依赖 Mp4Writer（encoder 全程持有，不重建），因此 Create
// 不再传 writer 指针。Open 完成后由 ScreenRecorder 统一调 SetQueue / SetStream /
// SetFlushDoneFlag 注入 muxer 相关资源。
class VideoSourceFactory {
public:
    // 已登记的视频后端名。
    static constexpr const char* kGdiGrab = "gdigrab";  // FFmpeg gdigrab 桌面抓屏。
    static constexpr const char* kDShow   = "dshow";    // FFmpeg dshow 摄像头/采集卡。
    static constexpr const char* kComposite = "composite";  // 桌面+摄像头画中画。

    // 按 backend 名创建视频源；名称大小写不敏感。未登记的后端返回 nullptr。
    static std::unique_ptr<IVideoSource> Create(const std::string& backend,
                                                const VideoOptions& options);
};

}  // namespace re

#endif  // RE_CAPTURE_VIDEO_SOURCE_FACTORY_H
