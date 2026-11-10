# 倍率录制（Time-Lapse）

> 历史方案文档（非当前实现说明）。最终实现以 `docs/项目架构与调用流程.md` 和 `ARCHITECTURE.md` 为准；本文中的 `frame_index_`、PTS 算法等细节已与代码不一致。

## Context

现有录屏只支持 1:1 实时录制。用户需要"倍率录制"——把 N 秒墙钟时间压缩到 1 秒 mp4 输出，用于延时/快进回放。支持 1/2/4/8 倍四档（8 倍即录 8 秒墙钟 → 输出 1 秒 mp4），视频音频都要保留，UI 加下拉框选择。维持手动停止（用户点"结束"才停）。

方向：N 倍 = 快进（time-lapse），不是慢放。
音频策略：下采样保留（每 N 个样本取 1 个），接受变调。
停止时机：维持手动停止（不自动停）。

## 实现方案

核心思路：在两条采集源的 `Run()` 循环里按 `speed_multiplier` 抽帧/抽样本，并改写输出 PTS。所有倍率逻辑封装在具体源内部，对协调器（ScreenRecorder）只多传一个旋钮。

### 1. RecorderOptions / VideoOptions / AudioOptions 加字段

[ScreenRecorder.h](file:///d:/Work/RecordExpert/src/model/ScreenRecorder.h#L18-L48) `RecorderOptions` 加：
```cpp
int speed_multiplier = 1;  // 1/2/4/8；N>1 时 N 秒墙钟 → 1 秒 mp4（time-lapse）。
```

[i_video_source.h](file:///d:/Work/RecordExpert/src/model/capture/i_video_source.h) `VideoOptions`、[i_audio_source.h](file:///d:/Work/RecordExpert/src/model/capture/i_audio_source.h) `AudioOptions` 各加同名字段。

[ScreenRecorder.cpp](file:///d:/Work/RecordExpert/src/model/ScreenRecorder.cpp) `Open()` 把 `opts_.speed_multiplier` 同时填到 `VideoOptions` 和 `AudioOptions`。

### 2. GdiGrabVideoSource::Run() 视频抽帧

[gdi_grab_video_source.cpp](file:///d:/Work/RecordExpert/src/model/capture/gdi_grab_video_source.cpp) `Run()` 循环：

- 加帧序号计数 `int64_t frame_index_ = 0`（成员，重置在 Run 开头）
- `avcodec_receive_frame` 拿到帧后：
  - 若 `frame_index_ % speed_multiplier != 0`：丢弃此帧（`av_frame_unref` + continue）
  - 否则保留此帧，输出 PTS = `frame_index_ / speed_multiplier`（替代当前的"真实时间×fps"算法）
  - `frame_index_++`
- 暂停期间仍按原逻辑跳过采集（不改）
- 由于输出 PTS 不再依赖墙钟，原 `pts_` 修正逻辑可保留（仍保持严格单调）

> 注：抽帧后输出的"逻辑帧率"还是 fps，因为 PTS 步长 1，time_base=1/fps。墙钟 N 秒采集 N*fps 帧，保留 N*fps/N = fps 帧 = 1 秒视频。

### 3. DShowAudioSource::Run() 音频下采样

[dshow_audio_source.cpp](file:///d:/Work/RecordExpert/src/model/capture/dshow_audio_source.cpp) `Run()` 循环：

- 解码 + swr 重采样后得到 `conv_frame`（FLTP）
- 下采样：对每通道，只取 `conv_frame->data[ch][i]` 中 `i % speed_multiplier == 0` 的样本，写入一个临时小 buffer（`std::vector<float>` 每通道），再送 `av_audio_fifo_write`
- `samples_emitted_` 只累加保留下来的样本数（即原 samples / N）
- PTS 公式不变（`pts_base_ + samples_emitted_`），但 `pts_base_` 首帧计算时也要按"已经过墙钟时间 / N"换算（因为墙钟 1 秒现在对应 1/N 秒输出 = sample_rate/N 个样本）
- FIFO flush 路径不变

### 4. Mp4Writer 无需改动

`av_packet_rescale_ts` 从 `enc_ctx_->time_base` 转流时基，源给的 PTS 已经是正确的输出时间轴，muxer 不感知倍率。

### 5. RecorderViewModel 暴露倍率给 QML

[RecorderViewModel.h](file:///d:/Work/RecordExpert/src/viewmodel/RecorderViewModel.h)：
```cpp
Q_PROPERTY(int speedMultiplier READ speedMultiplier WRITE setSpeedMultiplier NOTIFY speedMultiplierChanged)
int speedMultiplier() const { return speedMultiplier_; }
void setSpeedMultiplier(int v);
signals: void speedMultiplierChanged();
private: int speedMultiplier_ = 1;
```

`start()` 把 `speedMultiplier_` 写入 `opts_.speed_multiplier`。校验值域 1/2/4/8，越界回退 1。

### 6. RecordPage.qml 加下拉框

[RecordPage.qml](file:///d:/Work/RecordExpert/qml/RecordPage.qml) 在输出路径行下面加：
```qml
RowLayout {
    Layout.fillWidth: true
    Label { text: "倍率:" }
    ComboBox {
        model: ["1倍", "2倍", "4倍", "8倍"]
        currentIndex: 0
        onActivated: controller.speedMultiplier = [1,2,4,8][index]
    }
}
```

### 7. 验证

1. 编译通过（`Get-Process record_expert | Stop-Process -Force` + MinGW 重编）
2. 启动 GUI，倍率选 8 倍，点开始
3. 录约 16 秒墙钟时间（计时器或目测）
4. 点结束
5. 检查 output.mp4：时长应约 2 秒（16/8），视频帧数约 60 帧（30fps×2s），音频约 2 秒且变调
6. 切回 1 倍重复，时长应等于墙钟时长（回归测试）
7. 切 2 倍/4 倍各录一段验证中间档位

## 不改动

- IVideoSource/IAudioSource 抽象接口签名（不动 Open/Run/Close/Pause/Resume）
- Mp4Writer、watermark、screenshot 链路
- 工厂创建逻辑
- 暂停/继续语义（倍率与暂停正交，暂停期间两条源仍按各自策略跳过）
