#include "Watermark.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QString>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace re {
namespace {

// 水印字体（微软雅黑），字号按 scale*8 像素取值。
QFont WatermarkFont(int scale) {
    QFont f(QStringLiteral("Microsoft YaHei"));
    f.setPixelSize(scale * 8);
    return f;
}

// 把 BGRX 打包帧零拷贝包裹为 QImage。Format_RGB32 与 BGRA/BGR0 内存布局
// 一致（小端 32 位字 0xXRRGGBB）；QImage 不持有像素缓冲，QPainter 绘制直接
// 写入 frame->data[0]，无任何像素拷贝。非 BGRX 格式返回空 QImage，调用方
// 跳过绘制。
QImage WrapFrame(const AVFrame* frame) {
    const AVPixelFormat fmt = static_cast<AVPixelFormat>(frame->format);
    if (!frame->data[0] ||
        (fmt != AV_PIX_FMT_BGRA && fmt != AV_PIX_FMT_BGR0)) {
        return QImage();
    }
    return QImage(frame->data[0], frame->width, frame->height,
                  frame->linesize[0], QImage::Format_RGB32);
}

}  // namespace

// ---- QImage 版本（核心实现）----

void DrawTextOnImage(QImage* img, const char* text, int x, int y,
                     int scale, uint8_t r, uint8_t g, uint8_t b,
                     bool draw_shadow) {
    if (!img || img->isNull() || !text || scale < 1) return;

    QPainter p(img);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    const QFont f = WatermarkFont(scale);
    p.setFont(f);
    // drawText(x, y, s) 的 y 是基线；接口语义为"文字左上角"（cell 顶部），
    // 故换算 baseline = y + ascent。
    const int baseline = y + QFontMetrics(f).ascent();
    const QString s = QString::fromUtf8(text);
    // 先黑色阴影（右下偏移 1 像素），再正文——与区域截图水印同款画法。
    if (draw_shadow) {
        p.setPen(QColor(0, 0, 0));
        p.drawText(x + 1, baseline + 1, s);
    }
    p.setPen(QColor(r, g, b));
    p.drawText(x, baseline, s);
    p.end();
}

void DrawTimestampWatermark(QImage* img, int margin, int scale) {
    if (!img || img->isNull()) return;
    if (scale < 1) scale = 1;

    const QFont f = WatermarkFont(scale);
    const QFontMetrics fm(f);
    const QString ts = QDateTime::currentDateTime()
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    // 按真实渲染宽度定位右下角（margin 边距），换算基线。
    const int x = img->width() - fm.horizontalAdvance(ts) - margin;
    const int y_top = img->height() - fm.height() - margin;

    QPainter p(img);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setFont(f);
    p.setPen(QColor(0, 0, 0));
    p.drawText(x + 1, y_top + fm.ascent() + 1, ts);
    p.setPen(QColor(255, 255, 255));
    p.drawText(x, y_top + fm.ascent(), ts);
    p.end();
}

void DrawLabelWatermark(QImage* img, const char* text,
                        int margin, int scale) {
    if (!img || img->isNull() || !text) return;
    if (scale < 1) scale = 1;

    // 左上角，留 margin 边距（白色带阴影）。
    DrawTextOnImage(img, text, margin, margin, scale, 255, 255, 255, true);
}

// ---- AVFrame 版本（零拷贝包裹后转发）----

void DrawTimestampWatermark(AVFrame* frame, int margin, int scale) {
    QImage img = WrapFrame(frame);
    DrawTimestampWatermark(&img, margin, scale);
}

void DrawLabelWatermark(AVFrame* frame, const char* text,
                        int margin, int scale) {
    QImage img = WrapFrame(frame);
    DrawLabelWatermark(&img, text, margin, scale);
}

}  // namespace re
