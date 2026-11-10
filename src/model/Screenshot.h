#ifndef RECORD_EXPERT_SCREENSHOT_H
#define RECORD_EXPERT_SCREENSHOT_H

#include <string>

#include <QImage>

struct AVFrame;

namespace re {

// 截图选项：与录屏共用同一套桌面区域参数（-r x,y,w,h）。
struct ScreenshotOptions {
    // 采集区域左上角在桌面上的坐标。
    int offset_x = 0;
    int offset_y = 0;
    // 采集区域尺寸，<=0 表示采集整个主屏。
    int width = 0;
    int height = 0;
    // 在右下角绘制时间戳水印（与录屏视频帧同款，白字黑影）。
    bool draw_timestamp = true;
};

// 抓取一帧桌面（gdigrab 采集 + 解码第一帧），不做水印/编码。
// 输出参数 *out_frame：成功后指向新分配的 AVFrame（调用方负责
// av_frame_free）。失败返回 false，*out_frame 不被修改。
// 与 CapturePngScreenshot 共用 gdigrab 打开/解码逻辑，后者为
// CaptureDesktopFrame + 水印 + FrameToQImage + QImage::save。
bool CaptureDesktopFrame(AVFrame** out_frame,
                         const ScreenshotOptions& opts);

// 把 gdigrab 的 BGRX 打包帧（AV_PIX_FMT_BGRA/BGR0）转成 QImage。
// 零拷贝构造 Format_RGB32 后立即 .copy() 持有像素，使返回的 QImage
// 生命周期独立于 AVFrame（调用方随后可立即 av_frame_free）。
// 失败返回空 QImage（QImage::isNull() 为 true）。
QImage FrameToQImage(const AVFrame* frame);

// 抓取一帧桌面并保存为 PNG（CaptureDesktopFrame + 时间戳水印 → FrameToQImage
// → QImage::save 落盘）。PNG 编码内建于 Qt5Gui，Windows/Linux 共用同一实现，
// 无需系统编码组件；gdigrab 的 BGRX 帧经零拷贝构造 + 深拷贝转 QImage，
// 无需 swscale 转换。运行在调用方线程上，抓一帧即返回。
// 失败返回 false。
bool CapturePngScreenshot(const std::string& png_path,
                          const ScreenshotOptions& opts);

}  // namespace re

#endif  // RECORD_EXPERT_SCREENSHOT_H
