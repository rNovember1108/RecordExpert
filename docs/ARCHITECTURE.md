# RecordExpert —— 架构与代码分析

> 本文档整理 RecordExpert 录屏工具的整体架构、模块职责、关键数据流与设计决策，供维护者快速理解代码组织与扩展点。

---

## 1. 项目概述

RecordExpert 是一个 Windows 桌面录屏工具，支持全屏 / 指定区域 / 指定窗口录制、可选摄像头画中画、麦克风 + 系统声音混音，以及内置视频播放与区域截图。

**核心能力：**
- 桌面录屏（gdigrab）或摄像头录制（dshow），可选桌面 + 摄像头画中画合成
- 麦克风采集（dshow）或麦克风 + 系统声音混音（双路 dshow 设备等权叠加）
- 倍率录制（time-lapse，支持 1/2/4/8）
- 区域截图（gdigrab 单帧 → 水印 → Qt 内置 PNG 编码）
- 内置 MP4 播放器（FFmpeg 解码，音频时钟驱动帧消费，场景图纹理渲染）
- GUI 暂停 / 继续 / 停止，悬浮控制窗

---

## 2. 技术栈与构建约束

| 类别 | 选型 |
|------|------|
| 编程语言 | C++17 |
| UI 框架 | Qt 5.12 (QGuiApplication + QML) |
| 编译器 | MinGW 7.3.0 32-bit（`CMakeLists.txt` 固定 `mingw73_32` 套件） |
| 视频编码 | FFmpeg mpeg4（纯软件，GUI COM STA 兼容） |
| 音频编码 | FFmpeg native AAC，FLTP / 48kHz / 2ch |
| 屏幕采集 | FFmpeg gdigrab（BitBlt，硬编码 SRCCOPY\|CAPTUREBLT） |
| 音视频设备 | FFmpeg dshow |
| PNG 编码 | Qt 内置 `QImage::save(path, "PNG")`（Qt5Gui 自带） |
| 播放器渲染 | `QQuickItem` + `updatePaintNode` + `QSGImageNode`（纹理交给场景图，GPU 缩放） |
| 水印渲染 | `QPainter` 绘制（零拷贝包裹 32bpp BGRX 帧为 QImage，白字 + 右下 1px 黑阴影） |
| 构建系统 | CMake + MinGW Makefiles |

**关键构建约束：**
- Qt 套件固定为 `C:/Qt/Qt5.12.12/5.12.12/mingw73_32`（`QT_PREFIX`），FFmpeg 库同为 32-bit x86；MinGW 工具链天然产出 32 位二进制，`-A Win32` 只适用于 Visual Studio 生成器
- 链接 `-Wl,--large-address-aware`，把 32 位进程可用地址空间从 2GB 扩到 4GB（单帧 1920×1080×4 抓屏需一次分配约 8MB 连续内存）
- GUI 主线程为 COM STA（QGuiApplication 隐式 CoInitialize），**h264_mf 不可用**（avcodec_send_frame 在 STA 下必返 AVERROR_EXTERNAL），故统一使用 mpeg4
- WIN32 GUI 子系统（无控制台）；构建后把 `depends/ffmpeg/bin` 下的 DLL 与 `windeployqt` 部署的 Qt 运行时拷贝到 exe 同目录

---

## 3. 整体架构

```
┌──────────────────────────────────────────────────────────────────┐
│             RecorderViewModel (ViewModel, GUI 主线程)              │
│  QML 绑定 / 状态机 / 计时器 / 窗口显隐 / 区域录制坐标换算           │
└──────┬──────────────────────┬───────────────────────┬───────────┘
       │                      │                       │
       ▼                      ▼                       ▼
┌───────────────┐   ┌──────────────────┐   ┌───────────────────────┐
│RecordingService│  │ScreenshotService │   │ VideoPlayerItem (View)│
│ 录制生命周期   │  │截图会话/裁剪/水印 │   │ QQuickItem+QSGImageNode│
└──────┬────────┘   └────────┬─────────┘   └───────────┬───────────┘
       │                     │                         │ 持有
       ▼                     ▼                         ▼
┌──────────────┐     ┌──────────────┐       ┌──────────────────────┐
│ScreenRecorder│     │Screenshot 模块│       │ PlayerViewModel      │
│  编排与生命周期│    │gdigrab+QPainter│      │ 状态机/时钟/帧调度   │
└──────┬───────┘     └──────────────┘       └──────────┬───────────┘
       │                                               │
       ▼                                               ▼
┌─────────────────────────────┐            ┌──────────────────────┐
│ Mp4Writer 共享 MP4 + 互斥锁  │            │ PlaybackDecoder (Model) │
└──────────┬──────────────────┘            │ avformat+sws+atempo  │
           │                               └──────────┬───────────┘
  ┌────────┴─────────┐                               │
  ▼                  ▼                               ▼
┌─────────────┐ ┌─────────────┐           ┌──────────────────────┐
│IVideoSource │ │IAudioSource │           │ AudioPlayer          │
│ 视频采集    │ │ 音频采集    │           │ QAudioOutput (pull)  │
└─────────────┘ └─────────────┘           └──────────────────────┘
```

**组件关系：**
- `RecorderViewModel`（ViewModel）只做 QML 桥接，录制 / 截图 / 枚举业务下沉到 `RecordingService` / `ScreenshotService` / `EnumerationService`；窗口显隐（Qt 5.12 下必须 C++ `show()/hide()`）与 UI 计时器仍留在本类
- `RecordingService` 独占 `ScreenRecorder`（`unique_ptr`），负责 `RecorderOptions` 的组装与前置校验
- `ScreenRecorder` 持有 `Mp4Writer`（共享输出）、一个 `IVideoSource`、一个可选的 `IAudioSource`
- `VideoPlayerItem`（View，`QQuickItem` + `QSGImageNode`）持有 `PlayerViewModel`（播放状态机 + 时钟 + 帧调度），后者持有 `PlaybackDecoder`；`AudioPlayer` 由 `PlaybackDecoder` 创建并持有

---

## 4. 目录结构

```
src/
├── main.cpp                    # GUI 入口：QGuiApplication + QQmlApplicationEngine + 注入 controller
├── view/
│   ├── VideoPlayerItem.{h,cpp} # 播放器 View：QQuickItem + QSGImageNode 纹理渲染
│   └── ScreenshotImageProvider.{h,cpp} # QML Image Provider（区域截图背景图）
├── viewmodel/
│   ├── RecorderViewModel.{h,cpp} # ViewModel：QML 绑定 + 录制状态机 + 计时器 + 窗口显隐
│   └── PlayerViewModel.{h,cpp}   # 播放器 ViewModel：状态机 + 音频/墙钟 + 帧调度
└── model/
    ├── ScreenRecorder.{h,cpp}  # 录制会话编排（Start/Stop/Pause/Resume）
    ├── Mp4Writer.{h,cpp}       # 共享 MP4 输出上下文（WriteHeader/EncodeAndWrite）
    ├── PlaybackDecoder.{h,cpp} # 解码线程（Model）：avformat/avcodec/sws/atempo/swr
    ├── AudioPlayer.{h,cpp}     # QAudioOutput pull 模式 + SPSC 环形缓冲 + 音频时钟
    ├── Watermark.{h,cpp}       # QPainter 水印（标签 + 时间戳，零拷贝包裹 BGRX 帧）
    ├── Screenshot.{h,cpp}      # 单帧抓取 → 水印 → QImage → Qt 内置 PNG 编码
    ├── capture/
    │   ├── i_video_source.h        # 视频源抽象接口
    │   ├── i_audio_source.h        # 音频源抽象接口
    │   ├── gdi_grab_video_source.{h,cpp}    # gdigrab 桌面录屏（QPainter 手动水印 + sws 转 YUV420P）
    │   ├── dshow_video_source.{h,cpp}       # dshow 摄像头
    │   ├── composite_video_source.{h,cpp}   # 桌面 + 摄像头画中画（双线程）
    │   ├── dshow_audio_source.{h,cpp}       # dshow 单路麦克风
    │   ├── mixed_audio_source.{h,cpp}       # 双路 dshow 等权混音
    │   ├── video_source_factory.{h,cpp}     # VideoSource 运行时工厂
    │   └── audio_source_factory.{h,cpp}     # AudioSource 运行时工厂
    └── service/
        ├── RecordingService.{h,cpp}    # RecorderOptions 组装校验 + ScreenRecorder 生命周期
        ├── ScreenshotService.{h,cpp}   # 截图/区域截图业务（静态纯函数层 + 实例会话层）
        └── EnumerationService.{h,cpp}  # 窗口 + dshow 音视频设备枚举（纯静态）

qml/
├── MainWindow.qml              # 主窗：顶栏 Tab（录屏/截图）+ Loader 子页，承载各静态子窗口
├── RecordPage.qml              # 录制页：范围模式 / 设备 / 倍率 / 输出路径 / 控制按钮
├── ScreenshotPage.qml          # 截图页：全屏截图 / 区域截图入口
├── RegionCaptureWindow.qml     # 区域截图与区域录制的全屏选区覆盖窗
├── RecordingOverlay.qml        # 录制悬浮控制窗（暂停/继续/停止 + 计时，显隐由 C++ 控制）
└── PlayerWindow.qml            # 内置播放器窗口（VideoPlayerItem + 控制栏）
```

---

## 5. 模块详解

### 5.1 ScreenRecorder —— 录制编排器

**职责：**
- 持有共享的 `Mp4Writer` 和两条采集源（`IVideoSource` / `IAudioSource`）
- 在 `Start()` 中打开设备、写文件头，然后启动视频 worker（`capture_audio` 为真时再加音频 worker）
- 维护两条媒体时间轴的共同墙钟零点（`capture_start_`）
- 转发暂停 / 继续 / 停止请求，协调两条轨收尾

**关键接口：**
- `Start(duration)` — 打开设备 → 写头 → 启动 worker，立即返回
- `Stop()` — 请求停止 → join worker（各源在 `Run()` 末尾已 flush 编码器）→ Close（写 trailer）
- `Pause()` / `Resume()` — 转发给 video_ 和 audio_

**线程模型：**
- `video_worker_` 调用 `video_->Run(...)`，独立线程
- `audio_worker_` 调用 `audio_->Run(...)`，独立线程（仅 `capture_audio` 为真时创建）
- 两个 worker 结束后都会 `stop_requested_.store(true)` 让对方尽快退出

### 5.2 Mp4Writer —— 共享输出

**职责：**
- 管理 `AVFormatContext* out_fmt_ctx_`（由 `avformat_alloc_output_context2` 创建）
- `NewStream()` 从编码器拷贝 codecpar 到 `AVStream`
- `WriteHeader()` 打开 IO + 写文件头
- `EncodeAndWrite(enc, stream, frame)` — 向编码器送帧 → 取 packet → 时基换算 → `av_interleaved_write_frame`
- `Close()` — 写 trailer（仅一次）→ `avio_closep` → `avformat_free_context`；编码器 flush 由各 Source 在 `Run()` 末尾调用 `EncodeAndWrite(nullptr)` 完成

**线程安全：**
- `EncodeAndWrite` 内部用 `std::lock_guard<std::mutex>` 串行化 `av_interleaved_write_frame`，因为视频 / 音频线程共享同一个输出上下文

### 5.3 IVideoSource 体系

**抽象接口（`i_video_source.h`）：**
- `Open()` / `Close()`
- `Run(duration, start, stop_flag)` — 阻塞式主循环
- `Pause()` / `Resume()`

**三个具体实现：**

| 类 | 采集后端 | 水印 | 缩放 | 备注 |
|----|----------|------|------|------|
| `GdiGrabVideoSource` | gdigrab 桌面 | QPainter 直接画在采集帧上（`kWatermarkLabel` 左上角 + 右下角时间戳） | sws → YUV420P | 无 libavfilter 路径，水印统一复用 `Watermark.h` |
| `DShowVideoSource` | dshow 摄像头 | 无 | sws → YUV420P | 暂停期间持续读包丢弃（防 rtbufsize 溢出） |
| `CompositeVideoSource` | gdigrab 主 + dshow 副 | 桌面帧叠加 QPainter 水印 | 主 sws + 副 sws | 摄像头独立线程 drop-old 只留最新帧，主循环每帧 blit 到右下角 PiP 区 |

**共同特征：**
- 输入 `AVFormatContext*` 必须在 `avformat_open_input` 前手动 `avformat_alloc_context` 并挂中断回调（让停止请求能打断阻塞采集）
- `Run()` 循环内 `av_frame_unref(dec_frame)` 在每个分支正确调用；循环结束后统一 flush 编码器（`EncodeAndWrite(nullptr)`）
- `Close()` 使用 `if (ptr)` 守卫，可安全处理部分初始化状态；各 Source 的 `Open()` 失败路径也调 `Close()` 兜底

### 5.4 IAudioSource 体系

**抽象接口（`i_audio_source.h`）：**
- `Open()` / `Close()`
- `Run(start, stop_flag)` — 阻塞式主循环
- `Pause()` / `Resume()`

**两个具体实现：**

| 类 | 采集后端 | 重采样 | FIFO | 备注 |
|----|----------|--------|------|------|
| `DShowAudioSource` | 单路 dshow | SwrContext → FLTP | 一个 AVAudioFifo | `DrainFifo()` 攒够 1024 样本喂 AAC，flush 时补齐残帧 |
| `MixedAudioSource` | 双路 dshow | 两路各自 SwrContext | 两个输入 FIFO + 一个输出 FIFO | `PumpOne()` 非阻塞读两路，`MixOne()` 等权 0.5+0.5 clamp，`MixOne` 内含 resync 机制（一路超阈值时截断较长侧） |

**音频特有的暂停策略：**
- 暂停期间持续 `av_read_frame` 读出 dshow 推流的包并立即 `av_packet_unref`，防止 rtbufsize（16MB）持续堆积并在长暂停时溢出丢包
- 音频 PTS 与视频同口径：共享零点墙钟换算（扣除暂停总时长），暂停时长计入 `total_paused_`；Resume 后自然接续，不累积晶振偏差

### 5.5 VideoPlayerItem + PlaybackDecoder —— 内置播放器

**职责：**
- `PlaybackDecoder`（`QThread` 子类）：`avformat_open_input` → 找视频流与音频流 → 开解码器 → 循环 `av_read_frame` → 按 stream_index 分流：视频 `avcodec_send/receive` → `sws_scale` 到 RGB24 → 入有界队列（60 帧）；音频 `avcodec_send/receive` → atempo filter（倍速时）→ `swr_convert` 到 S16LE → `AudioPlayer` 环形缓冲
- `AudioPlayer`：32KB SPSC 环形缓冲 → `QAudioOutput`（pull 模式，S16LE interleaved，采样率 / 声道沿用源文件）；暂停/恢复用 `suspend()/resume()` 正规配对，不清缓冲
- `PlayerViewModel`（ViewModel，QObject）：播放状态机 + 音频/墙钟 + 帧调度（QTimer 按 1000/fps/speed 间隔 tick，以音频时钟为权威驱动帧消费）
- `VideoPlayerItem`（View，`QQuickItem` 子类）：`ItemHasContents` + `updatePaintNode()` + `QSGImageNode` 纹理渲染
  - `frameReady`（GUI 线程）→ 加锁快照 `pending_frame_` → `update()`
  - `updatePaintNode`（渲染线程）→ `window()->createImageNode()` 复用/新建 `QSGImageNode` → `createTextureFromImage(frame, TextureIsOpaque)` 新建纹理 → `setTexture`（`ownsTexture` 自动回收旧纹理）→ `setRect` 等比居中，GPU 采样完成缩放
  - Qt 5.12 的 `QSGTexture` 不支持原地更新像素，故每帧新建纹理；尚无帧时返回 `nullptr`，黑色背景由 QML `Rectangle` 提供
  - `frameReady` 与 `widthChanged` / `heightChanged` 都触发 `update()`；`updatePaintNode` 内按 item 尺寸与帧尺寸重算 `setRect`。渲染线程不直接读 `PlayerViewModel::currentFrame()`，避免跨线程数据竞争

**帧消费时钟机制（音频主时钟，与 ffplay/mpv 同构）：**
1. 音频可用时：`target_pts = qMax(audioClockMs(), position_ms_)`（以 atempo 输入前的解码样本数为基准，并用 `old_data_end_` 边界区分倍速切换前后的旧/新样本，切换/暂停恢复不清缓冲、时钟不冻结）
2. 无音频或音频未启动时降级墙钟：`target_pts = start_pts_ + elapsed_ms * speed_`
3. 只消费 `pts_ms <= target_pts` 的帧，超过目标的缓存到 `pending_frame_` 留待下次 tick

**节流策略：**
- `positionChanged` 信号每 200ms 最多 emit 一次，避免 burst 信号导致 QML 跳秒
- `tryTakeFrame()` 非阻塞取帧，防止 GUI 线程被解码阻塞

### 5.6 Watermark —— QPainter 文字水印

**职责：**
- `DrawLabelWatermark(frame, kWatermarkLabel)` — 左上角固定标签（右下偏移 1px 黑阴影 + 原位白色正文）
- `DrawTimestampWatermark(frame)` — 右下角 "YYYY-MM-DD HH:MM:SS" 时间戳
- `kWatermarkLabel` 全局常量：录制、截图、区域截图四处水印共用，改一处全部生效

**渲染路径：**
- 核心实现 `DrawTextOnImage(QImage*, ...)`：QPainter 直接绘制，支持中文（UTF-8）
- AVFrame 版本：零拷贝包裹 32bpp BGRX 帧缓冲为 QImage 后转发核心实现，无像素拷贝

**适用帧格式：**
- 仅支持 32bpp BGRX（gdigrab 原生输出）；YUV 帧跳过水印（DShowVideoSource / CompositeVideoSource 的摄像头画面）

### 5.7 Screenshot —— 单帧截图

**职责：**
- `CaptureDesktopFrame()` — 单帧 gdigrab 抓取（`draw_mouse=0`），返回 BGRA/BGR0 解码帧
- `CapturePngScreenshot(path, opts)` — 复用 `CaptureDesktopFrame` → 水印（`draw_timestamp` 为真时标签 + 时间戳）→ `FrameToQImage` → `QImage::save` 编码 PNG

**PNG 编码链路：**
`QImage::save(path, "PNG")`（PNG 编码内建于 Qt5Gui，Windows/Linux 共用同一实现，不依赖 WIC）

**区域截图（ScreenshotService 路径）：**
- 静态纯函数层（无 Qt 窗口依赖，可独立单测）：`TakeFullscreenPng` / `CaptureFullscreenFrame` / `CropRegion` / `SavePng`，底层复用 `Screenshot.h` 的 gdigrab 抓帧与 QPainter 水印
- `StartRegionSession()`：隐藏主窗 → 150ms 后抓整屏 → 灌入 `ScreenshotImageProvider` → 显示覆盖窗
- QML `RegionCaptureWindow` 通过 `image://screenshot/desktop` 取图作为选区背景
- 确认选区后 `CompleteSave(x,y,w,h,path)` / `CopyRegion(x,y,w,h)` 走 `CropRegion`（`QImage::copy` 裁剪 + QPainter 水印）后落盘 / 进剪贴板；若坐标已在 FileDialog 异步间隙由 `BeginSaveSession` 暂存，则改调 `CompleteSaveWithPending(path)`

---

## 6. 核心数据流

### 6.1 录制流程

```
GUI 点击"录制"
  → RecorderViewModel::start()
    → RecordingService::StartFromUiState(UiState)
      → 校验 + 组装 RecorderOptions
      → ScreenRecorder::Start(0)
        → VideoSourceFactory::Create / AudioSourceFactory::Create
        → Mp4Writer::Alloc → WriteHeader
        → video_->Run(duration, capture_start_, stop_flag)     [线程1]
        → audio_->Run(capture_start_, stop_flag)               [线程2，仅录制音频时]
          ↓
      ┌───────────────────────────┐
      │ 每个采集线程内部循环：     │
      │ av_read_frame             │
      │ → avcodec_send/receive    │
      │ → sws_scale/swr_convert   │
      │ → FIFO 缓冲（音频）       │
      │ → 水印（视频）             │
      │ → 算 PTS                  │
      │ → writer_->EncodeAndWrite    │ ← 内部互斥锁
      └───────────────────────────┘
          ↓
GUI 点击"停止"
  → RecorderViewModel::stop() → RecordingService::Stop() → ScreenRecorder::Stop()
  → stop_requested_ = true
  → Run 循环内 av_read_frame 返回 AVERROR_EXIT（中断回调）
  → writer_->EncodeAndWrite(nullptr) flush 编码器
  → ScreenRecorder::Close()
    → video_->Close / audio_->Close
    → writer_->Close() → trailer → avio_closep → avformat_free_context
```

### 6.2 截图流程

```
GUI 点击"截图"
  → RecorderViewModel::takeScreenshot(path)
    → ScreenshotService::TakeFullscreenPng(path, true, &saved_path)  // 自动补 .png / 用默认名
      → Screenshot.h: CaptureDesktopFrame(&frame)
        → gdigrab avformat_open_input("desktop")
        → av_read_frame 1 帧 → BMP decoder → BGRA/BGR0 帧
      → DrawLabelWatermark + DrawTimestampWatermark
      → FrameToQImage(frame) → img.save(path, "PNG")  // Qt 内置 PNG 编码
      → av_frame_free(&frame)
```

### 6.3 播放流程

```
QML → controller.playInBuiltInPlayer()
  → emit playInBuiltInPlayerRequested(path)   // QML 先赋 videoSource
  → player_window_->show()                    // Qt 5.12 必须 C++ show()
  → PlayerWindow.startPlayback() → VideoPlayerItem::open(path) → PlayerViewModel::open()
    → PlaybackDecoder::open() + start()          // QThread，自动 run()
      → avformat_open_input → av_find_best_stream → open decoder
      → sws_getContext (任何格式 → RGB24)
      → 音频可选：open decoder + swr(S16LE) + AudioPlayer::open
    → play()
      → timer_->start(1000/fps/speed)
      → onTimer() 每 tick:
          target_pts = qMax(audioClockMs(), position_ms_)  // 音频时钟优先
                       或 start_pts_ + elapsed * speed      // 降级墙钟
          while (queue frame && frame.pts <= target_pts):
              dequeue → current_frame_ → emit frameReady()
          pending_frame_ 未到时间的帧留待下次 tick
  → VideoPlayerItem::onFrameReady()               // GUI 线程：加锁快照 pending_frame_
    → update()                                // 调度渲染线程
      → updatePaintNode()                      // 渲染线程
          createTextureFromImage → QSGImageNode::setTexture
          setRect(等比居中) → GPU 采样缩放绘制
```

---

## 7. 线程模型

| 线程 | 运行循环 | 退出条件 | 清理 |
|------|----------|----------|------|
| GUI 主线程 (QGuiApplication) | QML 事件循环 | app.exec() 返回 | — |
| 视频采集线程 | `IVideoSource::Run()` | `stop_flag_` 置位 → av_read_frame 被中断回调中止 → AVERROR_EXIT | `video_->Close()` |
| 音频采集线程 | `IAudioSource::Run()` | 同上 | `audio_->Close()` |
| 摄像头线程（仅 CompositeVideoSource） | `CameraThread()` | `cam_stop_` + `stop_flag_` 置位 | `cam_thread_.join()` |
| 解码线程（播放器，由 `PlayerViewModel` 持有） | `PlaybackDecoder::run()` | `stop_` 置位 或 `av_read_frame` EOF | `~PlaybackDecoder` 调 `stop() + wait() + close()` |
| Qt 音频线程（`QAudioOutput` pull 回调，仅播放器） | `AudioPullDevice::readData()` → `AudioPlayer::pullRead()` | `QAudioOutput::stop()` / 销毁（`AudioPlayer::pause()` 走 `suspend()`，`stop()/close()` 置 `stop_` 使 `pullRead` 返回 0） | `QAudioOutput` / `AudioPullDevice` 由 `AudioPlayer::close()` 搬运回主线程销毁 |
| 场景图渲染线程（threaded render loop） | `VideoPlayerItem::updatePaintNode()` | 节点释放 / 窗口关闭 | 纹理由 `QSGImageNode`（`ownsTexture`）自动回收 |

**中断回调机制（ffmpeg I/O interrupt）：**
所有录制 Source 的 dshow / gdigrab 输入上下文在 `avformat_open_input` 前手动 `avformat_alloc_context` 并设置 `interrupt_callback.callback = InterruptCallback`。当 `stop_flag_` 为 true 时回调返回 1，FFmpeg 内部阻塞的 `av_read_frame` 立即返回 `AVERROR_EXIT`。

**线程安全汇总：**
- `Mp4Writer::EncodeAndWrite` 内部互斥锁（视频 / 音频线程唯一共享点）
- `CompositeVideoSource::cam_mutex_` 保护 `cam_small_` / `cam_has_frame_`
- `PlaybackDecoder::mutex_ + QWaitCondition` 保护帧队列
- `AudioPlayer::mutex_ + cond_` 保护环形缓冲（解码线程 write ↔ Qt 音频线程 `pullRead`）
- `VideoPlayerItem::frame_mutex_` 保护 GUI 线程 → 渲染线程的帧快照（`pending_frame_` / `pending_seq_`）
- `RecorderViewModel` 的所有操作在 GUI 线程执行，无跨线程写

---

## 8. PTS 对齐策略

视频和音频分别从 `ScreenRecorder::capture_start_` 作为共同墙钟零点起算，各自独立但同一起点：

| 轨 | PTS 策略 | 公式 | 备注 |
|----|----------|------|------|
| 视频 | 墙钟真实经过时间（扣除暂停总时长） | 每帧 `pts = llround(elapsed * fps / speed)`，撞槽强制 +1 保单调 | 倍率模式下墙钟 N 秒 → 1 秒视频；实际采集帧率低于标称 fps 时时间轴仍贴合真实时间 |
| 音频 | 墙钟真实经过时间（扣除暂停总时长），与视频同口径 | 每个 DrainFifo 帧 `pts = llround(elapsed * sample_rate / speed)`（flush 残帧改用 `prev_pts_ + frame_size` 接续），撞槽强制 +1 保单调 | 不跟随声卡采样计数，长录制无晶振偏差累积 |

**暂停期间的 PTS 行为：**
- 视频：gdigrab 拉模型不采集；dshow 摄像头推流模型则持续读包丢弃。暂停时长计入 `total_paused_`，Resume 后墙钟换算自然接续
- 音频：声卡仍在推流但读出即丢弃，暂停时长计入 `total_paused_`；Resume 后墙钟换算自然接续

---

## 9. FFmpeg 资源管理策略

**统一原则：**
- 每个 Source 的 `Close()` 使用 `if (ptr)` 守卫，可安全处理未完全初始化的状态
- 每个 Source 的 `Open()` 失败路径调 `Close()` 兜底（上一轮修复确认）
- `avformat_alloc_context` 在 `avformat_open_input` 前调用，失败时 `avformat_open_input` 会自行释放并置空

**资源分配 / 释放配对表：**

| 分配 | 释放 | 典型位置 |
|------|------|----------|
| `av_packet_alloc()` | `av_packet_free()` / `av_packet_unref()` | Run 循环入口 / 每次 send_packet 后 |
| `av_frame_alloc()` | `av_frame_free()` / `av_frame_unref()` | Run 循环入口 / 每次 receive_frame 后 |
| `avformat_alloc_context` + `avformat_open_input` | `avformat_close_input` | 各 Source::OpenInput（截图路径直接 `avformat_open_input`） |
| `avcodec_alloc_context3` + `avcodec_open2` | `avcodec_free_context` | OpenInput + OpenEncoder |
| `avformat_alloc_output_context2` | `avformat_free_context` | Mp4Writer::Alloc → Close |
| `sws_getContext` | `sws_freeContext` | VideoSource InitScaler |
| `swr_alloc` + `swr_init` | `swr_free` | AudioSource InitResampler |
| `av_audio_fifo_alloc` | `av_audio_fifo_free` | AudioSource InitResampler |
| `avfilter_graph_alloc` + `avfilter_inout_alloc` | `avfilter_graph_free` + `avfilter_inout_free` | PlaybackDecoder 音频 atempo 变速图 |
| `av_samples_alloc_array_and_samples` | `av_freep(&conv[0])` + `av_freep(&conv)` | Run 循环内每次 swr_convert |
| `av_dict_set*` | `av_dict_free` | avformat_open_input 前后 |

---

## 10. 设计模式总结

| 模式 | 应用 |
|------|------|
| **简单工厂** | `VideoSourceFactory` / `AudioSourceFactory` 按运行时后端名（`RecorderOptions.video_backend` / `audio_backend`）创建具体采集源，未登记名称返回 `nullptr` |
| **策略模式** | IVideoSource / IAudioSource 接口 + 多实现（gdigrab / dshow / composite / mixed） |
| **单一职责** | 每个 Source 只管采集 + 编码，Mp4Writer 只管封装 + 写盘，ScreenRecorder 只管编排 |
| **观察者 / 信号槽** | QML ↔ RecorderViewModel / PlayerViewModel 通过 Qt signals/slots 双向通信 |
| **MVVM** | View：`qml/*.qml` + `VideoPlayerItem`（纯绘制）；ViewModel：`RecorderViewModel` / `PlayerViewModel`（状态机 + 命令转发）；Model：`src/model/service/*`（业务规则）+ `ScreenRecorder` / `PlaybackDecoder` / `src/model/capture/*`（基础设施） |
| **RAII 兜底清理** | 各 Source::Close 可安全处理部分初始化状态；多数 Source 的 OpenInput 使用 `do-while(false) + if(!ok) Close()` 模式，失败路径零手动清理代码；`MixedAudioSource::OpenOneInput` 则在每个失败分支自行清理上游资源（模范写法） |
| **中断回调** | FFmpeg `interrupt_callback` 让阻塞采集能被停止请求打断 |

---

## 11. 已知限制与设计决策

| 项目 | 决策 / 限制 |
|------|------------|
| 编码器 | GUI STA 下 h264_mf 不可用，统一 mpeg4 纯软件 |
| 水印实现 | 不走 libavfilter drawtext（需 FreeType 支持），统一由 `Watermark.h` 用 QPainter 画在采集帧上 |
| 系统声音采集 | 依赖立体声混音 / 虚拟声卡。本机已启用“立体声混音 (Realtek(R) Audio)”，选中该设备即可录制系统播放声音；换机器若缺少此设备，需在 Windows 声音设置启用或安装虚拟声卡 |
| 光标闪烁 | gdigrab 硬编码 BITBlt SRCCOPY\|CAPTUREBLT，无法参数化关闭 |
| 暂停期间 buffer | 音频 / dshow 视频暂停时持续读包丢弃（防 rtbufsize 溢出） |
| 高分屏坐标换算 | 区域截图 / 区域录制用 overlay 尺寸比例换算 QML 逻辑坐标 → 物理像素 |
| FFmpeg 版本 | 外部 32-bit DLL，位于项目内 `depends/ffmpeg/bin`，构建后自动拷到 exe 同目录，不与源码一起构建 |
| 播放器渲染 | `QQuickItem` + `updatePaintNode` + `QSGImageNode`；Qt 5.12 无纹理原地更新接口，每帧新建纹理（30fps 下开销可接受），缩放交给 GPU |
| 播放器解码 | 不用 QtMultimedia `MediaPlayer`：Qt 5.12 DirectShow 后端依赖系统 MP4 解码器，本机缺失（0x80040266），且无法实现 atempo 倍速与音频时钟同步 |
