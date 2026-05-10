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
#include <libavutil/avutil.h>
#include <libavutil/time.h>

#define SEGMENT_DURATION 60.0
#define RECONNECT_DELAY 3000000
#define PYTHON_SCRIPT "get_rtsp.py"
#define LOG_FILE "record.log"

#define SERVICE_NAME "RTSPRecorder"
#define SERVICE_DISPLAY_NAME "RTSP Recorder Service"
#define SERVICE_DESC_TEXT "RTSP Camera Recorder Service"

FILE* log_fp = NULL;
SERVICE_STATUS g_ServiceStatus;
SERVICE_STATUS_HANDLE g_ServiceStatusHandle;

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
    snprintf(path, 512, "%s\\%04d%02d%02d%02d%02d%02d-%05d.mp4",
        dir,
        t->tm_year + 1900,
        t->tm_mon + 1,
        t->tm_mday,
        t->tm_hour,
        t->tm_min,
        t->tm_sec,
        ms5);
}

void setup_output_stream(AVFormatContext* ofmt_ctx, AVFormatContext* ifmt_ctx) {
    for (unsigned int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* in_stream = ifmt_ctx->streams[i];
        AVStream* out_stream = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        out_stream->codecpar->codec_tag = 0;
        out_stream->time_base = in_stream->time_base;
        out_stream->r_frame_rate = in_stream->r_frame_rate;
        out_stream->avg_frame_rate = in_stream->avg_frame_rate;
    }
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

    if (avformat_find_stream_info(ifmt_ctx, NULL) < 0) {
        write_log("Failed to find stream info\n");
        avformat_close_input(&ifmt_ctx);
        av_packet_free(&pkt);
        return -1;
    }

    start_time = av_gettime();
    create_filepath(filepath);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", filepath);
    if (!ofmt_ctx) {
        write_log("Failed to create output context\n");
        avformat_close_input(&ifmt_ctx);
        av_packet_free(&pkt);
        return -1;
    }

    setup_output_stream(ofmt_ctx, ifmt_ctx);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filepath, AVIO_FLAG_WRITE);
        if (ret < 0) {
            write_log("Failed to open output file: %d\n", ret);
            avformat_free_context(ofmt_ctx);
            avformat_close_input(&ifmt_ctx);
            av_packet_free(&pkt);
            return ret;
        }
    }

    AVDictionary* write_opts = NULL;
    av_dict_set(&write_opts, "movflags", "frag_keyframe+empty_moov", 0);
    ret = avformat_write_header(ofmt_ctx, &write_opts);
    av_dict_free(&write_opts);
    if (ret < 0) {
        write_log("Failed to write file header: %d\n", ret);
        avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
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
            
            avio_flush(ofmt_ctx->pb);
            av_write_trailer(ofmt_ctx);
            avio_closep(&ofmt_ctx->pb);
            avformat_free_context(ofmt_ctx);

            create_filepath(filepath);
            avformat_alloc_output_context2(&ofmt_ctx, NULL, "mp4", filepath);
            if (!ofmt_ctx) {
                write_log("Failed to create segment output context\n");
                break;
            }

            setup_output_stream(ofmt_ctx, ifmt_ctx);

            if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
                ret = avio_open(&ofmt_ctx->pb, filepath, AVIO_FLAG_WRITE);
                if (ret < 0) {
                    write_log("Failed to open segment file: %d\n", ret);
                    avformat_free_context(ofmt_ctx);
                    break;
                }
            }

            write_opts = NULL;
            av_dict_set(&write_opts, "movflags", "frag_keyframe+empty_moov", 0);
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

        pkt->stream_index = pkt->stream_index;
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        if (ret < 0) {
            write_log("Failed to write frame: %d\n", ret);
        }
        av_packet_unref(pkt);
    }

    if (ofmt_ctx) {
        avio_flush(ofmt_ctx->pb);
        av_write_trailer(ofmt_ctx);
        if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctx->pb);
        }
        avformat_free_context(ofmt_ctx);
    }

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
    StartServiceCtrlDispatcher(svcTable);
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

    SC_HANDLE hSvc = CreateService(
        hSCM,
        SERVICE_NAME,
        SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
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