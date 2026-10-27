#include "audio_waveform.h"
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <sys/stat.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

namespace preview {
namespace {
struct Decode {
    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    AVFormatContext* input=avformat_alloc_context();
    AVCodecContext* codec=nullptr;
    AVPacket* packet=av_packet_alloc();
    AVFrame* frame=av_frame_alloc();
    bool interrupted() const { return stop.stop_requested() || std::chrono::steady_clock::now()>=deadline; }
    ~Decode() {
        av_frame_free(&frame); av_packet_free(&packet);
        avcodec_free_context(&codec); avformat_close_input(&input);
    }
};
template<class T> double scalar(const uint8_t* bytes) {
    T value; std::memcpy(&value,bytes,sizeof value); return double(value);
}
float sample(const uint8_t* bytes,AVSampleFormat format) {
    double value=0;
    switch (format) {
    case AV_SAMPLE_FMT_U8: value=(double(*bytes)-128)/128; break;
    case AV_SAMPLE_FMT_S16: value=scalar<int16_t>(bytes)/32768; break;
    case AV_SAMPLE_FMT_S32: value=scalar<int32_t>(bytes)/2147483648.; break;
    case AV_SAMPLE_FMT_S64: value=scalar<int64_t>(bytes)/9223372036854775808.; break;
    case AV_SAMPLE_FMT_FLT: value=scalar<float>(bytes); break;
    case AV_SAMPLE_FMT_DBL: value=scalar<double>(bytes); break;
    default: break;
    }
    return std::isfinite(value)?float(std::clamp(value,-1.,1.)):0;
}
}

std::shared_ptr<const AudioWaveform> decodeAudioWaveform(const QString& path,double duration,std::stop_token stop) {
    if (stop.stop_requested() || !std::isfinite(duration) || duration<=0 || duration>21600 || !QFileInfo(path).isFile()) return {};
    const auto local=QFile::encodeName(path);
    struct stat before{},after{};
    if (::stat(local.constData(),&before)!=0 || !S_ISREG(before.st_mode)) return {};
    Decode d{stop};
    if (!d.input || !d.frame || !d.packet) return {};
    d.input->interrupt_callback={[](void* opaque) { return int(static_cast<Decode*>(opaque)->interrupted()); },&d};
    d.input->probesize=1024*1024;
    d.input->max_analyze_duration=2*AV_TIME_BASE;
    AVDictionary* options=nullptr;
    av_dict_set(&options,"protocol_whitelist","file",0);
    // Exclude playlists and reference demuxers. MOV external data references
    // remain disabled by FFmpeg's default enable_drefs=0.
    av_dict_set(&options,"format_whitelist","wav,mp3,flac,ogg,mov,matroska,webm,aac,aiff,asf,ape",0);
    const int opened=avformat_open_input(&d.input,local.constData(),nullptr,&options);
    av_dict_free(&options);
    if (opened<0 || d.interrupted() || avformat_find_stream_info(d.input,nullptr)<0) return {};
    const AVCodec* decoder=nullptr;
    const int index=av_find_best_stream(d.input,AVMEDIA_TYPE_AUDIO,-1,-1,&decoder,0);
    if (index<0 || !decoder) return {};
    auto* stream=d.input->streams[index];
    d.codec=avcodec_alloc_context3(decoder);
    if (!d.codec || avcodec_parameters_to_context(d.codec,stream->codecpar)<0) return {};
    d.codec->thread_count=1;
    d.codec->pkt_timebase=stream->time_base;
    if (avcodec_open2(d.codec,decoder,nullptr)<0) return {};
    auto result=std::make_shared<AudioWaveform>(); result->duration=duration;
    double nextTime=0;
    bool haveSamples=false;
    const auto receive=[&]() {
        while (!d.interrupted()) {
            const int status=avcodec_receive_frame(d.codec,d.frame);
            if (status==AVERROR(EAGAIN) || status==AVERROR_EOF) return true;
            if (status<0) return false;
            const auto& frame=*d.frame;
            const auto format=AVSampleFormat(frame.format);
            const int bytes=av_get_bytes_per_sample(format), channels=frame.ch_layout.nb_channels;
            if (bytes<=0 || channels<1 || channels>64 || frame.sample_rate<=0 || frame.nb_samples<0 || frame.nb_samples>1048576) return false;
            const bool planar=av_sample_fmt_is_planar(format);
            const auto packed=av_get_packed_sample_fmt(format);
            const double start=frame.best_effort_timestamp==AV_NOPTS_VALUE?nextTime:
                (double(frame.best_effort_timestamp)-(stream->start_time==AV_NOPTS_VALUE?0:double(stream->start_time)))*av_q2d(stream->time_base);
            for (int i=0;i<frame.nb_samples;++i) {
                if ((i%4096)==0 && d.interrupted()) return false;
                const double at=start+double(i)/frame.sample_rate;
                if (at<0 || at>=duration) continue;
                auto& bin=result->bins[std::clamp(int(at/duration*AudioWaveform::binCount),0,AudioWaveform::binCount-1)];
                for (int ch=0;ch<2;++ch) {
                    const int source=std::min(ch,channels-1);
                    const auto* data=frame.extended_data[planar?source:0];
                    if (!data) return false;
                    const auto value=sample(data+size_t(planar?i:i*channels+source)*bytes,packed);
                    bin[ch*2]=std::min(bin[ch*2],value); bin[ch*2+1]=std::max(bin[ch*2+1],value);
                    result->peak=std::max(result->peak,std::abs(value));
                }
                haveSamples=true;
            }
            nextTime=start+double(frame.nb_samples)/frame.sample_rate;
            result->channels=std::min(channels,2);
            av_frame_unref(d.frame);
        }
        return false;
    };
    int status=0;
    while (!d.interrupted() && (status=av_read_frame(d.input,d.packet))>=0) {
        if (d.packet->stream_index==index) {
            if (avcodec_send_packet(d.codec,d.packet)<0 || !receive()) return {};
        }
        av_packet_unref(d.packet);
    }
    if (d.interrupted() || status!=AVERROR_EOF || avcodec_send_packet(d.codec,nullptr)<0 || !receive() || !haveSamples) return {};
    if (::stat(local.constData(),&after)!=0 || before.st_dev!=after.st_dev || before.st_ino!=after.st_ino ||
        before.st_size!=after.st_size || before.st_mtim.tv_sec!=after.st_mtim.tv_sec || before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec!=after.st_ctim.tv_sec || before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec) return {};
    return result;
}
}
