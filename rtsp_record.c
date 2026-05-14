#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/timeb.h>
#include <direct.h>
#include <windows.h>
#include <winsvc.h>
#include <stdarg.h>
#include <signal.h>

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

static AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;

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
    fprintf(log_fp, "[%04d-%02d-%02d %02d-%02d-%02d] ",
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
    write_log("初始化服务路径: %s\n", g_base_dir);
    write_log("log 文件路径: %s\n", g_log_file);
    write_log("Python 脚本命令: %s\n", g_python_script);
}

char* get_new_rtsp_url() {
    static char rtsp_url[1024] = {0};
    char cmd[1024];
    const char* python_path = g_python_script[0] ? g_python_script : PYTHON_SCRIPT;
    write_log("Calling Python script Path: %s\n", python_path);

    rtsp_url[0] = '\0';
    snprintf(cmd, sizeof(cmd), "python \"%s\" 2>&1", python_path);
    FILE* fp = _popen(cmd, "r");
    if (!fp) {
        write_log("Failed to call Python script with python command\n");
    } else {
        if (fgets(rtsp_url, sizeof(rtsp_url), fp)) {
            size_t len = strlen(rtsp_url);
            while (len > 0 && (rtsp_url[len-1] == '\n' || rtsp_url[len-1] == '\r')) {
                rtsp_url[--len] = 0;
            }
        }
        _pclose(fp);
    }

    if (!rtsp_url[0]) {
        write_log("python command returned empty result, trying py launcher\n");
        rtsp_url[0] = '\0';
        snprintf(cmd, sizeof(cmd), "py -3 \"%s\" 2>&1", python_path);
        fp = _popen(cmd, "r");
        if (!fp) {
            write_log("Failed to call Python script with py launcher\n");
        } else {
            if (fgets(rtsp_url, sizeof(rtsp_url), fp)) {
                size_t len = strlen(rtsp_url);
                while (len > 0 && (rtsp_url[len-1] == '\n' || rtsp_url[len-1] == '\r')) {
                    rtsp_url[--len] = 0;
                }
            }
            _pclose(fp);
        }
    }

    write_log("Got RTSP URL: %s\n", rtsp_url[0] ? rtsp_url : "<empty>");
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
    snprintf(path, 512, "%s\\%04d%02d%02d%02d%02d%02d-%5d.%s",
        dir,
        t->tm_year + 1900,
        t->tm_mon + 1,
        t->tm_mday,
        t->tm_hour,
        t->tm_min,
        t->tm_sec,
        60000,
        OUTPUT_EXTENSION);
    write_log("Created output filepath: %s\n", path);
}

int start_record(const char* rtsp_url) {
    //AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket pkt;
    int ret, i;
    int64_t start_time = 0;
    double duration = 0;
    char filepath[512];
    char utf8_filepath[1024] = {0};

    write_log("开始录制: %s\n", rtsp_url);

    // ------------- 输入参数（正确）-------------
    AVDictionary* options = NULL;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);
    av_dict_set(&options, "stimeout", "5000000", 0);
    av_dict_set(&options, "fflags", "nobuffer+discardcorrupt", 0);

    ret = avformat_open_input(&ifmt_ctx, rtsp_url, NULL, &options);
    av_dict_free(&options);
    if (ret < 0) {
        write_log("打开RTSP失败\n");
        return ret;
    }

    avformat_find_stream_info(ifmt_ctx, NULL);
    start_time = av_gettime();
    create_filepath(filepath);

    if (win_path_to_utf8(filepath, utf8_filepath, sizeof(utf8_filepath)) < 0) {
        write_log("路径转换失败\n");
        return -1;
    }

    avformat_alloc_output_context2(&ofmt_ctx, NULL, "mov", utf8_filepath);

    // ------------- 复制流（不手动修改时间戳）-------------
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* in_stream = ifmt_ctx->streams[i];
        AVCodecParameters* par = in_stream->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;

        AVStream* out_stream = avformat_new_stream(ofmt_ctx, NULL);
        avcodec_parameters_copy(out_stream->codecpar, par);
        out_stream->time_base = in_stream->time_base;
        out_stream->codecpar->codec_tag = 0;
    }

    ofmt_ctx->max_delay = 500000; // 0.5秒队列，彻底解决队列堆积
    ofmt_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    ret = avio_open(&ofmt_ctx->pb, utf8_filepath, AVIO_FLAG_WRITE);
    if (ret < 0) {
        write_log("打开输出文件失败\n");
        return ret;
    }

    // ------------- 输出关键参数（解决时间戳错乱）-------------
    AVDictionary* out_opts = NULL;
    av_dict_set(&out_opts, "reset_timestamps", "1", 0);
    av_dict_set(&out_opts, "fflags", "genpts", 0);
    av_dict_set(&out_opts, "max_interleave_delta", "5000000", 0); // 5秒即可
    avformat_write_header(ofmt_ctx, &out_opts);
    av_dict_free(&out_opts);

    // 每个分段独立记录第一帧时间戳，解决 start 值问题
    int64_t base_pts[16] = { 0 };
    int64_t base_dts[16] = { 0 };
    for (int k = 0; k < 16; k++) {
        base_pts[k] = AV_NOPTS_VALUE;
        base_dts[k] = AV_NOPTS_VALUE;
    }

    // ======================== 核心循环 ========================
    while (1) {
        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0){
            char err_buf[1024];
            // 把错误码转成文字
            av_strerror(ret, err_buf, sizeof(err_buf));
            printf("av_read_frame failed: %s, error code: %d\n", err_buf, ret);
            break;
        }

        duration = (av_gettime() - start_time) / 1000000.0;

        // ======================== 分段逻辑 ========================
        if (duration >= SEGMENT_DURATION) {
            write_log("分段完成: %s\n", filepath);

            // 关闭旧文件
            av_write_trailer(ofmt_ctx);
            avio_closep(&ofmt_ctx->pb);
            avformat_free_context(ofmt_ctx);
            ofmt_ctx = NULL;

            // 新文件
            create_filepath(filepath);
            win_path_to_utf8(filepath, utf8_filepath, sizeof(utf8_filepath));

            avformat_alloc_output_context2(&ofmt_ctx, NULL, "mov", utf8_filepath);

            // 重新创建流
            for (i = 0; i < ifmt_ctx->nb_streams; i++) {
                AVStream* in_stream = ifmt_ctx->streams[i];
                AVCodecParameters* par = in_stream->codecpar;
                if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO)
                    continue;

                AVStream* out_stream = avformat_new_stream(ofmt_ctx, NULL);
                avcodec_parameters_copy(out_stream->codecpar, par);
                out_stream->time_base = in_stream->time_base;
                out_stream->codecpar->codec_tag = 0;
            }

            ofmt_ctx->max_delay = 500000;
            ofmt_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
            avio_open(&ofmt_ctx->pb, utf8_filepath, AVIO_FLAG_WRITE);

            // 新文件必须重置时间戳
            AVDictionary* new_out_opts = NULL;
            av_dict_set(&new_out_opts, "reset_timestamps", "1", 0);
            av_dict_set(&new_out_opts, "fflags", "genpts", 0);
            av_dict_set(&new_out_opts, "max_interleave_delta", "5000000", 0);
            avformat_write_header(ofmt_ctx, &new_out_opts);
            av_dict_free(&new_out_opts);

            start_time = av_gettime();


            for (int k = 0; k < 16; k++) {
                base_pts[k] = AV_NOPTS_VALUE;
                base_dts[k] = AV_NOPTS_VALUE;
            }
        }

     

        // ======================== 【关键】完全不手动改PTS/DTS！ ========================
        AVStream* in_stream = ifmt_ctx->streams[pkt.stream_index];
        AVStream* out_stream = ofmt_ctx->streams[pkt.stream_index];


        // 只做时间基转换，不修改任何值！！！
        av_packet_rescale_ts(&pkt, in_stream->time_base, out_stream->time_base);
        // ===================== 核心：去掉 start 值，每个分段从 0 开始 =====================
        if (base_pts[pkt.stream_index] == AV_NOPTS_VALUE) {
            base_pts[pkt.stream_index] = pkt.pts;
            base_dts[pkt.stream_index] = pkt.dts;
        }

        // 减去起始值，强制从0开始
        pkt.pts -= base_pts[pkt.stream_index];
        pkt.dts -= base_dts[pkt.stream_index];

        if (pkt.pts < 0) pkt.pts = 0;
        if (pkt.dts < 0) pkt.dts = 0;

        pkt.pos = -1;


        // ===================== 调试输出：所有关键时间 =====================
        
        // write_log("[调试] ====== 时间信息 ======\n");
        // write_log("[调试] ofmt_ctx->start_time = %lld\n", ofmt_ctx->start_time);
        // write_log("[调试] ofmt_ctx->duration   = %lld\n", ofmt_ctx->duration);
        // write_log("[调试] stream_index        = %d\n", pkt.stream_index);
        // write_log("[调试] out_stream->start_time = %lld\n", out_stream->start_time);
        // write_log("[调试] out_stream->duration   = %lld\n", out_stream->duration);
        // write_log("[调试] pkt.pts = %lld | pkt.dts = %lld\n", pkt.pts, pkt.dts);
        // write_log("[调试] ======================\n");
        // // ===================================================================


        // 直接写入，不做任何减法、归零、偏移
        av_interleaved_write_frame(ofmt_ctx, &pkt);
        av_packet_unref(&pkt);
    }

    // 收尾
    av_write_trailer(ofmt_ctx);
    avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    avformat_close_input(&ifmt_ctx);
    write_log("录制停止\n");
    return 0;
}

void main_record() {

    // 日志输出级别控制函数
    // AV_LOG_QUIET        // 完全安静，**不输出任何日志**（正式版用）
    // AV_LOG_ERROR        // 只显示错误
    // AV_LOG_WARNING      // 显示警告 + 错误
    // AV_LOG_INFO         // 显示普通信息（默认）
    // AV_LOG_DEBUG        // 调试用，超多日志
    av_log_set_level(AV_LOG_DEBUG);

    avformat_network_init();
    write_log("Service started\n");

    while (1) {
        char* url = get_new_rtsp_url();
        if (!url || !url[0]) {
            // write_log("No RTSP URL received, waiting and retrying\n");
            write_log("未获取到RTSP地址,等待3秒后重试...\n");
            av_usleep(RECONNECT_DELAY);
            continue;
        }
        start_record(url);
        av_usleep(RECONNECT_DELAY);
    }

    avformat_network_deinit();
}

// 信号处理：Ctrl+C / 终止信号
void signal_handler(int sig) {
    printf("\nReceived exit signal, shutting down safely...\n");
    g_exit = 1;

    sleep(1);

    if (ofmt_ctx != NULL) {
        av_write_trailer(ofmt_ctx);
        printf("Trailer written successfully, file is complete!\n");
    }

    avio_closep(&ofmt_ctx->pb);
    if (ifmt_ctx) avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx) avformat_free_context(ofmt_ctx);
    avformat_network_deinit();

    printf("Program exited safely\n");
    exit(0);
}

// 注册所有退出信号
void register_signal() {
    signal(SIGINT,  signal_handler);   // Ctrl+C
    signal(SIGTERM, signal_handler);   // kill
    signal(SIGABRT, signal_handler);   // 异常
    signal(SIGSEGV, signal_handler);   // 段错误
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv);
void ServiceCtrlHandler(DWORD ctrlCode);
int InstallService();
int UninstallService();

int main(int argc, char* argv[]) {
    init_service_paths();
    if (argc > 1) {
        if (!strcmp(argv[1], "install")) return InstallService();
        if (!strcmp(argv[1], "uninstall")) return UninstallService();
    }

    SERVICE_TABLE_ENTRY svcTable[] = {
        {SERVICE_NAME, ServiceMain},
        {NULL, NULL}
    };

    if (!StartServiceCtrlDispatcher(svcTable)) {
        main_record();
    }
    return 0;
}

void WINAPI ServiceMain(DWORD argc, LPSTR* argv) {
    g_ServiceStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
    main_record();
}

void ServiceCtrlHandler(DWORD ctrlCode) {
    if (ctrlCode == SERVICE_CONTROL_STOP || ctrlCode == SERVICE_CONTROL_SHUTDOWN) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
        ExitProcess(0);
    }
}

int InstallService() {
    SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!hSCM) return GetLastError();

    char path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    char quoted[MAX_PATH * 2];
    snprintf(quoted, sizeof(quoted), "\"%s\"", path);

    SC_HANDLE hSvc = CreateService(
        hSCM, SERVICE_NAME, SERVICE_DISPLAY_NAME, SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        quoted, NULL, NULL, NULL, NULL, NULL);

    if (hSvc) {
        SERVICE_DESCRIPTION sd = {SERVICE_DESC_TEXT};
        ChangeServiceConfig2(hSvc, SERVICE_CONFIG_DESCRIPTION, &sd);
        CloseServiceHandle(hSvc);
    }
    DWORD err = GetLastError();
    CloseServiceHandle(hSCM);
    return err == 0 ? 0 : err;
}

int UninstallService() {
    SC_HANDLE hSCM = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return GetLastError();

    SC_HANDLE hSvc = OpenService(hSCM, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (hSvc) {
        SERVICE_STATUS st;
        ControlService(hSvc, SERVICE_CONTROL_STOP, &st);
        Sleep(1000);
        DeleteService(hSvc);
        CloseServiceHandle(hSvc);
    }
    DWORD err = GetLastError();
    CloseServiceHandle(hSCM);
    return err == 0 ? 0 : err;
}