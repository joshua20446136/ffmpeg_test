#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/timeb.h>
#include <direct.h>
#include <windows.h>
#include <winsvc.h>
#include <process.h>

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavcodec/avcodec.h>

#define SEGMENT_DURATION 60.0
#define RECONNECT_DELAY 3000000
#define PYTHON_SCRIPT "get_rtsp.py"
#define LOG_FILE "record.log"

#define SERVICE_NAME "RTSPRecorder"
#define SERVICE_DISPLAY_NAME "RTSP Recorder Service"
#define SERVICE_DESC  "RTSP Camera Recorder with Auto Split and Python URL Refresh"

FILE* log_fp = NULL;
SERVICE_STATUS serviceStatus;
SERVICE_STATUS_HANDLE serviceStatusHandle;
HANDLE g_hStopEvent = NULL;

void write_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    log_fp = fopen(LOG_FILE, "a+");
    if (!log_fp) return;
    fprintf(log_fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
        t->tm_hour, t->tm_min, t->tm_sec);
    vfprintf(log_fp, fmt, ap);
    fflush(log_fp);
    fclose(log_fp);
    va_end(ap);
}

void print_av_error(const char* func, int ret)
{
    char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(ret, err_buf, sizeof(err_buf));
    write_log("%s error: %d (%s)\n", func, ret, err_buf);
}

char* get_new_rtsp_url()
{
    static char rtsp_url[1024] = {0};
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "python %s", PYTHON_SCRIPT);
    FILE* fp = _popen(cmd, "r");
    if (!fp)
    {
        write_log("Python script popen failed\n");
        return rtsp_url;
    }
    fgets(rtsp_url, 1024, fp);
    size_t len = strlen(rtsp_url);
    while (len > 0 && (rtsp_url[len-1] == '\n' || rtsp_url[len-1] == '\r'))
    {
        rtsp_url[len-1] = 0;
        len--;
    }
    _pclose(fp);
    write_log("Get RTSP URL: %s\n", rtsp_url);
    return rtsp_url;
}

void create_day_dir(char* dir)
{
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    strftime(dir, 256, "%Y%m%d", t);
    CreateDirectoryA(dir, NULL);
}

void create_filepath(char* path)
{
    char dir[256];
    create_day_dir(dir);
    struct _timeb tb;
    _ftime(&tb);
    struct tm* t = localtime(&tb.time);
    snprintf(path, 512, "%s\\%04d%02d%02d_%02d%02d%02d.mp4",
        dir,
        t->tm_year + 1900,
        t->tm_mon + 1,
        t->tm_mday,
        t->tm_hour,
        t->tm_min,
        t->tm_sec);
}

static int init_output_context(AVFormatContext** ofmt_ctx, AVFormatContext* ifmt_ctx, const char* filepath)
{
    int ret, i;
    *ofmt_ctx = NULL;
    ret = avformat_alloc_output_context2(ofmt_ctx, NULL, "mp4", filepath);
    if (ret < 0 || !*ofmt_ctx)
    {
        print_av_error("avformat_alloc_output_context2", ret);
        return ret;
    }

    for (i = 0; i < ifmt_ctx->nb_streams; i++)
    {
        AVStream* in_stream = ifmt_ctx->streams[i];
        AVStream* out_stream = avformat_new_stream(*ofmt_ctx, NULL);
        if (!out_stream) return AVERROR_UNKNOWN;

        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0) { print_av_error("avcodec_parameters_copy", ret); return ret; }
        out_stream->time_base = in_stream->time_base;
        out_stream->avg_frame_rate = in_stream->avg_frame_rate;
    }

    ret = avio_open(&(*ofmt_ctx)->pb, filepath, AVIO_FLAG_WRITE);
    if (ret < 0) { print_av_error("avio_open", ret); return ret; }

    ret = avformat_write_header(*ofmt_ctx, NULL);
    if (ret < 0) { print_av_error("avformat_write_header", ret); return ret; }

    write_log("New file: %s\n", filepath);
    return 0;
}

int start_record(const char* rtsp_url)
{
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket pkt;
    int ret, i;
    int64_t start_time = 0;
    double duration = 0;
    char filepath[512];

    write_log("Start recording: %s\n", rtsp_url);

    AVDictionary* options = NULL;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);
    av_dict_set(&options, "max_delay", "500000", 0);

    ret = avformat_open_input(&ifmt_ctx, rtsp_url, NULL, &options);
    av_dict_free(&options);
    if (ret < 0) { print_av_error("avformat_open_input", ret); return ret; }

    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret < 0) { print_av_error("avformat_find_stream_info", ret); avformat_close_input(&ifmt_ctx); return ret; }

    for (i = 0; i < ifmt_ctx->nb_streams; i++)
    {
        AVStream* s = ifmt_ctx->streams[i];
        write_log("Stream %d: type=%s, codec=%s\n",
            i, av_get_media_type_string(s->codecpar->codec_type), avcodec_get_name(s->codecpar->codec_id));
    }

    start_time = av_gettime();
    create_filepath(filepath);
    ret = init_output_context(&ofmt_ctx, ifmt_ctx, filepath);
    if (ret < 0) { avformat_close_input(&ifmt_ctx); return ret; }

    while (1)
    {
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;

        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0) { print_av_error("av_read_frame", ret); break; }

        AVStream* in_stream = ifmt_ctx->streams[pkt.stream_index];
        AVStream* out_stream = ofmt_ctx->streams[pkt.stream_index];

        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        pkt.pos = -1;

        duration = (av_gettime() - start_time) / 1000000.0;
        if (duration >= SEGMENT_DURATION)
        {
            write_log("Split segment, duration: %.1fs\n", duration);
            av_write_trailer(ofmt_ctx);
            avio_closep(&ofmt_ctx->pb);
            avformat_free_context(ofmt_ctx);

            create_filepath(filepath);
            ret = init_output_context(&ofmt_ctx, ifmt_ctx, filepath);
            if (ret < 0) { av_packet_unref(&pkt); break; }
            start_time = av_gettime();
        }

        ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
        if (ret < 0) print_av_error("av_interleaved_write_frame", ret);

        av_packet_unref(&pkt);
    }

    if (ofmt_ctx)
    {
        av_write_trailer(ofmt_ctx);
        avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
    }
    avformat_close_input(&ifmt_ctx);
    write_log("Recording stopped\n");
    return 0;
}

void main_record()
{
    avformat_network_init();
    av_log_set_level(AV_LOG_ERROR);
    write_log("Service started\n");

    while (1)
    {
        if (WaitForSingleObject(g_hStopEvent, 0) == WAIT_OBJECT_0) break;

        char* url = get_new_rtsp_url();
        if (strlen(url) > 0)
            start_record(url);
        else
            write_log("Empty RTSP URL, retry...\n");

        av_usleep(RECONNECT_DELAY);
    }

    avformat_network_deinit();
    write_log("Service stopped normally\n");
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv);
void ServiceControl(DWORD dwCtrlCode);
void InstallService();
void UninstallService();

int main(int argc, char* argv[])
{
    // 必须管理员运行！
    if (argc > 1)
    {
        if (!strcmp(argv[1], "install"))
        {
            InstallService();
            return 0;
        }
        if (!strcmp(argv[1], "uninstall"))
        {
            UninstallService();
            return 0;
        }
    }

    g_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    SERVICE_TABLE_ENTRY serviceTable[] =
    {
        {SERVICE_NAME, ServiceMain},
        {NULL, NULL}
    };
    if (!StartServiceCtrlDispatcher(serviceTable))
    {
        // 非服务模式，直接运行
        write_log("Run in console mode\n");
        main_record();
    }
    return 0;
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv)
{
    serviceStatusHandle = RegisterServiceCtrlHandlerA(SERVICE_NAME, ServiceControl);
    ZeroMemory(&serviceStatus, sizeof(serviceStatus));

    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    serviceStatus.dwWin32ExitCode = NO_ERROR;
    serviceStatus.dwCheckPoint = 1;
    SetServiceStatus(serviceStatusHandle, &serviceStatus);

    serviceStatus.dwCurrentState = SERVICE_RUNNING;
    serviceStatus.dwCheckPoint = 0;
    SetServiceStatus(serviceStatusHandle, &serviceStatus);

    main_record();

    serviceStatus.dwCurrentState = SERVICE_STOPPED;
    serviceStatus.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus(serviceStatusHandle, &serviceStatus);
}

void ServiceControl(DWORD dwCtrlCode)
{
    switch (dwCtrlCode)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(serviceStatusHandle, &serviceStatus);
        SetEvent(g_hStopEvent);
        Sleep(1000);
        serviceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(serviceStatusHandle, &serviceStatus);
        break;
    default: break;
    }
}

void InstallService()
{
    SC_HANDLE hSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCManager)
    {
        write_log("OpenSCManager failed: %lu\n", GetLastError());
        return;
    }

    char path[1024];
    GetModuleFileNameA(NULL, path, 1024);

    SC_HANDLE hService = CreateServiceA(
        hSCManager,
        SERVICE_NAME,
        SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        NULL, NULL, NULL, NULL, SERVICE_DESC
    );

    if (hService)
    {
        write_log("Service installed OK\n");
        CloseServiceHandle(hService);
    }
    else
    {
        write_log("CreateService failed: %lu\n", GetLastError());
    }
    CloseServiceHandle(hSCManager);
}

void UninstallService()
{
    SC_HANDLE hSCManager = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) return;

    SC_HANDLE hService = OpenServiceA(hSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (hService)
    {
        ControlService(hService, SERVICE_CONTROL_STOP, NULL);
        Sleep(500);
        if (DeleteService(hService))
            write_log("Service uninstalled OK\n");
        else
            write_log("DeleteService failed: %lu\n", GetLastError());
        CloseServiceHandle(hService);
    }
    CloseServiceHandle(hSCManager);
}
