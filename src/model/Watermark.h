#ifndef RECORD_EXPERT_WATERMARK_H
#define RECORD_EXPERT_WATERMARK_H

#include <cstdint>

struct QImage;

// 前置声明 FFmpeg 类型。
struct AVFrame;

namespace re {

// 水印标签文本：录制与截图共用。修改此常量即可同时作用于录屏（GdiGrab
// VideoSource）、混合录制（CompositeVideoSource）、全屏截图
// （CapturePngScreenshot）与区域截图（ScreenshotService::CropRegion）四处水印。
inline constexpr char kWatermarkLabel[] = "test";

// ---- QImage 版本（核心实现，所有水印路径共用）----

// 在 QImage 上绘制文字水印。img 须非空且可写。text 为 UTF-8 编码（支持中文）。
// x, y 为文字左上角坐标；scale 控制字号（scale*8 像素）。draw_shadow=true
// 时在文字右下方画 1 像素黑色阴影以增强在任意背景上的可读性。
void DrawTextOnImage(QImage* img, const char* text, int x, int y,
                     int scale, uint8_t r, uint8_t g, uint8_t b,
                     bool draw_shadow = true);

// 在图像的右下角绘制当前系统时间戳水印（格式 YYYY-MM-DD HH:MM:SS，
// 白色带阴影）。
void DrawTimestampWatermark(QImage* img, int margin = 10, int scale = 2);

// 在图像的左上角绘制固定文字水印（白色带阴影）。
void DrawLabelWatermark(QImage* img, const char* text,
                        int margin = 10, int scale = 2);

// ---- AVFrame 版本（零拷贝包裹为 QImage 后转发到上面的核心实现）----
// frame 须为 32bpp BGRX 打包格式（AV_PIX_FMT_BGRA/BGR0，gdigrab 原生输出），
// 实现直接写原帧内存，无像素拷贝；其他格式跳过绘制。

// 在帧的右下角绘制当前系统时间戳水印（格式 YYYY-MM-DD HH:MM:SS，白色带阴影）。
void DrawTimestampWatermark(AVFrame* frame, int margin = 10, int scale = 2);

// 在帧的左上角绘制固定文字水印（白色带阴影）。
void DrawLabelWatermark(AVFrame* frame, const char* text,
                        int margin = 10, int scale = 2);

}  // namespace re

#endif  // RECORD_EXPERT_WATERMARK_H
