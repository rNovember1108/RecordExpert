#include "Mp4Writer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace re {

Mp4Writer::~Mp4Writer() { Close(); }

bool Mp4Writer::OpenMuxer(const char* output_path,
                          const std::vector<AVCodecContext*>& encs,
                          std::vector<AVStream*>& out_streams_out) {
    // Alloc 新 output context（每次分段 Rotate 时也复用这段逻辑）。
    if (out_fmt_ctx_) Close();

    int ret = avformat_alloc_output_context2(&out_fmt_ctx_, nullptr,
                                              nullptr, output_path);
    if (ret < 0 || !out_fmt_ctx_) {
        out_fmt_ctx_ = nullptr;
        return false;
    }

    // NeedsGlobalHeader：编码器是否已设置 GLOBAL_HEADER（MP4 容器需要）。
    const bool need_global = NeedsGlobalHeader();

    // 为每个编码器创建 stream；Rotate 后 stream 会失效（out_fmt_ctx_ 换了），
    // 但 producer 持有的 stream_ 指针会通过 SetStream 刷新。
    out_streams_out.clear();
    for (AVCodecContext* enc : encs) {
        if (need_global) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVStream* stream = avformat_new_stream(out_fmt_ctx_, nullptr);
        if (!stream) { Close(); return false; }
        stream->time_base = enc->time_base;
        if (avcodec_parameters_from_context(stream->codecpar, enc) < 0) {
            Close(); return false;
        }
        out_streams_out.push_back(stream);
    }

    // avio_open（如果输出格式需要文件 IO）+ WriteHeader。
    if (!(out_fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx_->pb, out_fmt_ctx_->url, AVIO_FLAG_WRITE) < 0) {
            Close(); return false;
        }
    }

    if (avformat_write_header(out_fmt_ctx_, nullptr) < 0) {
        Close(); return false;
    }
    header_written_ = true;
    return true;
}

bool Mp4Writer::NeedsGlobalHeader() const {
    return out_fmt_ctx_ &&
           (out_fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) != 0;
}

// 写入一个已经 rescale 好 stream time_base 的 AVPacket。muxer worker 单线程
// 调用，不需要锁。pts_offset 让新 segment 的 packet.pts 从 0 起算。
int Mp4Writer::WritePacket(AVStream* stream, int64_t pts_offset, AVPacket* pkt) {
    if (!out_fmt_ctx_ || !stream || !pkt) return -1;
    pkt->stream_index = stream->index;
    if (pts_offset != 0) {
        pkt->pts -= pts_offset;
        pkt->dts -= pts_offset;
    }
    return av_interleaved_write_frame(out_fmt_ctx_, pkt);
}

// 分段切换：旧文件 trailer → avio_close → free_context；新文件 Alloc → NewStream × N → WriteHeader。
// encoder 不重建（producer 全程持有同一个 enc_ctx_），但 stream 必须重建（out_fmt_ctx_ 换了）。
// out_streams_out 按 encs 顺序输出新 stream 指针，供 producer SetStream 刷新。
bool Mp4Writer::RotateMuxer(const char* next_path,
                            const std::vector<AVCodecContext*>& encs,
                            std::vector<AVStream*>& out_streams_out) {
    // 旧文件收尾（trailer + avio_close + free_context）。
    if (out_fmt_ctx_) {
        if (header_written_) {
            av_write_trailer(out_fmt_ctx_);
            header_written_ = false;
        }
        if (!(out_fmt_ctx_->oformat->flags & AVFMT_NOFILE) && out_fmt_ctx_->pb) {
            avio_closep(&out_fmt_ctx_->pb);
        }
        avformat_free_context(out_fmt_ctx_);
        out_fmt_ctx_ = nullptr;
    }

    // 复用 OpenMuxer 的 Alloc + NewStream + WriteHeader 逻辑。
    std::vector<AVStream*> new_streams;
    if (!OpenMuxer(next_path, encs, new_streams)) return false;

    // 收集新 stream 指针（encs 顺序 = stream 顺序）。
    out_streams_out = new_streams;
    return true;
}

void Mp4Writer::Close() {
    if (out_fmt_ctx_) {
        if (header_written_) {
            av_write_trailer(out_fmt_ctx_);
            header_written_ = false;
        }
        if (!(out_fmt_ctx_->oformat->flags & AVFMT_NOFILE) && out_fmt_ctx_->pb) {
            avio_closep(&out_fmt_ctx_->pb);
        }
        avformat_free_context(out_fmt_ctx_);
        out_fmt_ctx_ = nullptr;
    }
}

}  // namespace re
