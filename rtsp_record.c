#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/timeb.h>
#include <direct.h>
#include <windows.h>
#include <winsvc.h>

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>

#define SEGMENT_DURATION 60.0
#define RECONNECT_DELAY 3000000
#define PYTHON_SCRIPT "get_rtsp.py"
#define LOG_FILE "record.log"

#define SERVICE_NAME "RTSPRecorder"
#define SERVICE_DISPLAY_NAME "RTSP Recorder Service"
#define SERVICE_DESC "RTSP Camera Recorder with Auto Split and Python URL Refresh"

FILE* log_fp = NULL;
SERVICE_STATUS serviceStatus;
SERVICE_STATUS_HANDLE serviceStatusHandle;

void write_log(const char* fmt, ...) {
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

char* get_new_rtsp_url() {
    static char rtsp_url[1024] = {0};
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "python %s", PYTHON_SCRIPT);
    FILE* fp = _popen(cmd, "r");
    if (!fp) {
        write_log("Python script error\n");
        return rtsp_url;
    }
    fgets(rtsp_url, 1024, fp);
    size_t len = strlen(rtsp_url);
    if (len > 0 && (rtsp_url[len-1] == '\n' || rtsp_url[len-1] == '\r'))
        rtsp_url[len-1] = 0;
    _pclose(fp);
    write_log("Get RTSP URL: %s\n", rtsp_url);
    return rtsp_url;
}

void create_day_dir(char* dir) {
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    strftime(dir, 256, "%Y%m%d", t);
    CreateDirectory(dir, NULL);
}

void create_filepath(char* path) {
    char dir[256];
    create_day_dir(dir);
    struct _timeb tb;
    _ftime(&tb);
    struct tm* t = localtime(&tb.time);
    int ms5 = tb.millitm * 1000;
    snprintf(path, 512, "%s\\%04d%02d%02d%02d%02d%02d-%04d.mp4",
        dir,
        t->tm_year + 1900,
        t->tm_mon + 1,
        t->tm_mday,
        t->tm_hour,
        t->tm_min,
        t->tm_sec,
        ms5);
}

int start_record(const char* rtsp_url) {
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

    ret = avformat_open_input(&ifmt_ctx, rtsp_url, NULL, &options);
    av_dict_free(&options);
    if (ret < 0) {
        write_log("Open RTSP failed\n");
        return ret;
    }

    avformat_find_stream_info(ifmt_ctx, NULL);
    start_time = av_gettime();
    create_filepath(filepath);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "matroska", filepath);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* in_stream = ifmt_ctx->streams[i];
        AVStream* out_stream = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    }

    ret = avio_open(&ofmt_ctx->pb, filepath, AVIO_FLAG_WRITE);
    ret = avformat_write_header(ofmt_ctx, NULL);

    while (1) {
        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0) break;

        duration = (av_gettime() - start_time) / 1000000.0;
        if (duration >= SEGMENT_DURATION) {
            write_log("Split file: %s\n", filepath);
            av_write_trailer(ofmt_ctx);
            avio_closep(&ofmt_ctx->pb);
            avformat_free_context(ofmt_ctx);

            create_filepath(filepath);
            avformat_alloc_output_context2(&ofmt_ctx, NULL, "matroska", filepath);
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
    write_log("Recording stopped\n");
    return 0;
}

void main_record() {
    avformat_network_init();
    av_log_set_level(AV_LOG_ERROR);
    write_log("Service started\n");

    while (1) {
        char* url = get_new_rtsp_url();
        start_record(url);
        av_usleep(RECONNECT_DELAY);
    }

    avformat_network_deinit();
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv);
void ServiceControl(DWORD dwCtrlCode);
void InstallService();
void UninstallService();

int main(int argc, char* argv[]) {
    if (argc > 1) {
        if (!strcmp(argv[1], "install")) {
            InstallService();
            return 0;
        }
        if (!strcmp(argv[1], "uninstall")) {
            UninstallService();
            return 0;
        }
    }

    SERVICE_TABLE_ENTRY serviceTable[] = {
        {SERVICE_NAME, ServiceMain},
        {NULL, NULL}
    };
    StartServiceCtrlDispatcher(serviceTable);
    main_record();
    return 0;
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv) {
    serviceStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceControl);
    serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState = SERVICE_START_PENDING;
    serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    serviceStatus.dwWin32ExitCode = 0;
    serviceStatus.dwServiceSpecificExitCode = 0;
    serviceStatus.dwCheckPoint = 0;
    serviceStatus.dwWaitHint = 0;
    SetServiceStatus(serviceStatusHandle, &serviceStatus);

    serviceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(serviceStatusHandle, &serviceStatus);

    main_record();
}

void ServiceControl(DWORD dwCtrlCode) {
    switch (dwCtrlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            serviceStatus.dwCurrentState = SERVICE_STOPPED;
            SetServiceStatus(serviceStatusHandle, &serviceStatus);
            ExitProcess(0);
            break;
        default:
            break;
    }
}

void InstallService() {
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCManager) return;

    char path[1024];
    GetModuleFileName(NULL, path, 1024);

    SC_HANDLE hService = CreateService(
        hSCManager, SERVICE_NAME, SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        path, NULL, NULL, NULL, NULL, NULL
    );

    if (hService) {
        write_log("Service installed\n");
        CloseServiceHandle(hService);
    }
    CloseServiceHandle(hSCManager);
}

void UninstallService() {
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) return;
    SC_HANDLE hService = OpenService(hSCManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (hService) {
        DeleteService(hService);
        CloseServiceHandle(hService);
        write_log("Service uninstalled\n");
    }
    CloseServiceHandle(hSCManager);
}
