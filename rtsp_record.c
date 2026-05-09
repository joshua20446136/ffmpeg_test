#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/timeb.h>
#include <direct.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>

#define SEGMENT_DURATION 60.0
#define RECONNECT_DELAY 3
#define PYTHON_SCRIPT "get_rtsp.py"

char* get_new_rtsp_url() {
    static char rtsp_url[1024] = { 0 };
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "python %s", PYTHON_SCRIPT);
    FILE* fp = _popen(cmd, "r");
    if (!fp) return rtsp_url;

    if (fgets(rtsp_url, sizeof(rtsp_url), fp)) {
        size_t len = strlen(rtsp_url);
        if (len > 0 && (rtsp_url[len-1] == '\n' || rtsp_url[len-1] == '\r'))
            rtsp_url[len-1] = 0;
    }
    _pclose(fp);
    return rtsp_url;
}

void create_day_dir(char* dir) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    strftime(dir, 256, "%Y%m%d", t);
    _mkdir(dir);
}

void create_filepath(char* path) {
    char dir[256];
    create_day_dir(dir);
    struct _timeb tb;
    _ftime(&tb);
    struct tm* t = localtime(&tb.time);
    snprintf(path, 512, "%s\\%04d%02d%02d%02d%02d%02d-%03d.mp4",
        dir,
        t->tm_year + 1900,
        t->tm_mon + 1,
        t->tm_mday,
        t->tm_hour,
        t->tm_min,
        t->tm_sec,
        tb.millitm);
}

int start_record(const char* rtsp_url) {
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket pkt;
    int ret, i;
    int64_t start_time = 0;
    double duration = 0;
    char filepath[512];

    AVDictionary* options = NULL;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);

    ret = avformat_open_input(&ifmt_ctx, rtsp_url, NULL, &options);
    av_dict_free(&options);
    if (ret < 0) return ret;

    avformat_find_stream_info(ifmt_ctx, NULL);
    start_time = av_gettime();
    create_filepath(filepath);
    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", filepath);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* in_stream = ifmt_ctx->streams[i];
        AVStream* out_stream = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    }

    avio_open(&ofmt_ctx->pb, filepath, AVIO_FLAG_WRITE);
    avformat_write_header(ofmt_ctx, NULL);

    while (1) {
        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0) break;

        duration = (av_gettime() - start_time) / 1000000.0;
        if (duration >= SEGMENT_DURATION) {
            av_write_trailer(ofmt_ctx);
            avio_closep(&ofmt_ctx->pb);
            avformat_free_context(ofmt_ctx);
            create_filepath(filepath);
            avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", filepath);
            for (i = 0; i < ifmt_ctx->nb_streams; i++) {
                AVStream* in_stream = ifmt_ctx->streams[i];
                AVStream* out_stream = avformat_new_stream(ofmt_ctx, NULL);
                avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            }
            avio_open(&ofmt_ctx->pb, filepath, AVIO_FLAG_WRITE);
            avformat_write_header(ofmt_ctx, NULL);
            start_time = av_gettime();
        }
        av_interleaved_write_frame(ofmt_ctx, &pkt);
        av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt_ctx);
    avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    avformat_close_input(&ifmt_ctx);
    return 0;
}

int main() {
    avformat_network_init();

    while (1) {
        char* url = get_new_rtsp_url();
        start_record(url);
        av_usleep(RECONNECT_DELAY * 1000000);
    }

    avformat_network_deinit();
    return 0;
}
