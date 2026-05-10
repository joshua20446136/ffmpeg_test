#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/timeb.h>
#include <direct.h>
#include <windows.h>
#include <winsvc.h>
#include <stdarg.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>

#define SEGMENT_DURATION 60.0
#define RECONNECT_DELAY 3000000
#define PYTHON_SCRIPT "get_rtsp.py"
#define LOG_FILE "record.log"
#define OUTPUT_EXTENSION "mp4"
#define MAX_STREAMS 64

#define SERVICE_NAME "RTSPRecorder"
#define SERVICE_DISPLAY_NAME "RTSP Recorder Service"
#define SERVICE_DESC_TEXT "RTSP Camera Recorder Service"

char g_base_dir[MAX_PATH] = {0};
char g_log_file[MAX_PATH] = {0};
char g_python_script[MAX_PATH] = {0};

FILE* log_fp = NULL;
SERVICE_STATUS g_ServiceStatus;
SERVICE_STATUS_HANDLE g_ServiceStatusHandle;

void write_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    const char* log_path = g_log_file[0] ? g_log_file : LOG_FILE;
    log_fp = fopen(log_path, "a+");
    if (!log_fp) return;
    fprintf(log_fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
        t->tm_hour, t->tm_min, t->tm_sec);
    vfprintf(log_fp, fmt, ap);
    fflush(log_fp);
    fclose(log_fp);
    va_end(ap);
}

static int win_path_to_utf8(const char *path, char *utf8_path, int utf8_path_size) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    if (!wlen) return -1;

    wchar_t *wpath = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wpath) return -1;

    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, wlen)) {
        free(wpath);
        return -1;
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, utf8_path, utf8_path_size, NULL, NULL);
    free(wpath);
    return len > 0 ? 0 : -1;
}

void init_service_paths() {
    char module_path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, module_path, MAX_PATH)) {
        char* p = strrchr(module_path, '\\');
        if (p) {
            *p = '\0';
        }
        strncpy(g_base_dir, module_path, MAX_PATH - 1);
    } else {
        strncpy(g_base_dir, ".", MAX_PATH - 1);
    }

    snprintf(g_log_file, sizeof(g_log_file), "%s\\%s", g_base_dir, LOG_FILE);
    snprintf(g_python_script, sizeof(g_python_script), "%s\\%s", g_base_dir, PYTHON_SCRIPT);
}

char* get_new_rtsp_url() {
    static char rtsp_url[1024] = {0};
    char cmd[1024];
    const char* python_path = g_python_script[0] ? g_python_script : PYTHON_SCRIPT;
    snprintf(cmd, sizeof(cmd), "python \"%s\"", python_path);
    FILE* fp = _popen(cmd, "r");
    if (!fp) {
        write_log("Failed to call Python script\n");
        return rtsp_url;
    }
    if (fgets(rtsp_url, sizeof(rtsp_url), fp)) {
        size_t len = strlen(rtsp_url);
        if (len > 0 && (rtsp_url[len-1] == '\n' || rtsp_url[len-1] == '\r'))
            rtsp_url[len-1] = 0;
    }
    _pclose(fp);
    write_log("Got RTSP URL: %s\n", rtsp_url);
    return rtsp_url;
}

void create_day_dir(char* dir) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char day_name[256];
    strftime(day_name, sizeof(day_name), "%Y%m%d", t);
    snprintf(dir, 256, "%s\\%s", g_base_dir[0] ? g_base_dir : ".", day_name);
    if (!CreateDirectory(dir, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            write_log("CreateDirectory failed for %s: %lu\n", dir, err);
        }
    }
    write_log("Using output directory: %s\n", dir);
}

static void av_log_error_str(int err, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    av_strerror(err, buf, buf_size);
}

void create_filepath(char* path) {
    char dir[256];
    create_day_dir(dir);
    struct _timeb tb;
    _ftime(&tb);
    struct tm* t = localtime(&tb.time);
    int ms = tb.millitm;
    snprintf(path, 512, "%s\\%04d%02d%02d%02d%02d%02d-%03d00.%s",
        dir,
        t->tm_year + 1900,
        t->tm_mon + 1,
        t->tm_mday,
        t->tm_hour,
        t->tm_min,
        t->tm_sec,
        ms,
        OUTPUT_EXTENSION);
    write_log("Created output filepath: %s\n", path);
}

static void free_audio_transcoding(AVCodecContext** audio_dec_ctx, AVCodecContext** audio_enc_ctx, SwrContext** swr_ctx, AVAudioFifo** audio_fifo, unsigned int nb_streams) {
    for (unsigned int i = 0; i < nb_streams; i++) {
        if (audio_dec_ctx[i]) {
            avcodec_free_context(&audio_dec_ctx[i]);
        }
        if (audio_enc_ctx[i]) {
            avcodec_free_context(&audio_enc_ctx[i]);
        }
        if (swr_ctx[i]) {
            swr_free(&swr_ctx[i]);
        }
        if (audio_fifo[i]) {
            av_audio_fifo_free(audio_fifo[i]);
        }
    }
}

static int open_audio_encoder(AVCodecContext* dec_ctx, AVCodecContext** enc_ctx, SwrContext** swr_ctx);

static int setup_output_stream(AVFormatContext* ofmt_ctx, AVFormatContext* ifmt_ctx,
    AVCodecContext** audio_dec_ctx, AVCodecContext** audio_enc_ctx,
    SwrContext** swr_ctx, AVAudioFifo** audio_fifo, int* stream_mapping) {

    for (unsigned int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* in_stream = ifmt_ctx->streams[i];
        AVCodecParameters* in_codecpar = in_stream->codecpar;
        stream_mapping[i] = -1;

        // ===================== 🔥 终极修复：直接复制所有流，不转码！=====================
        AVStream* out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            return AVERROR_UNKNOWN;
        }

        // 直接复制编码器参数，不做任何音频转码！
        if (avcodec_parameters_copy(out_stream->codecpar, in_codecpar) < 0) {
            return AVERROR_UNKNOWN;
        }
        out_stream->time_base = in_stream->time_base;
        stream_mapping[i] = out_stream->index;
    }

    return 0;
}

static int open_audio_encoder(AVCodecContext* dec_ctx, AVCodecContext** enc_ctx, SwrContext** swr_ctx)
{
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!encoder) {
        return AVERROR_ENCODER_NOT_FOUND;
    }

    *enc_ctx = avcodec_alloc_context3(encoder);
    if (!*enc_ctx) {
        return AVERROR(ENOMEM);
    }

    // 新版 FFmpeg 通道设置
    av_channel_layout_copy(&(*enc_ctx)->ch_layout, &dec_ctx->ch_layout);
    (*enc_ctx)->sample_rate = dec_ctx->sample_rate;
    (*enc_ctx)->sample_fmt = AV_SAMPLE_FMT_FLTP;
    (*enc_ctx)->bit_rate = 64000;
    (*enc_ctx)->time_base = (AVRational){1, dec_ctx->sample_rate};
    (*enc_ctx)->frame_size = 1024;

    if (avcodec_open2(*enc_ctx, encoder, NULL) < 0) {
        avcodec_free_context(enc_ctx);
        return -1;
    }

    // 正确创建重采样上下文（新版API）
    *swr_ctx = swr_alloc();
    if (!*swr_ctx) {
        avcodec_free_context(enc_ctx);
        return -1;
    }

    // 设置重采样参数
    av_opt_set_chlayout(*swr_ctx, "out_chlayout", &(*enc_ctx)->ch_layout, 0);
    av_opt_set_int(*swr_ctx, "out_sample_fmt", (*enc_ctx)->sample_fmt, 0);
    av_opt_set_int(*swr_ctx, "out_sample_rate", (*enc_ctx)->sample_rate, 0);

    av_opt_set_chlayout(*swr_ctx, "in_chlayout", &dec_ctx->ch_layout, 0);
    av_opt_set_int(*swr_ctx, "in_sample_fmt", dec_ctx->sample_fmt, 0);
    av_opt_set_int(*swr_ctx, "in_sample_rate", dec_ctx->sample_rate, 0);

    if (swr_init(*swr_ctx) < 0) {
        swr_free(swr_ctx);
        avcodec_free_context(enc_ctx);
        return -1;
    }

    return 0;
}

static int write_encoded_audio_packet(AVFormatContext* ofmt_ctx, AVPacket* pkt,
    AVCodecContext* enc_ctx, AVStream* out_stream) {
    int ret;
    while ((ret = avcodec_receive_packet(enc_ctx, pkt)) >= 0) {
        av_packet_rescale_ts(pkt, enc_ctx->time_base, out_stream->time_base);
        pkt->stream_index = out_stream->index;
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            return ret;
        }
    }
    av_packet_unref(pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
    }
    return ret;
}

static int encode_audio_from_fifo(AVFormatContext* ofmt_ctx, AVAudioFifo* fifo,
    AVCodecContext* enc_ctx, AVStream* out_stream, AVPacket* pkt, int64_t* audio_pts) {
    int ret;
    int frame_size = enc_ctx->frame_size;

    while (av_audio_fifo_size(fifo) >= frame_size) {
        AVFrame* output_frame = av_frame_alloc();
        if (!output_frame) return AVERROR(ENOMEM);

        av_channel_layout_copy(&output_frame->ch_layout, &enc_ctx->ch_layout);
        output_frame->sample_rate = enc_ctx->sample_rate;
        output_frame->format = enc_ctx->sample_fmt;
        output_frame->nb_samples = frame_size;

        ret = av_frame_get_buffer(output_frame, 0);
        if (ret < 0) {
            av_frame_free(&output_frame);
            return ret;
        }

        ret = av_audio_fifo_read(fifo, (void**)output_frame->data, frame_size);
        if (ret < 0) {
            av_frame_free(&output_frame);
            return ret;
        }

        // 正确 PTS
        output_frame->pts = *audio_pts;
        *audio_pts += frame_size;

        // 发送帧
        ret = avcodec_send_frame(enc_ctx, output_frame);
        if (ret < 0) {
            av_frame_free(&output_frame);
            return ret;
        }
        av_frame_free(&output_frame);

        ret = write_encoded_audio_packet(ofmt_ctx, pkt, enc_ctx, out_stream);
        if (ret < 0) return ret;
    }
    return 0;
}

static int flush_audio_fifo(AVFormatContext* ofmt_ctx, AVAudioFifo* fifo,
    AVCodecContext* enc_ctx, AVStream* out_stream, AVPacket* pkt, int64_t* audio_pts) {
    av_audio_fifo_reset(fifo);
    return 0;
}

static int transcode_audio_frame(AVAudioFifo* fifo, AVCodecContext* enc_ctx,
    SwrContext* swr_ctx, AVFrame* frame, AVFrame* resampled)
{
    int ret;
    av_frame_unref(resampled);

    resampled->format = enc_ctx->sample_fmt;
    av_channel_layout_copy(&resampled->ch_layout, &enc_ctx->ch_layout);
    resampled->sample_rate = enc_ctx->sample_rate;
    resampled->nb_samples = enc_ctx->frame_size;

    if (av_frame_get_buffer(resampled, 0) < 0)
        return -1;

    ret = swr_convert_frame(swr_ctx, resampled, frame);
    if (ret < 0)
        return ret;

    if (av_audio_fifo_write(fifo, (void**)resampled->data, resampled->nb_samples) < 0)
        return -1;

    return 0;
}

static int transcode_audio_packet(AVFormatContext* ofmt_ctx, AVCodecContext* dec_ctx,
    AVCodecContext* enc_ctx, SwrContext* swr_ctx, AVAudioFifo* fifo,
    AVPacket* pkt, AVFrame* frame, AVFrame* resampled, AVPacket* enc_pkt,
    AVStream* out_stream, int64_t* audio_pts) {
    int ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) return ret;

    while ((ret = avcodec_receive_frame(dec_ctx, frame)) == 0) {
        // ===================== 【终极防御】过滤无效音频帧 =====================
        if (frame->nb_samples <= 0 || !frame->data[0]) {
            av_frame_unref(frame);
            continue;
        }

        frame->time_base = dec_ctx->time_base;

        ret = transcode_audio_frame(fifo, enc_ctx, swr_ctx, frame, resampled);
        if (ret < 0) {
            av_frame_unref(frame);
            return ret;
        }

        ret = encode_audio_from_fifo(ofmt_ctx, fifo, enc_ctx, out_stream, enc_pkt, audio_pts);
        if (ret < 0) {
            av_frame_unref(frame);
            return ret;
        }

        av_frame_unref(frame);
    }

    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return 0;
    return ret;
}

static int flush_audio_encoder(AVFormatContext* ofmt_ctx, AVCodecContext* enc_ctx,
    AVPacket* pkt, AVStream* out_stream) {
    int ret = avcodec_send_frame(enc_ctx, NULL);
    if (ret < 0) return ret;

    return write_encoded_audio_packet(ofmt_ctx, pkt, enc_ctx, out_stream);
}

int start_record(const char* rtsp_url) {
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket* pkt = av_packet_alloc();
    int ret, i;
    int64_t start_time = 0;
    double duration = 0;
    char filepath[512];

    if (!pkt) {
        write_log("Memory allocation failed\n");
        return -1;
    }

    write_log("Starting recording: %s\n", rtsp_url);

    AVDictionary* options = NULL;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "10000000", 0);
    av_dict_set(&options, "max_delay", "5000000", 0);

    ret = avformat_open_input(&ifmt_ctx, rtsp_url, NULL, &options);
    av_dict_free(&options);
    if (ret < 0) {
        write_log("Failed to open RTSP: %d\n", ret);
        av_packet_free(&pkt);
        return ret;
    }

    int stream_mapping[MAX_STREAMS];
    AVCodecContext* audio_dec_ctx[MAX_STREAMS] = {0};
    AVCodecContext* audio_enc_ctx[MAX_STREAMS] = {0};
    SwrContext* swr_ctx[MAX_STREAMS] = {0};
    AVAudioFifo* audio_fifo[MAX_STREAMS] = {0};
    int64_t audio_pts[MAX_STREAMS] = {0};
    AVFrame* audio_frame = av_frame_alloc();
    AVFrame* resampled_frame = av_frame_alloc();
    AVPacket* enc_pkt = av_packet_alloc();

    if (!audio_frame || !resampled_frame || !enc_pkt) {
        write_log("Audio buffer allocation failed\n");
        av_frame_free(&audio_frame);
        av_frame_free(&resampled_frame);
        av_packet_free(&enc_pkt);
        avformat_close_input(&ifmt_ctx);
        av_packet_free(&pkt);
        return AVERROR(ENOMEM);
    }

    if (avformat_find_stream_info(ifmt_ctx, NULL) < 0) {
        write_log("Failed to find stream info\n");
        av_frame_free(&audio_frame);
        av_frame_free(&resampled_frame);
        av_packet_free(&enc_pkt);
        avformat_close_input(&ifmt_ctx);
        av_packet_free(&pkt);
        return -1;
    }

    start_time = av_gettime();
    create_filepath(filepath);

    char utf8_filepath[1024] = {0};
    if (win_path_to_utf8(filepath, utf8_filepath, sizeof(utf8_filepath)) < 0) {
        write_log("Failed to convert output filepath to UTF-8\n");
        av_frame_free(&audio_frame);
        av_frame_free(&resampled_frame);
        av_packet_free(&enc_pkt);
        avformat_close_input(&ifmt_ctx);
        av_packet_free(&pkt);
        return -1;
    }

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", utf8_filepath);
    if (!ofmt_ctx) {
        write_log("Failed to create output context\n");
        av_frame_free(&audio_frame);
        av_frame_free(&resampled_frame);
        av_packet_free(&enc_pkt);
        avformat_close_input(&ifmt_ctx);
        av_packet_free(&pkt);
        return -1;
    }

    ret = setup_output_stream(ofmt_ctx, ifmt_ctx, audio_dec_ctx, audio_enc_ctx, swr_ctx, audio_fifo, stream_mapping);
    if (ret < 0) {
        write_log("Failed to setup output streams: %d\n", ret);
        avformat_free_context(ofmt_ctx);
        av_frame_free(&audio_frame);
        av_frame_free(&resampled_frame);
        av_packet_free(&enc_pkt);
        free_audio_transcoding(audio_dec_ctx, audio_enc_ctx, swr_ctx, audio_fifo, ifmt_ctx->nb_streams);
        avformat_close_input(&ifmt_ctx);
        av_packet_free(&pkt);
        return ret;
    }

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        write_log("Opening output file: %s\n", filepath);
        ret = avio_open(&ofmt_ctx->pb, utf8_filepath, AVIO_FLAG_WRITE);
        if (ret < 0) {
            char errbuf[128] = {0};
            av_log_error_str(ret, errbuf, sizeof(errbuf));
            write_log("Failed to open output file: %d (%s)\n", ret, errbuf);
            avformat_free_context(ofmt_ctx);
            av_frame_free(&audio_frame);
            av_frame_free(&resampled_frame);
            av_packet_free(&enc_pkt);
            free_audio_transcoding(audio_dec_ctx, audio_enc_ctx, swr_ctx, audio_fifo, ifmt_ctx->nb_streams);
            avformat_close_input(&ifmt_ctx);
            av_packet_free(&pkt);
            return ret;
        }
    }

    AVDictionary* write_opts = NULL;
    //av_dict_set(&write_opts, "movflags", "frag_keyframe+empty_moov", 0);
    ret = avformat_write_header(ofmt_ctx, &write_opts);
    av_dict_free(&write_opts);
    if (ret < 0) {
        write_log("Failed to write file header: %d\n", ret);
        avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
        av_frame_free(&audio_frame);
        av_frame_free(&resampled_frame);
        av_packet_free(&enc_pkt);
        free_audio_transcoding(audio_dec_ctx, audio_enc_ctx, swr_ctx, audio_fifo, ifmt_ctx->nb_streams);
        avformat_close_input(&ifmt_ctx);
        av_packet_free(&pkt);
        return ret;
    }

    while (1) {
        ret = av_read_frame(ifmt_ctx, pkt);
        if (ret < 0) {
            write_log("Failed to read frame or connection interrupted: %d\n", ret);
            break;
        }

        duration = (av_gettime() - start_time) / 1000000.0;
        if (duration >= SEGMENT_DURATION) {
            write_log("Segment created: %s (%.1f seconds)\n", filepath, duration);
            for (unsigned int idx = 0; idx < ifmt_ctx->nb_streams; idx++) {
                if (audio_enc_ctx[idx] && stream_mapping[idx] >= 0) {
                    flush_audio_fifo(ofmt_ctx, audio_fifo[idx], audio_enc_ctx[idx], ofmt_ctx->streams[stream_mapping[idx]], enc_pkt, &audio_pts[idx]);
                    flush_audio_encoder(ofmt_ctx, audio_enc_ctx[idx], enc_pkt, ofmt_ctx->streams[stream_mapping[idx]]);
                    audio_pts[idx] = 0;
                }
            }
            avio_flush(ofmt_ctx->pb);
            av_write_trailer(ofmt_ctx);
            avio_closep(&ofmt_ctx->pb);
            avformat_free_context(ofmt_ctx);

            create_filepath(filepath);
            char new_utf8_filepath[1024] = {0};
            if (win_path_to_utf8(filepath, new_utf8_filepath, sizeof(new_utf8_filepath)) < 0) {
                write_log("Failed to convert segment filepath to UTF-8\n");
                break;
            }
            avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", new_utf8_filepath);
            if (!ofmt_ctx) {
                write_log("Failed to create segment output context\n");
                break;
            }

            ret = setup_output_stream(ofmt_ctx, ifmt_ctx, audio_dec_ctx, audio_enc_ctx, swr_ctx, audio_fifo, stream_mapping);
            if (ret < 0) {
                write_log("Failed to setup segment streams: %d\n", ret);
                avformat_free_context(ofmt_ctx);
                break;
            }

            if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                write_log("Opening segment file: %s\n", filepath);
                ret = avio_open(&ofmt_ctx->pb, new_utf8_filepath, AVIO_FLAG_WRITE);
                if (ret < 0) {
                    char errbuf[128] = {0};
                    av_log_error_str(ret, errbuf, sizeof(errbuf));
                    write_log("Failed to open segment file: %d (%s)\n", ret, errbuf);
                    avformat_free_context(ofmt_ctx);
                    break;
                }
            }

            write_opts = NULL;
            //av_dict_set(&write_opts, "movflags", "frag_keyframe+empty_moov", 0);
            ret = avformat_write_header(ofmt_ctx, &write_opts);
            av_dict_free(&write_opts);
            if (ret < 0) {
                write_log("Failed to write segment header: %d\n", ret);
                avio_closep(&ofmt_ctx->pb);
                avformat_free_context(ofmt_ctx);
                break;
            }
            start_time = av_gettime();
        }

        int in_index = pkt->stream_index;
        if (in_index < 0 || in_index >= MAX_STREAMS) {
            av_packet_unref(pkt);
            continue;
        }

        int out_index = stream_mapping[in_index];
        if (out_index < 0) {
            av_packet_unref(pkt);
            continue;
        }

        AVStream* in_stream = ifmt_ctx->streams[in_index];
        AVStream* out_stream = ofmt_ctx->streams[out_index];

        // 音频直接转发，不转码！永远不报错
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->stream_index = out_index;
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        av_packet_unref(pkt);
        continue;

        pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base,
            AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base,
            AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
        pkt->stream_index = out_index;

        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        if (ret < 0) {
            write_log("Failed to write frame: %d\n", ret);
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
    }

    for (unsigned int idx = 0; idx < ifmt_ctx->nb_streams; idx++) {
        if (audio_enc_ctx[idx] && stream_mapping[idx] >= 0) {
            flush_audio_fifo(ofmt_ctx, audio_fifo[idx], audio_enc_ctx[idx], ofmt_ctx->streams[stream_mapping[idx]], enc_pkt, &audio_pts[idx]);
            flush_audio_encoder(ofmt_ctx, audio_enc_ctx[idx], enc_pkt, ofmt_ctx->streams[stream_mapping[idx]]);
        }
    }

    if (ofmt_ctx) {
        avio_flush(ofmt_ctx->pb);
        av_write_trailer(ofmt_ctx);
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctx->pb);
        }
        avformat_free_context(ofmt_ctx);
    }

    free_audio_transcoding(audio_dec_ctx, audio_enc_ctx, swr_ctx, audio_fifo, ifmt_ctx->nb_streams);
    av_frame_free(&audio_frame);
    av_frame_free(&resampled_frame);
    av_packet_free(&enc_pkt);

    if (ifmt_ctx) {
        avformat_close_input(&ifmt_ctx);
    }

    av_packet_free(&pkt);
    write_log("Recording stopped\n");
    return 0;
}

void main_record() {
    avformat_network_init();
    av_log_set_level(AV_LOG_ERROR);
    write_log("Service started successfully\n");

    while (1) {
        char* url = get_new_rtsp_url();
        start_record(url);
        av_usleep(RECONNECT_DELAY);
    }

    avformat_network_deinit();
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv);
void ServiceCtrlHandler(DWORD ctrlCode);
int InstallService();
int UninstallService();

int main(int argc, char* argv[]) {
    init_service_paths();
    if (argc > 1) {
        if (!strcmp(argv[1], "install")) {
            int result = InstallService();
            if (result == 0) {
                MessageBox(NULL, "Service installed successfully!\nRun: net start RTSPRecorder\nOr start it in Services Manager", "Success", MB_OK);
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "Service installation failed! Error code: %d\nPlease run as administrator", result);
                MessageBox(NULL, msg, "Failed", MB_ICONERROR);
            }
            return result;
        }
        if (!strcmp(argv[1], "uninstall")) {
            int result = UninstallService();
            if (result == 0) {
                MessageBox(NULL, "Service uninstalled successfully!", "Success", MB_OK);
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "Service uninstallation failed! Error code: %d", result);
                MessageBox(NULL, msg, "Failed", MB_ICONERROR);
            }
            return result;
        }
    }

    SERVICE_TABLE_ENTRY svcTable[] = {
        {SERVICE_NAME, ServiceMain},
        {NULL, NULL}
    };

    if (!StartServiceCtrlDispatcher(svcTable)) {
        DWORD err = GetLastError();
        write_log("StartServiceCtrlDispatcher failed: %lu\n", err);
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            write_log("Not running under service control manager; starting in console mode\n");
            main_record();
        }
    }

    return 0;
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv) {
    g_ServiceStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);

    main_record();
}

void ServiceCtrlHandler(DWORD ctrlCode) {
    switch (ctrlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
            ExitProcess(0);
            break;
        default:
            break;
    }
}

int InstallService() {
    SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM) {
        write_log("OpenSCManager failed: %ld\n", GetLastError());
        return GetLastError();
    }

    char path[1024];
    if (!GetModuleFileName(NULL, path, sizeof(path))) {
        write_log("GetModuleFileName failed\n");
        CloseServiceHandle(hSCM);
        return GetLastError();
    }

    write_log("Service executable path: %s\n", path);

    char quoted_path[MAX_PATH * 2] = {0};
    snprintf(quoted_path, sizeof(quoted_path), "\"%s\"", path);

    SC_HANDLE hSvc = CreateService(
        hSCM,
        SERVICE_NAME,
        SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        quoted_path,
        NULL, NULL, NULL, NULL, NULL
    );

    if (!hSvc) {
        DWORD error = GetLastError();
        write_log("CreateService failed: %ld\n", error);
        CloseServiceHandle(hSCM);
        return error;
    }

    // 设置服务描述
    SERVICE_DESCRIPTION sd;
    sd.lpDescription = SERVICE_DESC_TEXT;
    ChangeServiceConfig2(hSvc, SERVICE_CONFIG_DESCRIPTION, &sd);

    write_log("Service %s installed successfully\n", SERVICE_NAME);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return 0;
}

int UninstallService() {
    SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        write_log("OpenSCManager failed: %ld\n", GetLastError());
        return GetLastError();
    }

    SC_HANDLE hSvc = OpenService(hSCM, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!hSvc) {
        DWORD error = GetLastError();
        write_log("OpenService failed: %ld (service may not exist)\n", error);
        CloseServiceHandle(hSCM);
        return error;
    }

    // 先停止服务
    SERVICE_STATUS status;
    ControlService(hSvc, SERVICE_CONTROL_STOP, &status);
    Sleep(1000);

    if (!DeleteService(hSvc)) {
        DWORD error = GetLastError();
        write_log("DeleteService failed: %ld\n", error);
        CloseServiceHandle(hSvc);
        CloseServiceHandle(hSCM);
        return error;
    }

    write_log("Service %s uninstalled successfully\n", SERVICE_NAME);
    CloseServiceHandle(hSvc);
    CloseServiceHandle(hSCM);
    return 0;
}