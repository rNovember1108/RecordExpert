# RecordExpert —— 架构与代码分析

> 本文档整理 RecordExpert 录屏工具的整体架构、模块职责、关键数据流与设计决策，供维护者快速理解代码组织与扩展点。

---

## 1. 项目概述

RecordExpert 是一个 Windows 桌面录屏工具，支持全屏 / 指定区域 / 指定窗口录制、可选摄像头画中画、麦克风 + 系统声音混音，以及内置视频播放与区域截图。

**核心能力：**
- 桌面录屏（gdigrab）或摄像头录制（dshow），可选桌面 + 摄像头画中画合成
- 麦克风采集（dshow）或麦克风 + 系统声音混音（双路 dshow 设备等权叠加）
- 倍率录制（time-lapse，支持 1/2/4/8）
- 分段录制（segment_duration_seconds > 0 时按时长自动切分多个 MP4，`output_001.mp4` / `output_002.mp4` …，中途不停采集设备、不重建编码器、不丢帧）
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
│ 三线程编排    │     │gdigrab+QPainter│      │ 状态机/时钟/帧调度   │
└──────┬───────┘     └──────────────┘       └──────────┬───────────┘
       │                                               │
       ▼                                               ▼
┌─────────────────────────────┐            ┌──────────────────────┐
│ Mp4Writer（仅 muxer 线程调用，│            │ PlaybackDecoder (Model) │
│  单线程无锁）                 │            │ avformat+sws+atempo  │
└──────────┬──────────────────┘            └──────────┬───────────┘
           │                               └──────────┬───────────┘
  ┌────────┴─────────┐                               │
  ▼                  ▼                               ▼
┌─────────────┐ ┌─────────────┐           ┌──────────────────────┐
│IVideoSource │ │IAudioSource │           │ AudioPlayer          │
│ 视频采集+编码│ │ 音频采集+编码│           │ QAudioOutput (pull)  │
│ (producer)  │ │ (producer)  │           └──────────────────────┘
└──────┬──────┘ └──────┬──────┘
       │ push          │ push
       ▼               ▼
  AVFifo queue_v   AVFifo queue_a   （单生产者-单消费者 lock-free）
       └───────┬───────┘
               ▼
        muxer_worker 消费 → WritePacket(pts-offset) → MP4
```

**组件关系：**
- `RecorderViewModel`（ViewModel）只做 QML 桥接，录制 / 截图 / 枚举业务下沉到 `RecordingService` / `ScreenshotService` / `EnumerationService`；窗口显隐（Qt 5.12 下必须 C++ `show()/hide()`）与 UI 计时器仍留在本类
- `RecordingService` 独占 `ScreenRecorder`（`unique_ptr`），负责 `RecorderOptions` 的组装与前置校验
- `ScreenRecorder` 持有 `Mp4Writer`（仅 muxer 线程调用）、一个 `IVideoSource`、一个可选的 `IAudioSource`，以及两个 AVFifo 队列（queue_v / queue_a）
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
    ├── ScreenRecorder.{h,cpp}  # 录制会话编排（三线程：video/audio producer + muxer consumer）
    ├── Mp4Writer.{h,cpp}       # 纯 muxer 层（OpenMuxer/WritePacket/RotateMuxer/Close，单线程无锁）
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

### 5.1 ScreenRecorder —— 录制编排器（方案 C：双生产者 + 单消费者）

**职责：**
- 持有 `Mp4Writer`（唯一 muxer）、两条采集编码源（`IVideoSource` / `IAudioSource`）、两个 AVFifo 队列
- `Start()` 内完成 `Open()`：创建两轨源并 `Open()`（各源持有全程唯一的 encoder）→ `writer_.OpenMuxer(path, {enc_v, enc_a?})` 写首个文件头 → 创建 AVFifo → `SetQueue/SetStream/SetFlushDoneFlag` 注入 producer 依赖 → 启动三个 worker
- **encoder 全程一个**（segment 边界不重建），分段切换只换 muxer（stream）+ 手动 pts 偏移

**关键接口：**
- `Start(duration)` — Open → 启动三个 worker，立即返回
- `Stop()` — `stop_requested_` → join producer（各源 Run 末尾 flush encoder，尾包全进队列）→ join muxer（最终 drain）→ Close（写 trailer）
- `Pause()` / `Resume()` — 转发给 video_ 和 audio_

**三线程模型：**

| 线程 | 职责 | 退出条件 |
|------|------|----------|
| `video_worker_`（producer） | `video_->Run(0, start, stop, flush_requested_v)`：采集→解码→sws→水印→send_frame/receive_packet→rescale→push queue_v_ | `stop_requested_`；segment 边界时响应 `flush_requested_v` flush 后等重置 |
| `audio_worker_`（producer） | 同上，push queue_a_ | 同上（`flush_requested_a`） |
| `muxer_worker_`（consumer） | **唯一触碰 Mp4Writer 的线程**：drain 两队列 → `WritePacket(stream, pts_offset, pkt)` → 墙钟检测 segment 边界 → 协调 flush → `RotateMuxer` → 刷新 stream/offset | `stop_requested_ && producers_done_v_ && producers_done_a_`（保证收尾 drain 时尾包全部到齐，不丢尾帧） |

**segment 边界协调协议（muxer_worker 主导）：**
1. `flush_done_v/a = false`，`flush_requested_v/a = true`
2. 等 producer flush 完（`flush_done_v/a == true`，超时 5s 强制停止）
3. drain 两队列（flush 出的尾包用旧 offset 写入旧 segment）
4. 新 offset = 本轨已写入的最大原始 pts + 1（`DrainFifoAndWrite` 返回值，muxer 权威跟踪，与 encoder flush 是否产出残包无关）
5. `RotateMuxer(next_path, encs, new_streams)`：旧文件 trailer → 新文件 header
6. `SetStream` 刷新 muxer 与 producer 持有的 stream 指针；更新 pts_offset
7. 重置段计时 `seg_active = 0`（暂停冻结的段内活跃时长）、`flush_requested_ = false`（producer 恢复采集）

**边界检测使用"段内活跃时长"（暂停冻结）：** muxer_worker 每轮累加 `seg_active`，`paused_` 为 true 时不推进。因此暂停期间永远不触发分段边界，边界顺延到 Resume 之后；触发时 producer 一定处于可响应状态，不会出现"暂停中超时误停整场录制"。`capture_start_` 全录制期间不变，仅作为 producer 的墙钟 PTS 零点。

**producer 侧 flush 分支的两个关键约束（5 个 Source 一致）：**
- flush 检查位于 paused 检查**之前**：若边界触发瞬间用户按下暂停，producer 若先进入 paused 分支将收不到 `flush_requested`，muxer 5s 超时误停整场。
- flush 期间**不清理暂停状态**（`paused_`/`pause_start_`/`total_paused_`）：`total_paused_` 是墙钟 PTS 的扣减基准，清零会导致边界后 PTS 跳变；强行解除暂停会让 UI 状态与实际不符。
- flush 后调用 `avcodec_flush_buffers(enc_ctx_)` 退出 drain 模式（`send_frame(nullptr)` 之后 `send_frame` 一律返回 `AVERROR_EOF`），否则首个分段边界后 encoder 永久失效、录制停止。receive 循环的 EOF/EAGAIN 必须 `break` 而不能提前 `return`，否则 flush_buffers 执行不到。

**同步原语：**
- `flush_requested_v/a`：muxer → producer，请求 flush
- `flush_done_v/a`：producer → muxer，flush 完成
- 新段 offset 的权威来源：muxer 侧 `last_raw_v/a`（各轨已写入的最大原始 pts），不经过 producer 原子变量
- `producers_done_v/a`：producer Run 返回后置位，muxer 用来安全收尾（防止 Stop 时 muxer 抢先写 trailer 丢尾包）；边界条件同时含 `!producers_done_*`，producer 异常退出后不再轮转空段

### 5.2 Mp4Writer —— 纯 muxer 层（单线程无锁）

**职责（仅被 muxer_worker 线程调用，内部不需要任何锁）：**
- 管理 `AVFormatContext* out_fmt_ctx_`（由 `avformat_alloc_output_context2` 创建）
- `OpenMuxer(path, encs, out_streams)` — Alloc + 逐 encoder `avformat_new_stream`（codecpar 拷贝 + `stream->time_base = enc->time_base`）+ `avio_open` + `avformat_write_header`；新 stream 指针按 encs 顺序输出
- `WritePacket(stream, pts_offset, pkt)` — **唯一写包入口**：`pkt.stream_index = stream->index`，`pts/dts -= pts_offset`，`av_interleaved_write_frame`。packet 由 producer 预先 rescale 到 stream time_base
- `RotateMuxer(next_path, encs, out_streams)` — 旧文件 `av_write_trailer` + `avio_closep` + free → 复用 `OpenMuxer` 建新文件；**encoder 不动，只重建 stream**（out_fmt_ctx_ 换了旧 stream 指针失效）
- `Close()` — 写 trailer（仅一次）→ `avio_closep` → `avformat_free_context`；幂等
- `NeedsGlobalHeader()` — muxer 是否要求全局头（MP4 需要，OpenMuxer 内自动给 encoder 补 `AV_CODEC_FLAG_GLOBAL_HEADER`）

**关键设计：**
- Mp4Writer 完全不知道"编码器"和"分段"的存在——它只收已经编码好的 packet 和调用方算好的 pts_offset
- 旧的 `NewStream` / `EncodeAndWrite` / `write_mutex_` 已移除：编码挪进 producer（encoder 全程一个），互斥锁因单线程化而不再需要

### 5.3 IVideoSource 体系

**抽象接口（`i_video_source.h`）—— producer 角色，只管"采集→编码→push 队列"，全程不触碰 muxer：**
- `SetQueue(AVFifo*)` — Open 前注入输出队列
- `SetStream(AVStream*)` — Open 后/Rotate 后注入流指针（rescale 用）
- `SetFlushDoneFlag(std::atomic<bool>*)` — flush 完置位通知 muxer
- `Open()` / `Close()`
- `Run(duration, start, stop, flush_requested)` — 阻塞式主循环；`duration=0` 表示无限（segment 边界由 muxer 的 `flush_requested` 驱动）
- `Pause()` / `Resume()`
- `EncCtx()` — 暴露 encoder 上下文（ScreenRecorder 建 muxer stream 用）

**三个具体实现：**

| 类 | 采集后端 | 水印 | 缩放 | 备注 |
|----|----------|------|------|------|
| `GdiGrabVideoSource` | gdigrab 桌面 | QPainter 直接画在采集帧上（`kWatermarkLabel` 左上角 + 右下角时间戳） | sws → YUV420P | 无 libavfilter 路径，水印统一复用 `Watermark.h` |
| `DShowVideoSource` | dshow 摄像头 | 无 | sws → YUV420P | 暂停期间持续读包丢弃（防 rtbufsize 溢出） |
| `CompositeVideoSource` | gdigrab 主 + dshow 副 | 桌面帧叠加 QPainter 水印 | 主 sws + 副 sws | 摄像头独立线程 drop-old 只留最新帧，主循环每帧 blit 到右下角 PiP 区 |

**共同特征（方案 C producer 模式）：**
- encoder（`enc_ctx_`）全程一个，segment 边界不重建；`AV_CODEC_FLAG_GLOBAL_HEADER` 在 Open 时预置（幂等，Mp4Writer::OpenMuxer 会再确认）
- `EncodeFrameToQueue(frame)`：`avcodec_send_frame` + 循环 `receive_packet`（EAGAIN/EOF 正常）→ `av_packet_rescale_ts` 到 `stream_->time_base` → `av_fifo_write(queue_)` → `av_packet_unref`。FIFO 满时 pkt 未 unref 可安全重试（最多 ~200ms，防 muxer 卡死死等；超时丢包——PTS 是墙钟，丢帧不破坏时间轴）
- `FlushEncoderToQueue()`：`send_frame(nullptr)` + receive 循环，每个包 push 队列；EOF/EAGAIN `break`（不能提前 return），末尾 `avcodec_flush_buffers` 退出 drain 模式
- Run 主循环每轮检查 `flush_requested`：置位时 flush → `flush_done = true` → spin-wait 等 muxer Rotate 后重置 → 继续采集（**这一段不采集帧**，段间有亚秒级间隙，属预期行为）
- Run 退出前（stop 路径）也 flush 一次，尾包全部进队列，之后 worker lambda 才置 `producers_done_`
- 输入 `AVFormatContext*` 在 `avformat_open_input` 前手动 `avformat_alloc_context` 并挂中断回调（`stop_flag_` 置位时返回 1 打断阻塞采集——5 个 Source 统一此模式）

### 5.4 IAudioSource 体系

**抽象接口（`i_audio_source.h`）—— 与 IVideoSource 同构的 producer 模式**：`SetQueue` / `SetStream` / `SetFlushDoneFlag` / `Open` / `Run(duration, start, stop, flush_requested)` / `Close` / `Pause` / `Resume` / `EncCtx`。

**两个具体实现：**

| 类 | 采集后端 | 重采样 | FIFO | 备注 |
|----|----------|--------|------|------|
| `DShowAudioSource` | 单路 dshow | SwrContext → FLTP | 一个 AVAudioFifo | `DrainFifo()` 攒够 1024 样本喂 AAC，flush 时补齐残帧 |
| `MixedAudioSource` | 双路 dshow | 两路各自 SwrContext | 两个输入 FIFO + 一个输出 FIFO | `PumpOne()` 非阻塞读两路，`MixOne()` 等权 0.5+0.5 clamp，`MixOne` 内含 resync 机制（一路超阈值时截断较长侧） |

**segment 边界的音频特有处理（flush_requested 分支内）：**
- `DrainFifo(true)`：把 AVAudioFifo 里不满 1024 样本的残帧补齐编出并 push 队列，然后 `FlushEncoderToQueue`
- `av_audio_fifo_reset`：清空跨段残帧（残帧若用旧墙钟打 PTS 会与下段时间轴错乱）
- 编出的尾包由 muxer 用旧 offset 写入旧 segment

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
      → 校验 + 组装 RecorderOptions（含 segment_duration_seconds）
      → ScreenRecorder::Start(0)
        → Open()：
          → VideoSourceFactory::Create / AudioSourceFactory::Create（工厂不再接收 writer）
          → video_->Open() / audio_->Open()        // 各源创建全程唯一的 encoder
          → writer_.OpenMuxer(path, {enc_v, enc_a?}, out_streams)  // 写首个文件头
          → av_fifo_alloc2 × 2（queue_v / queue_a，各 16 个 AVPacket 槽位）
          → SetQueue / SetStream / SetFlushDoneFlag 注入
        → 启动三线程：
          video_worker_  → video_->Run(0, start, stop, flush_requested_v)   [producer]
          audio_worker_  → audio_->Run(0, start, stop, flush_requested_a)   [producer，可选]
          muxer_worker_  → 主循环                                            [consumer]

  ┌─ producer 循环（每轨独立）────────────────────────┐
  │ av_read_frame → send/receive 解码                  │
  │ → sws_scale / swr_convert → AVAudioFifo（音频）    │
  │ → 水印 / PiP 合成（视频）                          │
  │ → 算 PTS（墙钟，扣暂停；跨段单调递增不重置）        │
  │ → EncodeFrameToQueue：send_frame/receive_packet    │
  │   → rescale 到 stream time_base → push AVFifo      │
  │   （encoder 全程一个，从不重建）                    │
  │                                                    │
  │ if (flush_requested)：                             │
  │   FlushEncoderToQueue（残包全 push + flush_buffers 退出 drain）│
  │   → flush_done = true → 等 flush_requested 复位     │
  └────────────────────────────────────────────────────┘
          ↓ AVFifo（单生产者-单消费者，lock-free）
  ┌─ muxer_worker 循环（唯一触碰 Mp4Writer 的线程）───┐
  │ drain queue_v / queue_a                            │
  │   → WritePacket(stream, pts_offset, pkt)：          │
  │     pkt.pts -= pts_offset → av_interleaved_write_frame│
  │                                                    │
  │ 段内活跃时长 seg_active >= seg_dur（暂停冻结，       │
  │ 分段模式）：                                         │
  │   flush_requested_v/a = true                        │
  │   → 等 flush_done_v/a（超时 5s 强制停止）           │
  │   → 再 drain（尾包用旧 offset 写旧段）              │
  │   → 新 offset = 本轨已写入最大原始 pts + 1          │
  │   → RotateMuxer(output_00N.mp4)：旧 trailer→新 header│
  │   → SetStream 刷新 muxer/producer 的 stream 指针    │
  │   → seg_active=0 / flush 复位                       │
  └────────────────────────────────────────────────────┘

GUI 点击"停止"
  → RecorderViewModel::stop() → RecordingService::Stop() → ScreenRecorder::Stop()
  → stop_requested_ = true
  → join video_worker_ / audio_worker_（Run 退出前 flush encoder，尾包全进队列；
    worker lambda 置 producers_done_）
  → join muxer_worker_（主循环条件含 producers_done_，最终 drain 不丢尾包）
  → Close()
    → video_->Close / audio_->Close（free encoder/decoder/输入）
    → writer_.Close() → trailer → avio_closep → avformat_free_context
    → av_fifo_freep2 × 2
```

**分段输出命名：** `output.mp4` → `output_001.mp4` / `output_002.mp4` …（`BuildSegmentPath` 保留目录与扩展名，只插序号）。`segment_duration_seconds == 0` 时单文件，行为与旧版一致。

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

### 6.4 混合录制完整流程（CompositeVideoSource + MixedAudioSource）

混合录制同时启用 **两路视频融合（桌面 + 摄像头画中画）** 和 **两路音频融合（麦克风 + 系统声音）**，最终写入同一个 MP4。两条合成链路各自独立处理，共享 `ScreenRecorder` 的墙钟零点；各自编码后经 AVFifo 队列（queue_v / queue_a）汇聚到唯一的 `muxer_worker` 线程交错写入。

#### 触发条件

UI 选择「Composite」视频设备 + 「Mixed」音频设备时触发。后端映射：
- `video_backend = "composite"` → `VideoSourceFactory` 创建 `CompositeVideoSource`
- `audio_backend = "mixed"` → `AudioSourceFactory` 创建 `MixedAudioSource`

#### 初始化阶段（一次性）

```
ScreenRecorder::Start() → Open()
├─ video_->Open()                                 // CompositeVideoSource::Open()
│   ├─ avdevice_register_all()
│   ├─ OpenMainInput()     [gdigrab 桌面]         // avformat_alloc_context + interrupt_callback +
│   │                                              // avfind_input_format("gdigrab") + avformat_open_input("desktop")
│   │                                              // → av_find_best_stream → open BMP 解码器 → main_dec_
│   ├─ OpenCameraInput()   [dshow 摄像头]          // avformat_alloc_context + interrupt_callback +
│   │                                              // avfind_input_format("dshow") + avformat_open_input("video=<name>")
│   │                                              // → av_find_best_stream → open 驱动对应解码器 → cam_dec_
│   ├─ OpenEncoder()       [mpeg4]                 // enc_ctx_: width/height(偶数对齐) + pix_fmt=YUV420P
│   └─ InitScalers()                              //
│       ├─ sws_main_:  main_dec_->pix_fmt → YUV420P(全尺寸)   // 桌面 BGRA → 编码格式
│       ├─ sws_cam_:   cam_dec_->pix_fmt  → YUV420P(pip_w×pip_h) // 摄像头 → 画中画小尺寸
│       ├─ 计算 pip 布局: pip_w = W/4, pip_h = H/4, 右下角 16px 边距, 坐标对齐偶数
│       ├─ 分配 cam_small_ (YUV420P 小帧, CameraThread 写入)
│       └─ 分配 yuv_frame_ (YUV420P 全帧, 主循环复用, 融合输出)
├─ audio_->Open()                                 // MixedAudioSource::Open()
│   ├─ OpenEncoder()       [AAC FLTP/48k/2ch]      // enc_ctx_: frame_size=1024
│   ├─ OpenOneInput(麦克风, &in_a_ctx_, &dec_a_, &swr_a_)  // dshow audio + 解码器 + SwrContext→FLTP
│   ├─ OpenOneInput(系统声音, &in_b_ctx_, &dec_b_, &swr_b_) // 同上, 另一路 dshow audio
│   └─ 分配 3 个 FIFO: fifo_a_, fifo_b_ (输入缓冲) + fifo_out_ (混音后缓冲)
└─ writer_.OpenMuxer(output_path, {enc_v, enc_a}, &out_streams)
                                                 // Alloc + NewStream×2 + WriteHeader
                                                 // （在两源 Open 之后，encoder 就绪才建流）

ScreenRecorder 启动三个 worker:
  std::thread video_worker_  { video_->Run(...) }   // 视频合成 producer
  std::thread audio_worker_  { audio_->Run(...) }   // 音频合成 producer
  std::thread muxer_worker_  { 主循环 }              // 唯一触碰 Mp4Writer 的 consumer
  // video_worker 内部再拉起 std::thread cam_thread { CameraThread() }
```

#### 运行阶段（数据流）

**视频链路（三条并行流 → 合成 → 编码）**

```
         桌面(gdigrab, 主线程)                摄像头(dshow, CameraThread)
         ───────────────────                 ──────────────────────────
         av_read_frame(main_in_)             av_read_frame(cam_in_)
         ┌──────────────────┐                ┌──────────────────┐
         │ AVPacket(BMP)    │                │ AVPacket(驱动格式)│
         └────────┬─────────┘                └────────┬─────────┘
                  │                                    │
         avcodec_send/receive                  avcodec_send/receive
                  │                                    │
         ┌────────▼─────────┐                ┌────────▼─────────┐
         │ dec_frame(BGRA)  │                │ dec(驱动pix_fmt)  │
         └────────┬─────────┘                └────────┬─────────┘
                  │                                    │
         DrawLabelWatermark                       sws_scale(sws_cam_)
         DrawTimestampWatermark                   (格式转换 + 缩到 pip_w×pip_h)
                  │                                    │
         sws_scale(sws_main_)                   ┌────────▼─────────┐
         (BGRA → YUV420P 全尺寸)                │ cam_small_       │
                  │                              │ YUV420P 小帧     │
         ┌────────▼─────────┐                    └────────┬─────────┘
         │ yuv_frame_       │                             │
         │ YUV420P 全尺寸   │    cam_mutex_ + drop-old   │
         └────────┬─────────┘ ◄──────────── memcpy ──────┘
                  │                              (BlitPiP: Y/U/V 三平面)
                  │                               逐行拷贝到 (pip_x_, pip_y_) 区域
                  ▼
         算 PTS = llround(elapsed × fps / speed)
                  │
                  ▼
         EncodeFrameToQueue(yuv_frame_)
                  │   内部: avcodec_send_frame
                  │         → avcodec_receive_packet
                  │         → av_packet_rescale_ts(stream_->time_base)
                  │         → av_fifo_write(queue_v_)   [pkt 满时重试 ~200ms]
                  ▼
         ─── AVFifo queue_v_ ─── → muxer_worker 消费 → WritePacket → MP4
```

**音频链路（两路 dshow 声卡 → FIFO 缓冲 → 混音 → AAC 编码）**

```
         麦克风(dshow)              系统声音(dshow)
         ────────────────          ─────────────────
         PumpOne(in_a_, ...)       PumpOne(in_b_, ...)
         │ av_read_frame            │ av_read_frame
         │ avcodec_send/receive     │ avcodec_send/receive
         │ swr_convert → FLTP      │ swr_convert → FLTP
         └──────────┬──────────┘   └──────────┬──────────┘
                    │                           │
         av_audio_fifo_write(fifo_a_)   av_audio_fifo_write(fifo_b_)
                    │                           │
                    └─────────┬─────────────────┘
                              ▼
                         MixOne()
                         ┌─────────────────────────────┐
                         │ 1. resync: 一路差 >24000    │
                         │    样本时截断较长侧          │
                         │ 2. 取 min(na, nb) 等权混音  │
                         │    out = 0.5×a + 0.5×b     │
                         │ 3. clamp [-1, 1] 防过载爆音 │
                         └─────────────┬───────────────┘
                                       │
                         av_audio_fifo_write(fifo_out_)
                                       │
                                       ▼
                         DrainFifo()  (fifo_out_ ≥ 1024 样本时)
                         ┌─────────────────────────────┐
                         │ 1. av_audio_fifo_read 1024   │
                         │ 2. 倍率下采样 speed>1 抽帧   │
                         │ 3. swr_convert FLTP → AAC   │
                         │ 4. 算 PTS = llround(        │
                         │    elapsed × 48000 / speed) │
                         └─────────────┬───────────────┘
                                       │
                                       ▼
                         EncodeFrameToQueue(frame)
                                       │   内部同视频路径,
                                       │   push av_fifo_write(queue_a_)
                                       ▼
                              ─── AVFifo queue_a_ ─── → muxer_worker 消费
                                       → WritePacket → 同一个 MP4
```

#### 融合点精确位置

| 融合 | 机制 | 文件:行号 | 前置条件 |
|------|------|-----------|----------|
| 摄像头 → 桌面 (视频位置合成) | `BlitPiP()` Y/U/V 三平面逐行 `memcpy` | `composite_video_source.cpp:285-318` | 两路都已是 `YUV420P`；pip 坐标全偶数（YUV420P 色度半平面对齐要求） |
| 麦克风 + 系统声音 (音频等权混音) | `MixOne()` float 缓冲 `0.5a + 0.5b` + clamp | `mixed_audio_source.cpp:255-303` | 两路都已重采样到 `FLTP/48kHz/2ch`；短于长侧 ≤24000 样本（resync 已截断） |
| 视频 + 音频 (MP4 混流) | 两个 producer push 各自 AVFifo，`muxer_worker` 单线程 drain → `WritePacket`（`pts -= offset`）→ `av_interleaved_write_frame` | `ScreenRecorder.cpp` | 两路各自编码后 packet 已 rescale 到各自 `AVStream` 时基；队列深度 16 + 1ms 轮询足够吸收写入抖动 |

#### 关键设计决策

**帧率解耦与 drop-old（视频摄像头帧）**
- 摄像头在独立 `CameraThread` 采集、解码、`sws_scale` 缩到 pip 尺寸
- 主循环每帧 `BlitPiP()` 时通过 `cam_mutex_` 读 `cam_small_`，**只取最新一帧，中间帧直接覆盖丢弃**
- 桌面采集（gdigrab 按需 BitBlt，帧率由主循环 pace）不会被摄像头慢帧阻塞
- 代码位置：`composite_video_source.cpp:269-277`（drop-old 写入）、`:406-407`（主循环读）

**PumpOne 非阻塞泵入（音频两路）**
- `PumpOne` 遇到 `AVERROR(EAGAIN)` 返回 1 让上层继续下一轮；遇到 `AVERROR_EXIT` 也返回 1（正常收尾）；硬错误才返回 0
- 外层循环每轮依次 Pump 两路 → MixOne → DrainFifo，**不等待某一路凑够固定样本数才推进**
- 代码位置：`mixed_audio_source.cpp:193-252`

**resync 策略（音频两路）**
- `kResyncThreshold = 24000` 样本（≈0.5s @48kHz），一路 FIFO 超过另一路时截断长侧
- 防止某一路设备暂时停推/慢推时，另一路无限堆积造成相位漂移或内存膨胀
- 代码位置：`mixed_audio_source.cpp:31`、`:261-265`

**暂停期间持续读包（两路 dshow 推流设备）**
- gdigrab 是拉模型，暂停时主循环 sleep 不抓帧即可
- dshow（摄像头 + 两路声卡）是推流模型，暂停期间必须持续 `av_read_frame` + `av_packet_unref` 丢弃，否则 `rtbufsize=64MB/16MB` 的内部缓冲区持续堆积并在长暂停时溢出丢包
- 摄像头暂停逻辑：`composite_video_source.cpp:232-243`（CameraThread 内）、`:330-344`（主 Run 循环内）
- 音频暂停逻辑：`mixed_audio_source.cpp:325-345`

**PTS 墙钟换算（两轨同口径）**
- 视频：`pts = llround(elapsed × fps / speed)`，撞槽强制 +1 保单调
- 音频：`pts = llround(elapsed × 48000 / speed)`，flush 残帧用 `prev_pts + 1024` 接续
- 两轨共享 `ScreenRecorder::capture_start_` 墙钟零点（先写后启动线程，无锁）
- 暂停总时长从墙钟扣除，Resume 后自然接续，不累积晶振偏差
- **分段录制下 producer 的 PTS 跨段单调递增不重置**；每个 segment 文件内的 PTS 由 muxer_worker 写包时减去 `pts_offset` 归零（见 §8）

#### 停止收尾流程

```
ScreenRecorder::Stop()
├─ stop_requested_ = true                    // 让 InterruptCallback 对所有 dshow/gdigrab 返回 1
├─ video_worker_.join()                      // 等视频 producer 退出
│   └─ Run 内部顺序:
│      av_read_frame → AVERROR_EXIT (中断回调)
│      BlitPiP 最后一帧 → EncodeFrameToQueue(yuv_frame_)
│      FlushEncoderToQueue()                 // flush 编码器，尾包全部 push queue_v
│      → lambda 置 producers_done_v_ = true
├─ audio_worker_.join()                      // 等音频 producer 退出
│   └─ Run 内部顺序:
│      av_read_frame → AVERROR_EXIT
│      DrainFifo(true)                       // flush fifo_out_ 残帧 + AAC 编码器
│      → lambda 置 producers_done_a_ = true
├─ muxer_worker_.join()                      // 主循环条件: stop && producers_done_v/a
│   └─ producer 都退出后才做最终 drain（尾包不丢）→ 写包完毕
├─ Close()                                   // 幂等清理
│   ├─ video_->Close()                       // Close 两路 in_fmt + dec + enc + sws + cam_thread_
│   ├─ audio_->Close()                       // Close 两路 in_fmt + dec + swr + fifo + enc
│   ├─ writer_.Close()                       // av_write_trailer → avio_closep → avformat_free_context
│   └─ av_fifo_freep2(queue_v_/queue_a_)     // 队列已空，直接释放
└─ recorder_.reset()
```

---

## 7. 线程模型

| 线程 | 运行循环 | 退出条件 | 清理 |
|------|----------|----------|------|
| GUI 主线程 (QGuiApplication) | QML 事件循环 | app.exec() 返回 | — |
| 视频采集编码线程（producer，`video_worker_`） | `IVideoSource::Run()` → push queue_v | `stop_requested_` 置位 → av_read_frame 被中断回调中止 → flush encoder → 返回；segment 边界响应 `flush_requested_v` | `video_->Close()` |
| 音频采集编码线程（producer，`audio_worker_`） | `IAudioSource::Run()` → push queue_a | 同上（`flush_requested_a`） | `audio_->Close()` |
| muxer 线程（consumer，`muxer_worker_`） | drain queue_v/queue_a → `WritePacket` → segment 边界协调 → `RotateMuxer` | `stop_requested_ && producers_done_v && producers_done_a` | 主循环尾部最终 drain；队列由 `Close()` 释放 |
| 摄像头线程（仅 CompositeVideoSource） | `CameraThread()` | `cam_stop_` + `stop_flag_` 置位 | `cam_thread_.join()` |
| 解码线程（播放器，由 `PlayerViewModel` 持有） | `PlaybackDecoder::run()` | `stop_` 置位 或 `av_read_frame` EOF | `~PlaybackDecoder` 调 `stop() + wait() + close()` |
| Qt 音频线程（`QAudioOutput` pull 回调，仅播放器） | `AudioPullDevice::readData()` → `AudioPlayer::pullRead()` | `QAudioOutput::stop()` / 销毁（`AudioPlayer::pause()` 走 `suspend()`，`stop()/close()` 置 `stop_` 使 `pullRead` 返回 0） | `QAudioOutput` / `AudioPullDevice` 由 `AudioPlayer::close()` 搬运回主线程销毁 |
| 场景图渲染线程（threaded render loop） | `VideoPlayerItem::updatePaintNode()` | 节点释放 / 窗口关闭 | 纹理由 `QSGImageNode`（`ownsTexture`）自动回收 |

**中断回调机制（ffmpeg I/O interrupt）：**
所有录制 Source 的 dshow / gdigrab 输入上下文在 `avformat_open_input` 前手动 `avformat_alloc_context` 并设置 `interrupt_callback.callback = InterruptCallback`。当 `stop_flag_`（Run 入口存下的停止请求指针）为 true 时回调返回 1，FFmpeg 内部阻塞的 `av_read_frame` 立即返回 `AVERROR_EXIT`。5 个 Source（gdi / dshow 视频 / composite / dshow 音频 / mixed 音频）统一此模式。

**线程安全汇总：**
- `Mp4Writer` **无锁**：整个录制期只被 `muxer_worker` 单线程调用（方案 C 核心约束）
- AVFifo（queue_v / queue_a）：单生产者-单消费者，FFmpeg 内部 C11 原子 head/tail 实现，无用户态锁；队列中 packet 为值拷贝（AVBufferRef 引用计数跨线程安全）
- `flush_requested_v/a`、`flush_done_v/a`、`producers_done_v/a`：`std::atomic`，muxer ↔ producer 全部同步走原子变量，无互斥锁
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

**分段录制下的 PTS 归零（pts_offset 手动减法）：**
- producer 的墙钟 PTS **跨段单调递增，从不重置**（暂停补偿持续有效，producer 不感知分段）
- muxer_worker 写包时做 `pkt.pts/dts -= pts_offset`（`Mp4Writer::WritePacket` 内），让每个 segment 文件从 PTS≈0 起算，独立可播放
- 每次 `RotateMuxer` 后：`pts_offset = 本轨已写入的最大原始 pts + 1`。该值由 muxer_worker 在 `DrainFifoAndWrite` 里跟踪（返回值累计），**权威且与 encoder flush 是否产出残包无关**——避免"一轨 flush 有残包 offset>0、另一轨无残包 offset=0"导致段内音视频失步。本轨整段零包时继承上一段 offset 而非归零（恢复后首包仍接近 0，不与另一轨失步）；首段零包时旧 offset 为 0，行为不变
- 音视频各自维护独立 offset（`pts_offset_v_` / `pts_offset_a_`），新段内两轨时间轴按各自 encoder 时基对齐，段内 A/V 同步保持不变
- 关键帧衔接：复用 encoder 后新段开头是否有 IDR 取决于 GOP——当前 encoder GOP=2s，segment 时长 ≥ 2s 时新文件开头必落在关键帧上；更短的分段可能出现开头若干帧参考前段数据（播放器一般可容忍，逐帧解码器可能短暂花屏）

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
| `avformat_alloc_output_context2` | `avformat_free_context` | Mp4Writer::OpenMuxer/RotateMuxer → Close |
| `av_fifo_alloc2`（packet 队列） | `av_fifo_freep2` | ScreenRecorder::Open → Close |
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
| **简单工厂** | `VideoSourceFactory` / `AudioSourceFactory` 按运行时后端名（`RecorderOptions.video_backend` / `audio_backend`）创建具体采集源（不含 writer 依赖），未登记名称返回 `nullptr` |
| **策略模式** | IVideoSource / IAudioSource 接口 + 多实现（gdigrab / dshow / composite / mixed） |
| **生产者-消费者** | 双 producer（video/audio worker，各自持有全程唯一 encoder）→ SPSC AVFifo → 单 consumer（muxer_worker，唯一触碰 Mp4Writer）。同步全走 `std::atomic`，无互斥锁 |
| **单一职责** | 每个 Source 只管采集 + 编码 + 入队，Mp4Writer 只管封装 + 写盘 + 换段，ScreenRecorder 只管编排与分段协调 |
| **观察者 / 信号槽** | QML ↔ RecorderViewModel / PlayerViewModel 通过 Qt signals/slots 双向通信 |
| **MVVM** | View：`qml/*.qml` + `VideoPlayerItem`（纯绘制）；ViewModel：`RecorderViewModel` / `PlayerViewModel`（状态机 + 命令转发）；Model：`src/model/service/*`（业务规则）+ `ScreenRecorder` / `PlaybackDecoder` / `src/model/capture/*`（基础设施） |
| **RAII 兜底清理** | 各 Source::Close 可安全处理部分初始化状态；多数 Source 的 OpenInput 使用 `do-while(false) + if(!ok) Close()` 模式，失败路径零手动清理代码；`MixedAudioSource::OpenOneInput` 则在每个失败分支自行清理上游资源（模范写法） |
| **中断回调** | FFmpeg `interrupt_callback` 让阻塞采集能被停止请求打断（5 个 Source 统一 `stop_flag_` 模式） |

---

## 11. 已知限制与设计决策

| 项目 | 决策 / 限制 |
|------|------------|
| 编码器 | GUI STA 下 h264_mf 不可用，统一 mpeg4 纯软件 |
| 分段架构 | 方案 C 三线程：encoder 全程一个不重建（消除硬编边界重建开销的路径已铺好），segment 边界只 RotateMuxer + pts_offset 归零；producer 侧 flush 期间短暂停采（亚秒级间隙） |
| 分段关键帧 | 新 segment 开头 IDR 依赖 encoder GOP（当前 2s）；segment 时长 < GOP 时开头可能参考前段数据（短暂花屏风险）。未实现 segment 边界主动触发 IDR（后续可在 flush 前置 `AV_CODEC_FLAG_FORCE_KF` 或重置 encoder 内部 picture 计数） |
| 分段时长精度 | 边界由 muxer_worker 1ms 轮询墙钟检测，实际切点误差 ≤ flush+rotate 耗时（百毫秒级），每段从 Rotate 完成时刻重新起算 |
| 水印实现 | 不走 libavfilter drawtext（需 FreeType 支持），统一由 `Watermark.h` 用 QPainter 画在采集帧上 |
| 系统声音采集 | 依赖立体声混音 / 虚拟声卡。本机已启用“立体声混音 (Realtek(R) Audio)”，选中该设备即可录制系统播放声音；换机器若缺少此设备，需在 Windows 声音设置启用或安装虚拟声卡 |
| 光标闪烁 | gdigrab 硬编码 BITBlt SRCCOPY\|CAPTUREBLT，无法参数化关闭 |
| 暂停期间 buffer | 音频 / dshow 视频暂停时持续读包丢弃（防 rtbufsize 溢出） |
| 高分屏坐标换算 | 区域截图 / 区域录制用 overlay 尺寸比例换算 QML 逻辑坐标 → 物理像素 |
| FFmpeg 版本 | 外部 32-bit DLL，位于项目内 `depends/ffmpeg/bin`，构建后自动拷到 exe 同目录，不与源码一起构建；MinGW 32 下用 dlltool 从 .def 生成 `.dll.a` 导入库链接（`libavutil-58.dll.a` 等，已生成在 bin/） |
| 播放器渲染 | `QQuickItem` + `updatePaintNode` + `QSGImageNode`；Qt 5.12 无纹理原地更新接口，每帧新建纹理（30fps 下开销可接受），缩放交给 GPU |
| 播放器解码 | 不用 QtMultimedia `MediaPlayer`：Qt 5.12 DirectShow 后端依赖系统 MP4 解码器，本机缺失（0x80040266），且无法实现 atempo 倍速与音频时钟同步 |
