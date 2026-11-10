#include "PlaybackDecoder.h"
#include "AudioPlayer.h"

#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <string>

namespace re {

PlaybackDecoder::PlaybackDecoder(QObject* parent) : QThread(parent) {}

PlaybackDecoder::~PlaybackDecoder() {
    stop();
    wait();  // 等线程退出
    close();
}

qint64 PlaybackDecoder::audioClockMs() const {
    double ms = audioClockMsDouble();
    return (ms < 0.0) ? -1 : static_cast<qint64>(ms);
}

double PlaybackDecoder::audioClockMsDouble() const {
    if (!hasAudio() || audio_sample_rate_ <= 0) return -1.0;
    if (!audio_clock_started_) return -1.0;
    // 输入样本时钟(精确,无 atempo 比例漂移):
    //   decoded = audio_start_pts + input_samples_total * 1000 / rate  (精确内容时间)
    //   played = decoded - ring_pending_content
    //          = decoded - (old_ring * prev_speed + new_ring * speed) * 1000 / rate
    //
    // input_samples_total 是 atempo 前的解码样本,精确等于内容时间,不受 atempo
    // 输出比例偏差影响。ring_pending 换算有 atempo 比例误差,但环形缓冲容量固定
    // (~170ms),误差有界不随播放时长累积。
    // setAudioSpeed 将 audio_start_pts rebase 到 decoded 位置,beginSpeedChange
    // 重置 input_samples_total=0,新轮次从 decoded 起算。
    qint64 old_ring = 0, new_ring = 0;
    double input_samples = 0;
    if (audio_player_) {
        audio_player_->ringPendingSamples(old_ring, new_ring);
        input_samples = static_cast<double>(audio_player_->inputSamplesTotal());
    }
    return audio_start_pts_ms_.load() +
           input_samples * 1000.0 / audio_sample_rate_ -
           static_cast<double>(old_ring) * prev_audio_speed_ * 1000.0 / audio_sample_rate_ -
           static_cast<double>(new_ring) * audio_speed_ * 1000.0 / audio_sample_rate_;
}

void PlaybackDecoder::setAudioSpeed(int speed) {
    if (speed == pending_audio_speed_.load()) return;
    pending_audio_speed_.store(speed);
    if (audio_player_ && audio_clock_started_ && audio_sample_rate_ > 0) {
        // Rebase 到 decoded 位置(精确,无 atempo 比例误差):
        //   decoded = audio_start_pts + input_samples_total * 1000 / rate
        // takeInputSamples 原子完成"读取+清零":新轮次从 decoded 起算,
        // 读与清零在同一把锁内,两者之间解码线程新累加的样本不会丢失。
        // (旧实现先读后由 beginSpeedChange 清零,两次加锁之间落入的样本
        //  两边都不计,每次切换时钟回退 ~21ms,频繁切换会累积。)
        qint64 input = audio_player_->takeInputSamples();
        audio_start_pts_ms_.store(audio_start_pts_ms_.load() +
            static_cast<double>(input) * 1000.0 / audio_sample_rate_);
        // 保存旧速率:ringPendingSamples 中旧数据用此速率换算内容时间
        prev_audio_speed_ = audio_speed_;
        // beginSpeedChange:不清缓冲,记录旧数据边界
        audio_player_->beginSpeedChange();
    }
    cond_.wakeAll();
    // audio_speed_ 更新 + atempo 重建 + endSpeedChange
    // 在 run() 顶部 pending 检查点(解码线程)执行
}

void PlaybackDecoder::pauseAudio() {
    // 先设 paused_=1:解码线程在 run() 顶部和 handleVideoFrame 队列满等待
    // 处都会检测到此标志并阻塞,避免暂停期间音频数据被提前解码丢弃。
    paused_ = 1;
    // 用 pause() 而非 stop():不清缓冲,保留旧数据供 resume 后无缝续播。
    // write() 检测到 paused_=1 后返回 false,handleAudioFrame 返回 false,
    // 解码循环 break → 退出到 run() 外层 while 的 paused_ 等待点。
    if (audio_player_) audio_player_->pause();
    // 唤醒所有等待者:队列满等待(handleVideoFrame)、暂停等待(run 外层)
    cond_.wakeAll();
}

void PlaybackDecoder::resumeAudio() {
    // 清暂停标志:解码线程检测后继续 run() 循环
    paused_ = 0;
    // 恢复音频输出:不清缓冲,QAudioOutput 立即消费暂停前缓冲中的旧数据
    // (~170ms),音频时钟由输入样本计数与待消费样本数继续推算,无缝衔接。
    if (audio_player_) audio_player_->resume();
    // 唤醒暂停等待的解码线程
    cond_.wakeAll();
}

bool PlaybackDecoder::initAudioFilterGraph(int speed) {
    freeAudioFilterGraph();

    afilter_graph_ = avfilter_graph_alloc();
    if (!afilter_graph_) return false;

    // abuffer 输入:按音频解码器参数构造
    const AVFilter* abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) { freeAudioFilterGraph(); return false; }

    char args[256];
    char layout_str[64] = {0};
    AVChannelLayout in_layout = audio_codec_ctx_->ch_layout;
    av_channel_layout_describe(&in_layout, layout_str, sizeof(layout_str));
    std::snprintf(args, sizeof(args),
                  "sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                  audio_codec_ctx_->sample_rate,
                  av_get_sample_fmt_name(audio_codec_ctx_->sample_fmt),
                  layout_str);

    int ret = avfilter_graph_create_filter(&abuffersrc_ctx_, abuffer, "in",
                                           args, nullptr, afilter_graph_);
    if (ret < 0) { freeAudioFilterGraph(); return false; }

    // abuffersink 输出
    const AVFilter* abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) { freeAudioFilterGraph(); return false; }

    ret = avfilter_graph_create_filter(&abuffersink_ctx_, abuffersink, "out",
                                       nullptr, nullptr, afilter_graph_);
    if (ret < 0) { freeAudioFilterGraph(); return false; }

    // 约束 buffersink 输出采样格式 = 解码器原格式(如 fltp 平面)。
    // 不约束时格式协商可能选中 packed 格式(实测选了 flt),而 writeFrame
    // 的 swr 输入按解码器平面格式取 in_data[] 平面指针,packed 帧的
    // data[1..] 为 NULL → swresample 读空指针崩溃(0xC0000005)。
    const enum AVSampleFormat sink_fmts[] = {
        audio_codec_ctx_->sample_fmt, AV_SAMPLE_FMT_NONE };
    ret = av_opt_set_bin(abuffersink_ctx_, "sample_fmts",
                         reinterpret_cast<const uint8_t*>(sink_fmts),
                         static_cast<int>(sizeof(sink_fmts) -
                                          sizeof(sink_fmts[0])),
                         AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) { freeAudioFilterGraph(); return false; }

    // atempo 串联:单次范围 0.5~2.0,>2 需多级串联
    // speed=2 → "atempo=2.0"
    // speed=4 → "atempo=2.0,atempo=2.0"
    // speed=8 → "atempo=2.0,atempo=2.0,atempo=2.0"
    std::string filter_desc;
    int remain = speed;
    while (remain > 1) {
        int chunk = (remain >= 2) ? 2 : remain;
        if (!filter_desc.empty()) filter_desc += ",";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "atempo=%.1f", static_cast<double>(chunk));
        filter_desc += buf;
        remain /= chunk;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs  = avfilter_inout_alloc();
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = abuffersrc_ctx_;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = abuffersink_ctx_;
    inputs->pad_idx     = 0;
    inputs->next        = nullptr;

    ret = avfilter_graph_parse(afilter_graph_, filter_desc.c_str(),
                               inputs, outputs, nullptr);
    // avfilter_graph_parse 成功后 inout 链已被 graph 内部吸收,
    // 结构本身(含 name 的 av_strdup 拷贝)需要手动释放。
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0) {
        freeAudioFilterGraph();
        return false;
    }

    ret = avfilter_graph_config(afilter_graph_, nullptr);
    if (ret < 0) { freeAudioFilterGraph(); return false; }

    return true;
}

void PlaybackDecoder::freeAudioFilterGraph() {
    if (afilter_graph_) {
        avfilter_graph_free(&afilter_graph_);
        abuffersrc_ctx_  = nullptr;
        abuffersink_ctx_ = nullptr;
    }
}

bool PlaybackDecoder::open(const QString& path) {
    close();

    // 解析相对路径:相对于 exe 所在目录。
    QString abs_path = path;
    if (!QFileInfo(path).isAbsolute())
        abs_path = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(path);
    if (!QFileInfo::exists(abs_path)) return false;

    // 打开输入文件
    int ret = avformat_open_input(&fmt_ctx_, abs_path.toUtf8().constData(),
                                  nullptr, nullptr);
    if (ret < 0) { fmt_ctx_ = nullptr; return false; }

    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) { close(); return false; }

    // 查找视频流
    video_stream_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO,
                                        -1, -1, nullptr, 0);
    if (video_stream_ < 0) { close(); return false; }

    // 打开解码器
    const AVCodec* codec = avcodec_find_decoder(
        fmt_ctx_->streams[video_stream_]->codecpar->codec_id);
    if (!codec) { close(); return false; }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) { close(); return false; }

    avcodec_parameters_to_context(codec_ctx_,
                                  fmt_ctx_->streams[video_stream_]->codecpar);
    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) { avcodec_free_context(&codec_ctx_); close(); return false; }

    width_  = codec_ctx_->width;
    height_ = codec_ctx_->height;

    // 计算帧率和时长
    auto* stream = fmt_ctx_->streams[video_stream_];
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
        fps_ = av_q2d(stream->avg_frame_rate);
    else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0)
        fps_ = av_q2d(stream->r_frame_rate);

    if (fmt_ctx_->duration != AV_NOPTS_VALUE)
        duration_ms_ = fmt_ctx_->duration / 1000;  // AV_TIME_BASE -> ms

    // 创建 sws 上下文:任意像素格式 → RGB24
    sws_ctx_ = sws_getContext(width_, height_, codec_ctx_->pix_fmt,
                              width_, height_, AV_PIX_FMT_RGB24,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) { close(); return false; }

    // 尝试打开音频流(可选:失败不致命,降级为静音播放)
    audio_stream_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_AUDIO,
                                        -1, -1, nullptr, 0);
    if (audio_stream_ >= 0) {
        bool audio_ok = false;
        do {
            const AVCodec* acodec = avcodec_find_decoder(
                fmt_ctx_->streams[audio_stream_]->codecpar->codec_id);
            if (!acodec) break;

            audio_codec_ctx_ = avcodec_alloc_context3(acodec);
            if (!audio_codec_ctx_) break;
            avcodec_parameters_to_context(
                audio_codec_ctx_,
                fmt_ctx_->streams[audio_stream_]->codecpar);
            if (avcodec_open2(audio_codec_ctx_, acodec, nullptr) < 0) break;

            // 重采样到 S16LE interleaved 16bit,采样率/通道沿用源
            audio_sample_rate_ = audio_codec_ctx_->sample_rate;
            audio_channels_   = audio_codec_ctx_->ch_layout.nb_channels;
            if (audio_channels_ <= 0) audio_channels_ = 2;

            // 构造输出通道布局(与输入同通道数,默认布局)
            AVChannelLayout out_layout;
            av_channel_layout_default(&out_layout, audio_channels_);

            swr_ctx_ = swr_alloc();
            bool swr_ok = (swr_ctx_ != nullptr);
            if (swr_ok) {
                av_opt_set_chlayout(swr_ctx_, "in_chlayout",
                                    &audio_codec_ctx_->ch_layout, 0);
                av_opt_set_int(swr_ctx_, "in_sample_rate",
                               audio_codec_ctx_->sample_rate, 0);
                av_opt_set_sample_fmt(swr_ctx_, "in_sample_fmt",
                                      audio_codec_ctx_->sample_fmt, 0);
                av_opt_set_chlayout(swr_ctx_, "out_chlayout",
                                    &out_layout, 0);
                av_opt_set_int(swr_ctx_, "out_sample_rate",
                               audio_sample_rate_, 0);
                av_opt_set_sample_fmt(swr_ctx_, "out_sample_fmt",
                                      AV_SAMPLE_FMT_S16, 0);
                if (swr_init(swr_ctx_) < 0) swr_ok = false;
            }
            av_channel_layout_uninit(&out_layout);
            if (!swr_ok) break;

            audio_player_ = new AudioPlayer();
            if (!audio_player_->open(audio_sample_rate_, audio_channels_)) {
                break;
            }

            audio_ok = true;
        } while (false);

        // 音频段失败:只清理音频相关资源,不释放已成功打开的视频上下文。
        // close() 也做同样清理,幂等安全。
        if (!audio_ok) {
            if (audio_player_) { delete audio_player_; audio_player_ = nullptr; }
            if (swr_ctx_) { swr_free(&swr_ctx_); swr_ctx_ = nullptr; }
            if (audio_codec_ctx_) { avcodec_free_context(&audio_codec_ctx_); }
            audio_stream_ = -1;
            audio_sample_rate_ = 0;
            audio_channels_ = 0;
        }
    }

    audio_start_pts_ms_ = -1.0;
    audio_clock_started_ = false;
    audio_speed_ = 1;  // 重新打开时复位倍速:上层 play() 会按 UI 倍速重新设置
    pending_audio_speed_.store(1);  // BUG3:pending 标志也需复位
    stop_ = 0;
    eof_ = 0;
    return true;
}

void PlaybackDecoder::close() {
    // 先停音频输出(避免解码线程结束后还有 PCM 在飞)
    if (audio_player_) { delete audio_player_; audio_player_ = nullptr; }
    freeAudioFilterGraph();
    if (swr_ctx_) { swr_free(&swr_ctx_); swr_ctx_ = nullptr; }
    if (audio_codec_ctx_) { avcodec_free_context(&audio_codec_ctx_); }
    audio_stream_ = -1;
    audio_sample_rate_ = 0;
    audio_channels_   = 0;
    audio_start_pts_ms_ = -1.0;
    audio_clock_started_ = false;

    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (codec_ctx_) { avcodec_free_context(&codec_ctx_); }
    if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); }
    video_stream_ = -1;
    width_ = height_ = 0;
    fps_ = 30.0;
    duration_ms_ = 0;
    {
        QMutexLocker lk(&mutex_);
        queue_.clear();
    }
}

void PlaybackDecoder::stop() {
    stop_ = 1;
    // 清 paused_:先 pause 再 stop 时,解码线程可能卡在 run() 外层的
    // paused_ 等待里;清掉后 wakeAll 能让它检测到 stop_ 并退出。
    paused_ = 0;
    // 联动停音频输出:解码线程可能阻塞在 AudioPlayer::write 的
    // 空闲缓冲等待里,仅 wakeAll 自己的 cond_ 无法解锁它,
    // 导致上层 wait() 永久挂起(GUI 卡死)
    if (audio_player_) audio_player_->stop();
    cond_.wakeAll();  // 唤醒可能在等待的取帧者、队列满等待、暂停等待
}

DecodedFrame PlaybackDecoder::tryTakeFrame() {
    QMutexLocker lk(&mutex_);
    if (!queue_.isEmpty()) {
        DecodedFrame f = queue_.dequeue();
        cond_.wakeOne();
        return f;
    }
    return {};  // 队列空
}

bool PlaybackDecoder::hasFrame() const {
    QMutexLocker lk(&mutex_);
    return !queue_.isEmpty();
}

void PlaybackDecoder::run() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame*  vframe = av_frame_alloc();   // 视频帧
    AVFrame*  aframe = nullptr;            // 音频帧(按需分配)
    // RGB24 目标缓冲
    const int rgb_buf_size = av_image_get_buffer_size(
        AV_PIX_FMT_RGB24, width_, height_, 1);
    uint8_t* rgb_buf = (rgb_buf_size > 0)
        ? static_cast<uint8_t*>(av_malloc(rgb_buf_size)) : nullptr;
    uint8_t* rgb_data[4] = {rgb_buf, nullptr, nullptr, nullptr};
    int rgb_linesize[4] = {0, 0, 0, 0};

    // 音频重采样输出缓冲(S16LE interleaved),按需分配、按需扩容
    uint8_t* pcm_buf     = nullptr;
    int      pcm_buf_cap = 0;
    if (audio_stream_ >= 0)
        aframe = av_frame_alloc();

    // 任一必需资源分配失败 → 清理返回(close() 负责释放 PlaybackDecoder 成员)
    if (!pkt || !vframe || !rgb_buf ||
        (audio_stream_ >= 0 && !aframe)) {
        av_frame_free(&aframe);
        av_frame_free(&vframe);
        av_packet_free(&pkt);
        av_free(rgb_buf);
        return;
    }

    av_image_fill_arrays(rgb_data, rgb_linesize, rgb_buf,
                         AV_PIX_FMT_RGB24, width_, height_, 1);

    // 把一个视频帧转为 RGB 并入队,返回 false 表示 stop
    auto handleVideoFrame = [&](AVFrame* f, qint64 override_pts_ms) -> bool {
        sws_scale(sws_ctx_, f->data, f->linesize, 0, height_,
                  rgb_data, rgb_linesize);
        QImage img(rgb_buf, width_, height_, rgb_linesize[0],
                   QImage::Format_RGB888);
        QImage copy = img.copy();
        qint64 pos_ms = override_pts_ms;
        if (pos_ms < 0) {
            auto* stream = fmt_ctx_->streams[video_stream_];
            pos_ms = (f->pts != AV_NOPTS_VALUE)
                ? f->pts * av_q2d(stream->time_base) * 1000 : 0;
        }
        {
            QMutexLocker lk(&mutex_);
            // 队列满时,若 paused_ 也需阻塞(否则 pause 瞬间队列已满,
            // 解码线程卡死在这里,pauseAudio 的 cond_.wakeAll 唤醒后
            // 队列仍满、paused_ 仍 0,循环条件 !paused_ 不成立,会继续等待)
            while (queue_.size() >= kMaxQueue && !stop_ && !paused_)
                cond_.wait(&mutex_);
            if (stop_ || paused_) return false;
            queue_.enqueue({copy, pos_ms});
        }
        cond_.wakeOne();
        return true;
    };

    // 把一个音频帧重采样为 S16LE 并送入 QAudioOutput。返回 false 表示 stop。
    // 1x:解码帧 → swr → S16LE → QAudioOutput
    // 倍速:解码帧 → abuffersrc(atempo) → abuffersink → swr → S16LE → QAudioOutput
    auto handleAudioFrame = [&](AVFrame* f) -> bool {
        if (!swr_ctx_ || !audio_player_) return false;
        // 首帧记录起始 PTS(音画同步基准,用原始帧 PTS)。
        // 首帧 PTS 可能为合法负值(AAC priming delay),用标志位区分
        // "未捕获"与"负 PTS",避免被误判为未初始化而反复覆盖
        if (!audio_clock_started_ && f->pts != AV_NOPTS_VALUE) {
            auto* astream = fmt_ctx_->streams[audio_stream_];
            audio_start_pts_ms_ =
                f->pts * av_q2d(astream->time_base) * 1000;
            audio_clock_started_ = true;
        }

        // 暂停/停止时不处理:返回 false 让解码循环退出到暂停等待点,帧丢弃。
        // 丢弃帧不计入 input_samples——保持"已解码计数"与"实际进入播放
        // 管线"严格一致;否则每次暂停音频时钟虚增 ~21ms(1024 样本@48k),
        // 频繁暂停会累积成可感知的音画偏移。
        if (paused_.load() || stop_.load()) return false;

        // 把一个已格式匹配的帧送入 swr → QAudioOutput
        auto writeFrame = [&](AVFrame* fr) -> bool {
            int samples     = fr->nb_samples;
            int out_samples = swr_get_out_samples(swr_ctx_, samples);
            int pcm_size    = out_samples * audio_channels_ * 2;  // S16
            if (pcm_size > pcm_buf_cap) {
                av_free(pcm_buf);
                pcm_buf = static_cast<uint8_t*>(av_malloc(pcm_size));
                if (!pcm_buf) {
                    pcm_buf_cap = 0;  // 同步归零,下次扩容时重新分配
                    return false;
                }
                pcm_buf_cap = pcm_size;
            }
            const uint8_t* in_data[AV_NUM_DATA_POINTERS];
            for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i)
                in_data[i] = fr->data[i];
            int got = swr_convert(swr_ctx_, &pcm_buf, out_samples,
                                  in_data, samples);
            if (got > 0) {
                int bytes = got * audio_channels_ * 2;
                if (!audio_player_->write(pcm_buf, bytes))
                    return false;  // stop
            }
            return true;
        };

        // 累加输入样本数(atempo 前的解码样本)。
        // input_samples 是精确的内容时间,不受 atempo 比例偏差影响,
        // 用于 audioClockMsDouble 计算精确时钟,避免 output_played*speed 的累积漂移。
        // 必须在 atempo 之前累加(此处 f 是原始解码帧),且只在帧确定进入
        // 播放管线后计数:1x 在写入成功后,倍速在本帧全部输出写完后——
        // 被中止(暂停/速度切换落在中途)的帧不虚增计数,graph 丢弃时账目一致。
        if (afilter_graph_ && abuffersrc_ctx_ && abuffersink_ctx_) {
            // 倍速:送入 filter graph,取出变速后的帧再走 swr
            int ret = av_buffersrc_add_frame_flags(
                abuffersrc_ctx_, f, AV_BUFFERSRC_FLAG_KEEP_REF);
            if (ret < 0) return true;  // 单帧入 graph 失败,跳过不致命
            AVFrame* filtered = av_frame_alloc();
            bool drain_ok = true;
            while (true) {
                ret = av_buffersink_get_frame(abuffersink_ctx_, filtered);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) break;
                if (!writeFrame(filtered)) {
                    drain_ok = false;
                    av_frame_unref(filtered);
                    break;
                }
                av_frame_unref(filtered);
            }
            av_frame_free(&filtered);
            if (!drain_ok) {
                // drain 被中止(write 拒绝):本帧不计数,graph 内残留状态随
                // 后续重建(速度切换)或续播(暂停)处理,误差有界 ≤1 帧。
                return false;
            }
            audio_player_->addInputSamples(f->nb_samples);
            return true;
        } else {
            // 1x:直接走 swr,写入成功才计数
            if (!writeFrame(f)) return false;
            audio_player_->addInputSamples(f->nb_samples);
            return true;
        }
    };

    while (!stop_) {
        // BUG3 修复:pending_audio_speed_ 由 GUI 线程写入,
        // 解码线程在这里检测并应用,确保 filter graph 的创建/销毁/使用
        // 全部在解码线程内完成,与 handleAudioFrame 里的 graph 访问零竞争。
        int pending = pending_audio_speed_.load();
        if (pending != audio_speed_) {
            // 时钟基准 + prev_audio_speed_ 已在 setAudioSpeed() 完成:
            // audio_start_pts_ms_ = 当前时钟,prev_audio_speed_ = 旧速率,
            // beginSpeedChange 记录 old_data_end_(旧数据边界,不清缓冲)。
            // 此处只更新 audio_speed_ + 重建 atempo graph + endSpeedChange,
            // 不触碰 old_data_end_,否则会破坏旧/新数据边界。
            audio_speed_ = pending;
            freeAudioFilterGraph();
            if (audio_speed_ > 1) {
                if (!initAudioFilterGraph(audio_speed_)) {
                    freeAudioFilterGraph();
                    audio_speed_ = 1;  // 降级为静音,但时钟仍可估
                    pending_audio_speed_.store(1);
                }
            }
            // 速度切换完成:恢复 write(),解码线程开始用新 atempo graph
            // 产出音频数据写入缓冲。QAudioOutput 全程未中断,直接消费新数据。
            if (audio_player_) audio_player_->endSpeedChange();
        }

        // 暂停点:解码线程在 paused_=1 时阻塞,避免暂停期间音频数据
        // 被提前解码并丢弃(BUG2)。stop_ 优先级更高。
        if (paused_.load()) {
            QMutexLocker lk(&mutex_);
            while (paused_.load() && !stop_)
                cond_.wait(&mutex_);
            if (stop_) break;
        }
        // 读取下一个 packet
        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            // 文件读完,flush 两个解码器(送 NULL packet 取出剩余帧)
            av_packet_unref(pkt);
            // flush 视频
            avcodec_send_packet(codec_ctx_, nullptr);
            while (true) {
                ret = avcodec_receive_frame(codec_ctx_, vframe);
                if (ret < 0) break;
                // flush 帧的 PTS 若编码器没给则用 -1(由 handleVideoFrame 兜底)
                qint64 flush_pts = (vframe->pts != AV_NOPTS_VALUE) ? -1 : 0;
                if (!handleVideoFrame(vframe, flush_pts)) break;
            }
            // flush 音频
            if (audio_codec_ctx_ && audio_player_) {
                avcodec_send_packet(audio_codec_ctx_, nullptr);
                while (true) {
                    ret = avcodec_receive_frame(audio_codec_ctx_, aframe);
                    if (ret < 0) break;
                    if (!handleAudioFrame(aframe)) break;
                }
            }
            break;  // EOF
        }

        if (pkt->stream_index == video_stream_) {
            ret = avcodec_send_packet(codec_ctx_, pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;
            while (ret >= 0 && !stop_) {
                ret = avcodec_receive_frame(codec_ctx_, vframe);
                if (ret < 0) break;
                if (!handleVideoFrame(vframe, -1)) break;
            }
        } else if (pkt->stream_index == audio_stream_ && audio_player_) {
            ret = avcodec_send_packet(audio_codec_ctx_, pkt);
            av_packet_unref(pkt);
            if (ret < 0) continue;
            while (ret >= 0 && !stop_) {
                ret = avcodec_receive_frame(audio_codec_ctx_, aframe);
                if (ret < 0) break;
                if (!handleAudioFrame(aframe)) break;
            }
        } else {
            // 不关心的流(字幕等):丢弃
            av_packet_unref(pkt);
        }
    }

    // 标记 EOF(解码线程退出)
    eof_ = 1;
    cond_.wakeAll();

    if (aframe) av_frame_free(&aframe);
    av_frame_free(&vframe);
    av_packet_free(&pkt);
    av_free(rgb_buf);
    if (pcm_buf) av_free(pcm_buf);
}

} // namespace re
