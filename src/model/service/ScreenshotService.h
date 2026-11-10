#ifndef RECORD_EXPERT_SCREENSHOT_SERVICE_H
#define RECORD_EXPERT_SCREENSHOT_SERVICE_H

#include <QImage>
#include <QObject>
#include <QString>

#include "Screenshot.h"

class QQuickWindow;

namespace re {

class ScreenshotImageProvider;

// ScreenshotService 封装截图相关的全部业务逻辑。
//
// 截图功能天然分层:
//   ┌─────────────────────────────────────────────────┐
//   │ 静态纯函数层(无 Qt 窗口依赖,可独立单测)         │
//   │ TakeFullscreenPng / CaptureFrame / CropRegion  │
//   ├─────────────────────────────────────────────────┤
//   │ 实例方法层(依赖注入的窗口/Provider)             │
//   │ StartRegionSession / CompleteSave / CopyRegion │
//   └─────────────────────────────────────────────────┘
//
// 继承 QObject 的唯一理由:StartRegionSession 用 QTimer::singleShot 带
// this 指针的 lambda,让 QObject 生命周期与 timer 安全关联——Service
// 析构时 Qt 自动取消所有挂起的 singleShot 定时器,避免 lambda 访问悬空 this。
//
// 全屏截图和区域截图底层 100% 共用 gdigrab 抓帧/水印/PNG 保存逻辑,
// 拆成两个独立 Service 会导致底层函数被复制,维护成本翻倍。本类统一承载,
// 静态层为共享内核,实例层封装需要 QML 交互的区域截图会话。
class ScreenshotService : public QObject {
    Q_OBJECT
public:
    explicit ScreenshotService(QObject* parent = nullptr)
        : QObject(parent) {}

    // ---- 注入 ----
    // 主窗口 / 区域覆盖窗 / ScreenshotImageProvider。
    // 三者都非 owned:窗口由 main.cpp 构造,provider 由 QQmlEngine 持有。
    void setMainWindow(QQuickWindow* w) { main_window_ = w; }
    void setOverlayWindow(QQuickWindow* w) { overlay_window_ = w; }
    void setImageProvider(ScreenshotImageProvider* p) { provider_ = p; }

    // ---- 静态纯函数层(与 Controller/窗口完全解耦) ----

    // 全屏截图存 PNG。path 为空时用默认文件名 screenshot_<timestamp>.png。
    // draw_timestamp=true 时在右下角叠加录屏同款时间戳水印。
    // saved_path 非空时回填实际保存路径(可能自动补 .png 或使用默认文件名)。
    static bool TakeFullscreenPng(const QString& path,
                                  bool draw_timestamp = true,
                                  QString* saved_path = nullptr);

    // 抓一帧整屏返回 QImage(深拷贝,与 gdigrab 缓冲解耦)。
    // 供区域截图前置流程使用——整屏抓一次存 provider,再由用户自由选区域。
    static QImage CaptureFullscreenFrame();

    // 从整屏图裁剪选区并叠加水印(左上角标签 + 右下角时间戳,
    // 与全屏截图/录屏同款)。失败返回空 QImage。
    // ref_w/ref_h 为选区坐标系参考尺寸(区域覆盖窗的逻辑尺寸),
    // 与 full.size() 均 >0 时按比例换算高分屏 DPI。
    static QImage CropRegion(const QImage& full, int x, int y, int w, int h,
                             int ref_w = 0, int ref_h = 0,
                             bool draw_watermark = true);

    // QImage 存 PNG。qtgui 内置 PNG 编码,无系统依赖。
    static bool SavePng(const QImage& img, const QString& path);

    // ---- 保存暂存会话(跨 QML FileDialog 异步间隙保存选区坐标) ----

    // 记录用户在覆盖窗上画的选区,供之后 CompleteSaveWithPending 使用。
    void BeginSaveSession(int x, int y, int w, int h);

    // 用之前 BeginSaveSession 存的选区做裁剪→水印→存 png→收尾。
    bool CompleteSaveWithPending(const QString& file_path);

    // 查询 BeginSaveSession 是否已被调用、CompleteSaveWithPending 还没被调。
    bool HasPendingSave() const { return pending_save_active_; }

    // ---- 区域截图会话(实例方法,需要 View 层配合) ----

    // 启动区域截图会话:隐藏主窗 → 150ms 后抓整屏 → 存 provider → 显示覆盖窗。
    // 返回 true 表示成功发起,false 表示前置条件不满足(无 provider 等)。
    bool StartRegionSession();

    // 保存选区为 PNG:裁剪(provider 图,传入坐标)→ 水印 → 存 url → 收尾。
    bool CompleteSave(int x, int y, int w, int h, const QString& file_path);

    // 复制选区到系统剪贴板:裁剪 → 水印 → QClipboard::setImage → 收尾。
    bool CopyRegion(int x, int y, int w, int h);

    // 取消区域截图会话:隐藏覆盖窗 → 恢复主窗。供 Controller 的取消路径调用。
    void CancelSession();

    // 仅清会话标志(不碰窗口)。供"区域截图流程被区域录制接管"时使用——
    // Controller 先调 StartRegionSession 拿整屏图 + 显示覆盖窗,用户在覆盖窗上画完选区、
    // 点"确认录制"时 Controller 自己处理窗口显隐,但需要告诉 Service 会话已结束。
    void EndSession();

private:
    // 收尾:hide overlay + restore main + 清 provider 图 + 复位标志。
    void FinishSession();

    // ---- 注入依赖(裸指针,非 owned) ----
    QQuickWindow*           main_window_      = nullptr;
    QQuickWindow*           overlay_window_    = nullptr;
    ScreenshotImageProvider* provider_         = nullptr;

    // ---- 状态 ----
    bool session_active_ = false;

    // ---- 保存暂存(pending 坐标) ----
    int  pending_x_ = 0, pending_y_ = 0, pending_w_ = 0, pending_h_ = 0;
    bool pending_save_active_ = false;
};

}  // namespace re

#endif  // RECORD_EXPERT_SCREENSHOT_SERVICE_H
