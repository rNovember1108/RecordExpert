# 微信风格拖动选择框截图

> 历史方案文档（功能已实现）。当前实现以 `docs/项目架构与调用流程.md`、`ARCHITECTURE.md` 与源码为准。

## Context

当前 `src/model/Screenshot.cpp` 的 `CapturePngScreenshot` 只能按预设参数(`offset_x/y/width/height`)整屏截图,UI 上点"截图"按钮直接保存全屏 PNG,无法交互选择区域。用户希望像微信截图那样:点击按钮后屏幕变暗,鼠标拖动画选区,确认后只保存选区内的 PNG。这是对现有截图功能的交互升级,不改动录屏/工厂架构,仅扩展截图链路 + 新增一个全屏覆盖窗口组件。

## 设计要点

### 核心流程
1. 用户点"区域截图"按钮 → 隐藏主窗口 → 等 ~150ms(让 OS 重绘桌面无主窗)
2. C++ 调 gdigrab 抓一帧整屏 AVFrame(BGRX,复用现有采集逻辑)
3. AVFrame → QImage(Format_RGB32,零拷贝构造 + `.copy()` 持有)
4. QImage 存入 `ScreenshotImageProvider`,emit `regionScreenshotReady()`
5. QML 弹出全屏 `RegionCaptureWindow`,显示 `image://screenshot/desktop` 背景 + 半透明黑色遮罩
6. 鼠标拖动画选区,选区内透明(无遮罩)+ 边框 + 尺寸提示 + 工具栏
7. 双击/Enter/✓按钮确认 → 调 `saveRegionScreenshot(rect, outputPath)` → provider 内 QImage 复制 rect 后 `QImage::save(path,"PNG")` → emit `screenshotSaved(path)`
8. Esc/✕按钮取消 → emit `regionScreenshotCancelled()`
9. 关闭 RegionCaptureWindow → 恢复主窗口

### 水印
与 `takeScreenshot` 行为一致:在**裁剪后的 QImage** 上调用 `DrawTimestampWatermark` 等价逻辑(直接在 QImage 上绘制,绕过 AVFrame)。简化实现:用 `QPainter` 在 QImage 右下角画 `yyyy-MM-dd HH:MM:SS` 白字黑影,与现有 `watermark.cpp` 视觉风格保持一致(字号、颜色、阴影偏移)。不绘制左上角 'test' 标签(区域截图更接近微信,无标识标签)。

### 主窗口隐藏
- `startRegionScreenshot()` 调用 `QGuiApplication::focusWindow()->hide()` 或暴露 `QQuickWindow*` 给 controller 持有;最简方案:gui_main 把顶层 `QQmlApplicationEngine::rootObjects()[0]` 作为 `QQuickWindow*` 传给 controller 的 `setMainWindow(QQuickWindow*)`,start 时 `main_window_->hide()`,confirm/cancel 后 `main_window_->show()`
- 等 150ms 通过 `QTimer::singleShot(150, ...)` 实现,确保 OS 重绘

### 多屏
本机为单屏环境,不做多屏支持。gdigrab "desktop" 默认抓主屏。若用户有多屏,gdigrab 的多屏行为不在本次范围。

## 实施步骤

### 1. C++ 重构 `src/model/Screenshot.h` / `src/model/Screenshot.cpp`
- 抽取 `bool CaptureDesktopFrame(AVFrame** out_frame)`(从 `CapturePngScreenshot` 剥离采集部分,不含水印/WIC)
- `CapturePngScreenshot` 重构为 `CaptureDesktopFrame` + 水印 + `WritePngViaWic`,行为保持不变(CLI 入口不受影响)（历史方案，现状见 ARCHITECTURE.md）
- 新增 `QImage FrameToQImage(const AVFrame* frame)` 工具:用 BGRX 数据零拷贝构造 `QImage::Format_RGB32`,再 `.copy()` 持有
- 不新增 `WritePngRegion`:区域保存走 QImage 路径(下文 controller 内 `QImage::save`),避免重复实现裁剪+ WIC

### 2. 新增 `src/view/ScreenshotImageProvider.h` / `.cpp`
```cpp
class ScreenshotImageProvider : public QQuickImageProvider {
public:
    ScreenshotImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& reqSize) override;
    void setCapturedImage(const QImage& img);  // 由 RecorderViewModel 调用
    void clear();
private:
    QImage captured_;
};
```
- `requestImage` 忽略 `id`,直接返回 `captured_`(本应用只有一个被俘图)
- 线程安全:`setCapturedImage` / `clear` / `requestImage` 都在 GUI 主线程,无需锁

### 3. 扩展 `src/viewmodel/RecorderViewModel.h` / `.cpp`
新增:
- `Q_INVOKABLE bool startRegionScreenshot()` — 隐藏主窗 → 150ms 后采集 → 转 QImage → 存 provider → emit `regionScreenshotReady()`
- `Q_INVOKABLE void saveRegionScreenshot(int x, int y, int w, int h, QString path)` — 从 provider 取 QImage → `.copy(x,y,w,h)` → QPainter 画水印 → `QImage::save(path,"PNG")` → emit `screenshotSaved(path)` → 恢复主窗
- `Q_INVOKABLE void cancelRegionScreenshot()` — 清 provider → emit `regionScreenshotCancelled()` → 恢复主窗
- `void setMainWindow(QQuickWindow* w) { main_window_ = w; }`(gui_main 调用)
- `void setImageProvider(ScreenshotImageProvider* p) { provider_ = p; }`
- 新增 signals: `regionScreenshotReady()`, `regionScreenshotCancelled()`
- 私有成员:`ScreenshotImageProvider* provider_ = nullptr;`、`QQuickWindow* main_window_ = nullptr;`

### 4. 修改 `src/gui_main.cpp`
- `#include "ScreenshotImageProvider.h"`
- `#include <QQuickWindow>`
- 创建 `auto* provider = new ScreenshotImageProvider;`
- `engine.addImageProvider(QStringLiteral("screenshot"), provider);`
- engine.load 后,`controller.setMainWindow(qobject_cast<QQuickWindow*>(engine.rootObjects().value(0)));`
- `controller.setImageProvider(provider);`

### 5. 新增 `qml/RegionCaptureWindow.qml`
根节点 `ApplicationWindow`:
- `visibility: Window.FullScreen`,`flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint`
- `color: "transparent"`(让 Image 自己画背景)
- 子项:
  - `Image { source: "image://screenshot/desktop"; anchors.fill: parent }` — 显示被俘图
  - `Rectangle { color: "#80000000"; anchors.fill: parent }` — 全屏半透明遮罩
  - `MouseArea` 全屏覆盖:`onPressed` 记录起点,`onPositionChanged` 更新 `selRect`,`onReleased` 锁定
  - `Rectangle selRect`:`color: "transparent"`,`border.color: "#00aaff"`,`border.width: 2`,位置/尺寸跟随 `selRect`
  - `Rectangle dimMaskOuter`:遮罩外 4 块(选区上下左右),实现"选区透明,外围暗化"的真效果(微信同款)。可简化为:整个 dimming rect 在选区位置 `clip: true` 镂空——但 Qt Rectangle 不支持镂空。改用 4 个 Rectangle 围绕 `selRect` 排列
  - `Label sizeLabel`:`text: selRect.width + " × " + selRect.height`,贴 selRect 右下
  - `Rectangle toolbar`:贴 selRect 右下外侧,含 `Button "✓ 确认"` 和 `Button "✕ 取消"`
  - `Keys.onPressed`:Esc → `controller.cancelRegionScreenshot(); window.close()`;Enter → 调用确认逻辑
  - `MouseArea` 内 `onDoubleClicked`:同确认逻辑
- 确认逻辑(统一抽成 `function confirm()`):
  ```js
  var r = selRect
  controller.saveRegionScreenshot(r.x, r.y, r.width, r.height, controller.outputPath)
  window.close()
  ```
- `Component.onCompleted`: forceActiveFocus() 接键盘事件

### 6. 修改 `qml/ScreenshotPage.qml`
在"截图"按钮旁加:
```qml
Button {
    text: "区域截图"
    onClicked: {
        if (controller.startRegionScreenshot()) {
            var comp = Qt.createComponent("qrc:/qml/RegionCaptureWindow.qml")
            var win = comp.createObject()
            win.show()
        }
    }
    Connections {
        target: controller
        onRegionScreenshotReady: win.show()  // 等 C++ 抓帧完毕再显示
    }
}
```
调整为 `Component` + `Object` 模式:点按钮先调 `startRegionScreenshot()`,等 `regionScreenshotReady` 信号再创建并 show 窗口。

### 7. 修改 `qml.qrc`
新增 `<file>qml/RegionCaptureWindow.qml</file>`

### 8. 修改 `CMakeLists.txt`
在 `add_executable` 列表中加入:
- `src/view/ScreenshotImageProvider.cpp`
(其他都是 .h 或已存在文件)

## 关键文件

- [Screenshot.h](file:///d:/Work/RecordExpert/src/model/Screenshot.h) / [Screenshot.cpp](file:///d:/Work/RecordExpert/src/model/Screenshot.cpp) — 抽取 `CaptureDesktopFrame`
- 新增 [ScreenshotImageProvider.h](file:///d:/Work/RecordExpert/src/view/ScreenshotImageProvider.h) / `.cpp`
- [RecorderViewModel.h](file:///d:/Work/RecordExpert/src/viewmodel/RecorderViewModel.h) / [.cpp](file:///d:/Work/RecordExpert/src/viewmodel/RecorderViewModel.cpp) — 加 3 个 Q_INVOKABLE + 2 信号 + provider/window 指针
- `src/gui_main.cpp`（历史方案，现入口为 `src/main.cpp`）— 注册 image provider,注入 ViewModel
- 新增 [RegionCaptureWindow.qml](file:///d:/Work/RecordExpert/qml/RegionCaptureWindow.qml)
- [ScreenshotPage.qml](file:///d:/Work/RecordExpert/qml/ScreenshotPage.qml) — 加按钮
- [qml.qrc](file:///d:/Work/RecordExpert/qml.qrc) — 注册新 qml
- [CMakeLists.txt](file:///d:/Work/RecordExpert/CMakeLists.txt) — 加源文件

## 复用的现有工具
- [CapturePngScreenshot](file:///d:/Work/RecordExpert/src/model/Screenshot.cpp#L164) 内的 gdigrab 采集逻辑 → 抽取复用
- [WritePngViaWic](file:///d:/Work/RecordExpert/src/model/Screenshot.cpp#L53) 不动,继续给 `CapturePngScreenshot` 整屏路径使用
- [DrawTimestampWatermark](file:///d:/Work/RecordExpert/src/model/Watermark.h#L23) 不直接复用(它操作 AVFrame),QImage 路径用 QPainter 等效绘制,视觉对齐

## 验证

1. `cmake --build build`(或当前使用的 build 命令),零错误零警告
2. 桌面运行 `build\record_expert.exe`,切到"截图"页,点"区域截图"
3. 主窗口消失 → ~150ms 后全屏变暗覆盖层出现,背景是被俘桌面
4. 鼠标拖动画选区 → 选区内透明、外围暗化、边框 + 尺寸标签 + 工具栏跟随
5. 双击或点"✓ 确认" → 选区 PNG 保存到 outputPath(默认 `screenshot_<时间戳>.png` 路径或 controller.outputPath),主窗口恢复,状态栏显示 "Screenshot saved: ..."
6. 按 Esc 或点"✕ 取消" → 不保存,主窗口恢复
7. 验证保存的 PNG 尺寸 = 选区尺寸,右下角有 yyyy-MM-dd HH:MM:SS 时间戳水印
8. 验证原"截图"按钮(整屏)仍可用,未受重构影响
