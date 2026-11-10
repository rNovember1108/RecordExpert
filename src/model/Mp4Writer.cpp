#include "Mp4Writer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace re {

Mp4Writer::~Mp4Writer() { Close(); }

bool Mp4Writer::Alloc(const char* output_path) {
    // nullptr format_name = 按输出文件名后缀猜测（普通 MP4）。
    int ret = avformat_alloc_output_context2(&out_fmt_ctx_, nullptr,
                                              nullptr, output_path);
    return ret >= 0 && out_fmt_ctx_;
}

bool Mp4Writer::NeedsGlobalHeader() const {
    return out_fmt_ctx_ &&
           (out_fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) != 0;
}

AVStream* Mp4Writer::NewStream(AVCodecContext* enc) {
    AVStream* stream = avformat_new_stream(out_fmt_ctx_, nullptr);
    if (!stream) return nullptr;
    stream->time_base = enc->time_base;
    int ret = avcodec_parameters_from_context(stream->codecpar, enc);
    if (ret < 0) return nullptr;
    return stream;
}

bool Mp4Writer::WriteHeader() {
    if (!(out_fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&out_fmt_ctx_->pb, out_fmt_ctx_->url,
                            AVIO_FLAG_WRITE);
        if (ret < 0) return false;
    }

    int ret = avformat_write_header(out_fmt_ctx_, nullptr);
    if (ret < 0) return false;
    header_written_ = true;
    return true;
}

bool Mp4Writer::EncodeAndWrite(AVCodecContext* enc, AVStream* stream,
                                AVFrame* frame) {
    int ret = avcodec_send_frame(enc, frame);
    if (ret < 0) return false;

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    bool ok = true;
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) { ok = false; break; }

        // 把包的时间戳从编码器时基换算到流时基。
        av_packet_rescale_ts(pkt, enc->time_base, stream->time_base);
        pkt->stream_index = stream->index;

        {
            // 视频与音频线程共享同一个输出上下文，因此对 muxer 的调用必须串行化。
            std::lock_guard<std::mutex> lock(write_mutex_);
            ret = av_interleaved_write_frame(out_fmt_ctx_, pkt);
        }
        av_packet_unref(pkt);
        if (ret < 0) { ok = false; break; }
    }

    av_packet_free(&pkt);
    return ok;
}

void Mp4Writer::Close() {
    // 仅当已写过头时，才写一次 trailer。
    if (out_fmt_ctx_ && header_written_) {
        av_write_trailer(out_fmt_ctx_);
        header_written_ = false;
    }

    if (out_fmt_ctx_) {
        if (!(out_fmt_ctx_->oformat->flags & AVFMT_NOFILE) &&
            out_fmt_ctx_->pb) {
            avio_closep(&out_fmt_ctx_->pb);
        }
        avformat_free_context(out_fmt_ctx_);
        out_fmt_ctx_ = nullptr;
    }
}

}  // namespace re
