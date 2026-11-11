// FFmpeg 头文件必须在任何项目头文件之前 include，确保 libavutil/fifo.h 的
// 第一次 include 就被 extern "C" 包裹（该头文件自身没有 extern "C"）。
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/fifo.h>
}

#include "ScreenRecorder.h"

#include <algorithm>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include "capture/audio_source_factory.h"
#include "capture/video_source_factory.h"

using namespace std::chrono;

namespace re {
namespace {

// 从 AVFifo 里把所有可读的 AVPacket pop 出来，每个都通过 writer.WritePacket
// 写入对应 stream + pts_offset。
// 返回本次成功写入的 packet 中最大的"原始 pts"（offset 减法之前、producer 已
// rescale 到 stream time_base 的值），没写任何包时返回 -1。muxer 用它作为新
// segment 的 pts_offset 权威来源——与 encoder flush 是否产出残包无关，且视频/
// 音频两轨各自独立跟踪、口径一致。
int64_t DrainFifoAndWrite(AVFifo* queue, AVStream* stream, int64_t pts_offset,
                          Mp4Writer& writer) {
    int64_t last_raw_pts = -1;
    if (!queue || !stream) return -1;

    while (av_fifo_can_read(queue) > 0) {
        AVPacket pkt; av_init_packet(&pkt);
        if (av_fifo_read(queue, &pkt, 1) < 1) break;
        // producer 侧已经 rescale 到 stream time_base，但 stream 指针在 Rotate
        // 后可能变了；不过 rescale 用的是 producer 里持有的 stream_->time_base，
        // 而 Rotate 后新 stream 的 time_base 跟 encoder 的 time_base 应该是一致的
        // （OpenMuxer 里 stream->time_base = enc->time_base），所以 rescale 是安全的。
        // 但 WritePacket 里会重置 stream_index = stream->index，所以不会错乱。
        // WritePacket 会原地做 pkt.pts -= offset，因此原始 pts 必须在调用前取。
        const int64_t raw_pts = pkt.pts;
        if (writer.WritePacket(stream, pts_offset, &pkt) >= 0) {
            if (raw_pts > last_raw_pts) last_raw_pts = raw_pts;
        }
        av_packet_unref(&pkt);
    }
    return last_raw_pts;
}

}  // namespace

// 把原始 output_path 转成分段录制下第 segment_index 段的文件名：
//   output.mp4       → output_001.mp4
//   C:/videos/r.mp4  → C:/videos/r_001.mp4
std::string ScreenRecorder::BuildSegmentPath(const std::string& original,
                                              int segment_index) {
    const int n = std::max(1, segment_index);
    const std::string suffix = "_" + std::to_string(n);

    auto slash = original.find_last_of("/\\");
    auto dot   = original.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        return original.substr(0, dot) + suffix + original.substr(dot);
    }
    return original + suffix;
}

ScreenRecorder::ScreenRecorder(RecorderOptions options)
    : opts_(std::move(options)) {}

ScreenRecorder::~ScreenRecorder() {
    // 确保 worker 线程都已结束，再释放资源。幂等：Stop() 会 join 各 worker。
    Stop();
    Close();
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

bool ScreenRecorder::Open() {
    // 1. 构造 VideoOptions / AudioOptions，分别 Create + Open。
    VideoOptions vopts;
    vopts.fps = opts_.fps;
    vopts.bit_rate = opts_.bit_rate;
    vopts.offset_x = opts_.offset_x;
    vopts.offset_y = opts_.offset_y;
    vopts.width = opts_.width;
    vopts.height = opts_.height;
    vopts.speed_multiplier = opts_.speed_multiplier;
    vopts.window_title = opts_.window_title;
    vopts.device = opts_.video_device;
    vopts.secondary_device = opts_.video_secondary_device;

    video_ = VideoSourceFactory::Create(opts_.video_backend, vopts);
    if (!video_ || !video_->Open()) return false;

    if (opts_.capture_audio) {
        AudioOptions aopts;
        aopts.device = opts_.audio_device;
        aopts.secondary_device = opts_.audio_secondary_device;
        aopts.sample_rate = opts_.audio_sample_rate;
        aopts.channels = opts_.audio_channels;
        aopts.bit_rate = opts_.audio_bit_rate;
        aopts.speed_multiplier = opts_.speed_multiplier;

        audio_ = AudioSourceFactory::Create(opts_.audio_backend, aopts);
        if (!audio_ || !audio_->Open()) {
            // 音频初始化失败时降级为纯视频录制，不中断。
            audio_.reset();
            opts_.capture_audio = false;
        }
    }

    // 2. OpenMuxer — 为已打开的各轨 encoder 创建 stream + 写首个文件头。
    std::vector<AVCodecContext*> encs;
    encs.push_back(video_->EncCtx());
    if (audio_) encs.push_back(audio_->EncCtx());

    std::vector<AVStream*> out_streams;
    if (!writer_.OpenMuxer(opts_.output_path.c_str(), encs, out_streams)) {
        return false;
    }

    current_stream_v_ = out_streams[0];
    current_stream_a_ = audio_ ? out_streams[1] : nullptr;

    // 3. 创建 AVFifo（每轨 depth=16 AVPacket，足够吸收 producer/muxer 的短暂速度差）。
    queue_v_ = av_fifo_alloc2(16, sizeof(AVPacket), 0);
    if (!queue_v_) return false;
    if (opts_.capture_audio) {
        queue_a_ = av_fifo_alloc2(16, sizeof(AVPacket), 0);
        if (!queue_a_) return false;
    }

    // 4. 注入 producer 侧的外部依赖（queue / stream / flush_done 标志）。
    video_->SetQueue(queue_v_);
    video_->SetStream(current_stream_v_);
    video_->SetFlushDoneFlag(&flush_done_v_);

    if (audio_) {
        audio_->SetQueue(queue_a_);
        audio_->SetStream(current_stream_a_);
        audio_->SetFlushDoneFlag(&flush_done_a_);
    }

    return true;
}

bool ScreenRecorder::Start(double duration_seconds) {
    if (recording_.load()) return false;
    if (video_worker_.joinable() || audio_worker_.joinable() || muxer_worker_.joinable()) {
        return false;
    }

    if (!Open()) {
        Close();
        return false;
    }

    // 重置所有原子状态。
    stop_requested_.store(false);
    video_ok_.store(true);
    audio_ok_.store(true);
    recording_.store(true);
    segment_index_ = 0;
    pts_offset_v_ = 0;
    pts_offset_a_ = 0;
    flush_requested_v_.store(false);
    flush_requested_a_.store(false);
    flush_done_v_.store(false);
    flush_done_a_.store(false);
    producers_done_v_.store(false);
    producers_done_a_.store(false);
    if (!audio_) producers_done_a_.store(true);  // 无音频轨时视为"已完成"

    // 共享墙钟零点 —— 所有轨的 wall clock PTS 都从此刻起算。
    capture_start_ = std::chrono::steady_clock::now();

    const double seg_dur = opts_.segment_duration_seconds;
    const bool segmented = seg_dur > 0.0;

    // producer 的 Run 签名：duration_seconds 传 0 表示无限循环（segment 边界由 muxer 驱动，
    // producer 不主动因为墙钟到点退出）；只有 stop_requested 才退出。
    // Run 返回前内部已 FlushEncoderToQueue() 把尾包 push queue；producers_done_
    // 置 true 告诉 muxer "我的包全到齐了，可以安全做最终 drain + trailer"。
    video_worker_ = std::thread([this]() {
        bool ok = video_->Run(/*duration_seconds=*/0,
                              capture_start_,
                              stop_requested_,
                              flush_requested_v_);
        video_ok_.store(ok);
        producers_done_v_.store(true);
    });

    if (audio_) {
        audio_worker_ = std::thread([this]() {
            bool ok = audio_->Run(/*duration_seconds=*/0,
                                  capture_start_,
                                  stop_requested_,
                                  flush_requested_a_);
            audio_ok_.store(ok);
            producers_done_a_.store(true);
        });
    }

    // muxer_worker：唯一触碰 Mp4Writer 的线程。消费两个 queue，做 segment 边界检测 + Rotate。
    muxer_worker_ = std::thread([this, segmented, seg_dur]() {
        // 段内"活跃"时长（暂停期间冻结）：与 UI 有效时长同口径。暂停时不推进，
        // 分段边界顺延到 Resume 之后，避免"暂停超时误触发边界 → producer 不响应
        // flush_requested → 5s 超时强制停止整场录制"的问题。
        double seg_active = 0.0;
        auto last_tick = steady_clock::now();
        // 各轨已成功写入的最大原始 pts（DrainFifoAndWrite 返回值的跨轮累计），
        // 是新 segment pts_offset 的权威来源。
        int64_t last_raw_v = -1;
        int64_t last_raw_a = -1;

        // 主循环退出条件：stop_requested 且两个 producer 都已退出（尾包全部到齐）。
        // 这样 Stop() 时 muxer 不会抢先收尾，producer flush 出来的尾包不会丢。
        while (!stop_requested_.load() ||
               !producers_done_v_.load() ||
               !producers_done_a_.load()) {
            // 1. Drain 视频 queue
            last_raw_v = std::max(last_raw_v,
                DrainFifoAndWrite(queue_v_, current_stream_v_, pts_offset_v_, writer_));
            // 2. Drain 音频 queue（可选）
            if (audio_) {
                last_raw_a = std::max(last_raw_a,
                    DrainFifoAndWrite(queue_a_, current_stream_a_, pts_offset_a_, writer_));
            }

            // 2.5 推进段计时（暂停冻结）。
            const auto tick = steady_clock::now();
            if (!paused_.load()) {
                seg_active += duration<double>(tick - last_tick).count();
            }
            last_tick = tick;

            // 3. Segment 边界检测：段内活跃时长达到 seg_dur。producer 已退出时不再
            // 触发边界（异常退出后继续轮转空段毫无意义）；无音频轨时忽略音频 done 标志。
            if (segmented && !stop_requested_.load() &&
                !producers_done_v_.load() && (!audio_ || !producers_done_a_.load()) &&
                seg_active >= seg_dur) {
                // ==== 触发 flush 协调 ====
                flush_done_v_.store(false);
                flush_requested_v_.store(true);
                if (audio_) {
                    flush_done_a_.store(false);
                    flush_requested_a_.store(true);
                }

                // 等 producer flush 完（encoder 内部残帧 flush → push queue → flush_done=true）。
                // timeout 防止 producer 异常卡死；producer 已退出则不再等。
                const auto t0 = steady_clock::now();
                while (!flush_done_v_.load() && !stop_requested_.load() &&
                       !producers_done_v_.load()) {
                    if (duration<double>(steady_clock::now() - t0).count() > 5.0) {
                        stop_requested_.store(true);
                        break;
                    }
                    std::this_thread::sleep_for(milliseconds(2));
                }
                if (audio_) {
                    const auto t0a = steady_clock::now();
                    while (!flush_done_a_.load() && !stop_requested_.load() &&
                           !producers_done_a_.load()) {
                        if (duration<double>(steady_clock::now() - t0a).count() > 5.0) {
                            stop_requested_.store(true);
                            break;
                        }
                        std::this_thread::sleep_for(milliseconds(2));
                    }
                }

                if (stop_requested_.load()) break;

                // 4. Drain flush 出来的最后几个包（属于旧 segment，用旧 offset 写）。
                last_raw_v = std::max(last_raw_v,
                    DrainFifoAndWrite(queue_v_, current_stream_v_, pts_offset_v_, writer_));
                if (audio_) {
                    last_raw_a = std::max(last_raw_a,
                        DrainFifoAndWrite(queue_a_, current_stream_a_, pts_offset_a_, writer_));
                }

                // 5. 新 segment 的 pts_offset = 本轨已写入的最大原始 pts + 1。
                //    muxer 侧权威跟踪：与 encoder flush 是否产出残包无关，且天然
                //    保证两轨在同一时间点取口径（不会一轨归零、另一轨不归零导致
                //    段内音视频失步）。
                //    本轨整段零包时（如音频设备短暂 stall 但 producer 仍存活）
                //    继承上一段 offset 而非归零：恢复后首包 pts = 原始 pts - 旧
                //    offset 仍接近 0，避免与另一轨失步；首段零包时旧 offset 本
                //    就是 0，行为与归零相同。
                const int64_t new_off_v =
                    (last_raw_v >= 0) ? last_raw_v + 1 : pts_offset_v_;
                const int64_t new_off_a =
                    (audio_ && last_raw_a >= 0) ? last_raw_a + 1 : pts_offset_a_;

                // 6. RotateMuxer：旧文件 trailer → 新文件 Alloc + NewStream + WriteHeader。
                std::string next_path = BuildSegmentPath(opts_.output_path,
                                                         ++segment_index_);
                std::vector<AVCodecContext*> encs;
                encs.push_back(video_->EncCtx());
                if (audio_) encs.push_back(audio_->EncCtx());

                std::vector<AVStream*> new_streams;
                if (!writer_.RotateMuxer(next_path.c_str(), encs, new_streams)) {
                    stop_requested_.store(true);
                    break;
                }

                // 7. 刷新 stream 指针（muxer 自己持有的 + producer 持有的），
                // 以及 pts_offset。
                current_stream_v_ = new_streams[0];
                pts_offset_v_ = new_off_v;

                video_->SetStream(current_stream_v_);

                if (audio_) {
                    current_stream_a_ = new_streams[1];
                    pts_offset_a_ = new_off_a;
                    audio_->SetStream(current_stream_a_);
                }

                // 8. Reset 段计时：新 segment 从 0 起算，Rotate 耗时不计入下一段。
                //    capture_start_ 只作为 producer 的墙钟 PTS 零点，全录制期间不变。
                seg_active = 0.0;
                last_tick = steady_clock::now();

                // 9. Reset flush 状态，producer 可以继续采集。
                flush_requested_v_.store(false);
                flush_requested_a_.store(false);

                continue;  // 回到主循环顶部，从 drain queue 开始
            }

            // 短暂 sleep 避免空转耗 CPU。AVFifo 无 event 通知机制，用 polling。
            std::this_thread::sleep_for(milliseconds(1));
        }

        // ==== Stop 收尾：producer 都已退出（producers_done_ 保证尾包全部到齐），
        // 最后 drain 一次 queue，然后由 Stop() 里的 Close() → writer_.Close() 写 trailer。
        DrainFifoAndWrite(queue_v_, current_stream_v_, pts_offset_v_, writer_);
        if (audio_) {
            DrainFifoAndWrite(queue_a_, current_stream_a_, pts_offset_a_, writer_);
        }
    });

    return true;
}

bool ScreenRecorder::Stop() {
    // 1. 通知所有线程停止。
    stop_requested_.store(true);

    // 2. producer 线程：内部 Run 会看到 stop_requested → 退出 while → FlushEncoderToQueue → return。
    if (video_worker_.joinable()) video_worker_.join();
    if (audio_worker_.joinable()) audio_worker_.join();

    // 3. muxer_worker：主循环看到 stop_requested → drain queue → writer_.Close() → return。
    if (muxer_worker_.joinable()) muxer_worker_.join();

    // 4. 释放资源。
    Close();

    recording_.store(false);
    return video_ok_.load() && (!opts_.capture_audio || audio_ok_.load());
}

// ---------------------------------------------------------------------------
// 暂停 / 继续
// ---------------------------------------------------------------------------

bool ScreenRecorder::Pause() {
    if (!recording_.load() || paused_.load()) return false;
    paused_.store(true);
    if (video_) video_->Pause();
    if (audio_) audio_->Pause();
    return true;
}

bool ScreenRecorder::Resume() {
    if (!paused_.load()) return false;
    if (video_) video_->Resume();
    if (audio_) audio_->Resume();
    paused_.store(false);
    return true;
}

// ---------------------------------------------------------------------------
// 拆除
// ---------------------------------------------------------------------------

void ScreenRecorder::Close() {
    // 顺序很重要：先 close Source（free encoder/decoder 等），再 close writer（trailer + free muxer），
    // 最后 free AVFifo。AVFifo 必须所有 producer/consumer 都退出后才 free。
    if (video_) video_->Close();
    if (audio_) audio_->Close();

    writer_.Close();

    if (queue_v_) { av_fifo_freep2(&queue_v_); }
    if (queue_a_) { av_fifo_freep2(&queue_a_); }

    current_stream_v_ = nullptr;
    current_stream_a_ = nullptr;
}

}  // namespace re
