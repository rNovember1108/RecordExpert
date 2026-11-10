# Qt/QML GUI 迁移方案（MSVC → MinGW + Qt5.12）

> 历史方案文档（迁移已完成）。当前架构以 `docs/项目架构与调用流程.md` 和 `ARCHITECTURE.md` 为准。

## Context

当前 RecordExpert 是纯 MSVC Win32 C++17 CLI 项目。用户要求添加 QML 页面：主视图含"录屏"和"截图"两个子视图，录屏视图有开始/暂停/继续/结束四个按钮。（历史方案，现状见 ARCHITECTURE.md）

经环境勘察：
- 系统已装 Qt5.12.12 LTS，但只有 `mingw73_32` 套件（无 msvc2017_32），与 MSVC 项目不兼容
- Qt5.6.3 msvc2013 太老，不适合 C++17
- FFmpeg 库（`depends/ffmpeg`）是 MSVC 格式 .lib + .def + .dll，但 MinGW ld 可直接链接 DLL（`-l:avformat-60.dll`）

**用户决策**：
1. 项目从 MSVC 改为 MinGW，复用 `C:\Qt\Qt5.12.12\5.12.12\mingw73_32`（含 `Tools\mingw730_32` 工具链）
2. 仅 GUI 入口，删掉 CLI 解析（历史方案，现状见 ARCHITECTURE.md）
3. Pause/Resume = 暂停采集 + 丢弃（推荐方案）

预期结果：`record_expert.exe` 启动后显示 QtQuick 窗口，录屏视图可开始/暂停/继续/结束，截图视图可保存 PNG。底层 ScreenRecorder/Screenshot/Watermark/Mp4Writer/VideoTrack/AudioTrack 模块复用，仅新增 Pause/Resume 接口和 Qt 适配层。（历史方案，现状见 ARCHITECTURE.md）

---

## A. CMakeLists.txt 改造

修改 [CMakeLists.txt](file:///d:/Work/RecordExpert/CMakeLists.txt)：

- 移除两处 MSVC 专用块：
  - `if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)` 32 位强校验（MinGW 生成器天然 32 位）
  - `if(MSVC)` 块的 `_CRT_SECURE_NO_WARNINGS` 和 `/utf-8`（MinGW 默认 UTF-8 源码）
- 新增 MinGW UTF-8 选项替代 `/utf-8`：
  ```
  target_compile_options(record_expert PRIVATE
      -finput-charset=UTF-8 -fexec-charset=UTF-8
      -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
  ```
  （`NOMINMAX`/`WIN32_LEAN_AND_MEAN` 防 windows.h 宏与 Qt 头冲突）
- Qt 集成：
  ```
  set(CMAKE_PREFIX_PATH "C:/Qt/Qt5.12.12/5.12.12/mingw73_32")
  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)  # qml.qrc 必需
  set(CMAKE_AUTOUIC ON)  # 保留以备后续 Widgets
  find_package(Qt5 5.12 COMPONENTS Core Gui Qml Quick Widgets REQUIRED)
  ```
- FFmpeg 链接改 MinGW 形式：`target_link_directories` 指向 `${FFMPEG_BIN_DIR}`（不是 lib），`target_link_libraries` 用 `-l:avformat-60.dll -l:avcodec-60.dll -l:avdevice-60.dll -l:avfilter-9.dll -l:avutil-58.dll -l:swscale-7.dll -l:swresample-4.dll`。保留 `windowscodecs ole32`（g++ 能直接吃系统 .lib）。
- 源文件列表新增：`src/gui_main.cpp`、`src/viewmodel/RecorderViewModel.h`、`src/viewmodel/RecorderViewModel.cpp`；移除 `src/main.cpp`（删除该文件，内容并入 gui_main.cpp）
- 资源文件：`qt5_add_resources(RES_QRC qml.qrc)` 后把 `${RES_QRC}` 加入 add_executable
- target_link_libraries 增加：`Qt5::Core Qt5::Gui Qt5::Qml Qt5::Quick Qt5::Widgets`
- POST_BUILD：
  - 新增 windeployqt 命令（拷贝 Qt DLL 到输出目录，扫描 qml/ 提取 QML 依赖）：
    ```
    "${CMAKE_PREFIX_PATH}/bin/windeployqt.exe"
        --qmldir ${CMAKE_SOURCE_DIR}/qml
        --release
        $<TARGET_FILE:record_expert>
    ```
  - 原有 FFmpeg DLL `copy_directory` 命令保留

---

## B. 入口重写（src/main.cpp → src/gui_main.cpp）

删除 [src/main.cpp](file:///d:/Work/RecordExpert/src/main.cpp) 全部内容并替换为 GUI 入口。删除的元素：
- `<conio.h>` include 与 `_kbhit`/`_getch` 轮询循环
- `PrintUsage` 函数及 CLI 选项解析循环（历史方案，现状见 ARCHITECTURE.md）
- `HandleSignal`/`g_recorder`（Ctrl+C 信号处理；GUI 不需要）
- `EnsurePngExtension`/`TimestampForFilename`/`StemOfPath`（截图文件名派生移入 RecorderViewModel）

新 `src/gui_main.cpp`（历史方案，现入口为 `src/main.cpp`）内容骨架：
```cpp
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include "RecorderViewModel.h"

int main(int argc, char** argv) {
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Fusion");

    re::RecorderViewModel controller;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("controller", &controller);
    engine.load(QUrl("qrc:/qml/MainWindow.qml"));
    if (engine.rootObjects().isEmpty()) return -1;

    return app.exec();
}
```

---

## C. 新增 RecorderViewModel（QObject 子类）

新文件 [src/viewmodel/RecorderViewModel.h](file:///d:/Work/RecordExpert/src/viewmodel/RecorderViewModel.h) 和 [src/viewmodel/RecorderViewModel.cpp](file:///d:/Work/RecordExpert/src/viewmodel/RecorderViewModel.cpp)：

**职责**：QML ↔ ScreenRecorder/Screenshot 桥接，状态机管理。

**Q_PROPERTY**：
- `isRecording` (bool, NOTIFY recordingStarted/recordingStopped)
- `isPaused` (bool, NOTIFY paused/resumed)
- `outputPath` (QString, NOTIFY outputPathChanged)
- `status` (QString, NOTIFY statusChanged) — 显示错误/状态文本

**public slots (Q_INVOKABLE 等价)**：
- `void start()` — 用 outputPath 组装 RecorderOptions，`recorder_ = make_unique<ScreenRecorder>(opts); recorder_->Start(0)`（无限时长），emit recordingStarted
- `void pause()` — 转发 `recorder_->Pause()`，emit paused
- `void resume()` — 转发 `recorder_->Resume()`，emit resumed
- `void stop()` — `recorder_->Stop(); recorder_.reset()`，emit recordingStopped
- `void takeScreenshot(QString path)` — 在 GUI 线程同步调 `re::CapturePngScreenshot`，emit screenshotSaved/errorOccurred；文件名如未带 .png 后缀自动补；未指定路径用 `screenshot_<yyyyMMdd_HHmmss>.png`

**signals**：
- `recordingStarted()`、`recordingStopped()`、`paused()`、`resumed()`、`screenshotSaved(QString)`、`errorOccurred(QString)`

**内部**：
- `std::unique_ptr<re::ScreenRecorder> recorder_`
- `QString outputPath_` 默认 `"output.mp4"`
- 状态机：Idle → Recording ⇄ Paused → Idle，由 isRecording/isPaused 两个 bool 推送 QML 绑定
- 默认参数：fps=30、bit_rate=4'000'000、capture_audio=true、audio_device="麦克风阵列 (Realtek(R) Audio)"（与 CLI 默认一致）；GUI 暂不暴露这些高级旋钮，保持简单（历史方案，现状见 ARCHITECTURE.md）

---

## D. ScreenRecorder 增加 Pause/Resume

修改 [src/model/ScreenRecorder.h](file:///d:/Work/RecordExpert/src/model/ScreenRecorder.h)：

新增 public：
```cpp
bool Pause();      // 仅当 recording_ && !paused_ 时置 true 并转发两轨，返回 true
bool Resume();     // 仅当 paused_ 时置 false 并转发两轨，返回 true
bool IsPaused() const { return paused_.load(); }
```

新增 private 成员：`std::atomic<bool> paused_{false};`

Pause/Resume 转发逻辑：`if (video_) video_->Pause(); if (audio_) audio_->Pause();`（audio_ 可能为 null，仅录视频时）。

**不需要改 Start/Stop/WaitUntilDone/Close 主体**——paused_ 是 worker 循环内轮询的，与 stop_requested_ 解耦。

---

## E. VideoTrack Pause 实现（历史方案，现状见 ARCHITECTURE.md）

修改 `src/video_track.h` 和 `src/video_track.cpp`（历史方案，现状见 ARCHITECTURE.md）：

**头文件新增**：
```cpp
void Pause() { paused_.store(true); }
void Resume() {
    if (pause_start_ != std::chrono::steady_clock::time_point{}) {
        total_paused_ += std::chrono::steady_clock::now() - pause_start_;
        pause_start_ = std::chrono::steady_clock::time_point{};
    }
    paused_.store(false);
}
bool IsPaused() const { return paused_.load(); }

private 新增成员：
std::atomic<bool> paused_{false};
std::chrono::steady_clock::time_point pause_start_{};
std::chrono::steady_clock::duration total_paused_{};
```

**Run 循环改造**（在 `src/video_track.cpp` 的 Run 函数 while 入口处插入）（历史方案，现状见 ARCHITECTURE.md）：
```cpp
while (!stop.load()) {
    if (paused_.load()) {
        if (pause_start_ == std::chrono::steady_clock::time_point{})
            pause_start_ = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
    }
    // ...原 av_read_frame/sws/编码/写包逻辑
}
```

**PTS 修正**：Run 里计算 `elapsed` 的两处：
```cpp
// 原：auto elapsed = std::chrono::duration<double>(now - start).count();
auto now = std::chrono::steady_clock::now();
auto paused_dur = total_paused_;
if (paused_.load() && pause_start_ != time_point{})
    paused_dur += now - pause_start_;
double elapsed = std::chrono::duration<double>(now - start - paused_dur).count();
```
pts 单调保护 `if (pts <= pts_) pts = pts_ + 1` 原样保留。

**gdigrab 暂停安全性**：gdigrab 是按需 BitBlt，不调 av_read_frame 就不抓新帧，无内部缓冲堆积；Resume 后下一帧的 elapsed 自然跳变，PTS 单调保护兜底。

---

## F. AudioTrack Pause 实现（方案 F1：暂停期间不读不写）（历史方案，现状见 ARCHITECTURE.md）

修改 `src/audio_track.h` 和 `src/audio_track.cpp`（历史方案，现状见 ARCHITECTURE.md）：

头文件新增与 VideoTrack 镜像的 `Pause/Resume/IsPaused` 接口 + 成员。（历史方案，现状见 ARCHITECTURE.md）

**Run 循环改造**：
```cpp
while (!stop.load()) {
    if (paused_.load()) {
        if (pause_start_ == time_point{})
            pause_start_ = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
    }
    // ...原 av_read_frame/swr/fifo/编码/写包逻辑
}
```

**不选 F2（持续 av_read_frame 丢弃）的原因**：
- interrupt_callback 只看 `stop_flag_`，无法让 paused_ 触发 AVERROR_EXIT 提前返回；与 stop 路径混淆
- dshow 无数据时 av_read_frame 会阻塞，sleep+continue 死循环里反而把 worker 挂在 dshow 内部等待

**F1 风险**：dshow 内部 `rtbufsize`（默认 16MB，48kHz/2ch/s16 约 87 秒）会堆积。短期暂停无丢帧；长暂停超过容量时 dshow 自身丢老包，Resume 后第一帧有几毫秒延迟，**不崩溃不卡死**。GUI 层建议在 RecorderViewModel 加单次暂停 60 秒软上限警告（非阻塞）。

**PTS 不需要修正**：音频 PTS = `pts_base_ + samples_emitted_`，靠声卡采样计数推进；暂停期间 samples_emitted_ 不增长，Resume 后自然接续（pts_base_ 在首帧到达时一次性捕获的偏移，与墙钟脱钩）。total_paused_ 在 AudioTrack 保留但仅诊断用。（历史方案，现状见 ARCHITECTURE.md）

---

## G. QML 文件结构

新建 [qml/MainWindow.qml](file:///d:/Work/RecordExpert/qml/MainWindow.qml)：
- `ApplicationWindow` 顶层，标题 "RecordExpert"，宽 480 高 320
- 顶栏两 Tab 按钮："录屏"、"截图"
- `StackLayout` 或 `Loader { id: loader; source: "qrc:/qml/RecordPage.qml" }` 切换子视图

新建 [qml/RecordPage.qml](file:///d:/Work/RecordExpert/qml/RecordPage.qml)：
- `Column` 布局
- 4 个 Button：开始（!isRecording 时启用）、暂停（isRecording && !isPaused 时启用）、继续（isPaused 时启用）、结束（isRecording 时启用）
- `TextField` 输出路径，绑定 `controller.outputPath`
- `Label` 状态显示（`controller.status`）

新建 [qml/ScreenshotPage.qml](file:///d:/Work/RecordExpert/qml/ScreenshotPage.qml)：
- `Column` 布局
- `Button` 截图（onClick: `controller.takeScreenshot(controller.outputPath)`）
- `TextField` 输出路径
- `Label` 状态显示

新建 [qml.qrc](file:///d:/Work/RecordExpert/qml.qrc)：
```xml
<RCC>
  <qresource prefix="/">
    <file>qml/MainWindow.qml</file>
    <file>qml/RecordPage.qml</file>
    <file>qml/ScreenshotPage.qml</file>
  </qresource>
</RCC>
```

---

## H. 构建与调试

**构建命令**（PowerShell）：
```powershell
$env:Path = "C:\Qt\Qt5.12.12\Tools\mingw730_32\bin;C:\Qt\Qt5.12.12\5.12.12\mingw73_32\bin;" + $env:Path
cmake -G "MinGW Makefiles" `
  -DQt5_DIR=C:/Qt/Qt5.12.12/5.12.12/mingw73_32/lib/cmake/Qt5 `
  -DCMAKE_PREFIX_PATH=C:/Qt/Qt5.12.12/5.12.12/mingw73_32 `
  -S . -B build
cmake --build build -j
```

**调试**（[.vscode/launch.json](file:///d:/Work/RecordExpert/.vscode/launch.json)，需用户手动移入因沙箱保护）：
- `type` 从 `cppvsdbg` 改为 `cppdbg`（gdb 系）
- `miDebuggerPath` 指向 `C:/Qt/Qt5.12.12/Tools/mingw730_32/bin/gdb.exe`
- `cwd` 改为 `${workspaceFolder}/build`
- `program` 改 `${workspaceFolder}/build/record_expert.exe`

**运行**：exe 同目录需 Qt DLLs（windeployqt 已处理）+ FFmpeg DLLs（POST_BUILD copy 已处理）。

---

## 关键风险点

1. **MinGW ld 链接 MSVC 编译的 FFmpeg DLL**：`-l:avformat-60.dll` 直接吃 DLL 导出表通常可行，但若 .def 里有 DATA 导出可能告警。回退方案：`gendef`+`dlltool` 从 .dll 生成 .a 导入库。
2. **C++17 与 Qt 5.12 headers**：MinGW 7.3.0 + C++17 编译 Qt 5.12 LTS 头 OK。需加 `NOMINMAX WIN32_LEAN_AND_MEAN` 防 windows.h 宏冲突。
3. **h264_mf / aac_mf**：Media Foundation 是系统编解码器，FFmpeg 动态加载，与编译器无关，回退链不变。
4. **dshow 暂停堆积**：方案 F1，超 87 秒暂停会丢部分音频样本，不崩溃。建议 GUI 加 60 秒软上限提示。
5. **windeployqt --release**：MinGW Makefiles 无 config 区分，统一加 `--release`。

---

## 验证清单

1. 构建：`cmake -G "MinGW Makefiles" ... -S . -B build` + `cmake --build build -j`，无错误无 undefined reference
2. 启动 GUI：`build/record_expert.exe`，窗口弹出"RecordExpert"标题，可见"录屏"和"截图"两 Tab
3. 切换到"录屏"视图，4 个按钮初始状态：开始=启用、暂停/继续/结束=禁用
4. 点开始：状态变 Recording，输出文件 output.mp4 生成
5. 点暂停：dshow 不再读，gdigrab 不再抓；点继续：恢复采集
6. 点结束：mp4 收尾可正常播放（含视频 + 音频）
7. 切到"截图"视图，点截图按钮：screenshot.png 生成，Qt QImage::save 编码 PNG（1920×1080 约 374KB）
8. 关闭窗口：析构 Stop+Close 调用，无崩溃无泄漏

---

## 关键改动文件清单

- [CMakeLists.txt](file:///d:/Work/RecordExpert/CMakeLists.txt) — MinGW + Qt 集成
- [src/main.cpp](file:///d:/Work/RecordExpert/src/main.cpp) → 重命名/重写为 `src/gui_main.cpp`（历史方案，最终未落地）
- 新增 [src/viewmodel/RecorderViewModel.h](file:///d:/Work/RecordExpert/src/viewmodel/RecorderViewModel.h)
- 新增 [src/viewmodel/RecorderViewModel.cpp](file:///d:/Work/RecordExpert/src/viewmodel/RecorderViewModel.cpp)
- [src/model/ScreenRecorder.h](file:///d:/Work/RecordExpert/src/model/ScreenRecorder.h) — + Pause/Resume 接口
- `src/video_track.h` / `src/video_track.cpp` — Run 循环 + PTS 修正（历史方案，现状见 ARCHITECTURE.md）
- `src/audio_track.h` / `src/audio_track.cpp` — Run 循环（方案 F1）（历史方案，现状见 ARCHITECTURE.md）
- 新增 [qml/MainWindow.qml](file:///d:/Work/RecordExpert/qml/MainWindow.qml)
- 新增 [qml/RecordPage.qml](file:///d:/Work/RecordExpert/qml/RecordPage.qml)
- 新增 [qml/ScreenshotPage.qml](file:///d:/Work/RecordExpert/qml/ScreenshotPage.qml)
- 新增 [qml.qrc](file:///d:/Work/RecordExpert/qml.qrc)
- 复用：[src/model/Screenshot.h](file:///d:/Work/RecordExpert/src/model/Screenshot.h) / [src/model/Screenshot.cpp](file:///d:/Work/RecordExpert/src/model/Screenshot.cpp)（WIC PNG，不动）
- 复用：[src/model/Watermark.h](file:///d:/Work/RecordExpert/src/model/Watermark.h) / [src/model/Watermark.cpp](file:///d:/Work/RecordExpert/src/model/Watermark.cpp)（GDI 真字体水印，不动）
- 复用：[src/model/Mp4Writer.h](file:///d:/Work/RecordExpert/src/model/Mp4Writer.h) / [src/model/Mp4Writer.cpp](file:///d:/Work/RecordExpert/src/model/Mp4Writer.cpp)（共享 MP4，不动）
