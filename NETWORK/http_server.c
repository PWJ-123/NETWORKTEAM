#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define WEB_ROOT "C:\\GIMALWORKSPACE"
#define MAX_REQ_SIZE 4096

// STOP 기능용 전역 변수 (이 파일 안에서만 사용)
static volatile int g_serverStopRequested = 0;
static SOCKET       g_serverSocket = INVALID_SOCKET;

typedef struct {
    char method[10];
    char uri[256];
    char query_string[256];
    char* body;
} HttpRequest;

typedef struct {
    int status_code;
    const char* status_text;
    const char* mime_type;
    char* content;
    long content_size;
} HttpResponse;

typedef struct {
    SOCKET client_sock;
    char client_ip[32];
} ThreadArgs;

// --- 로그 콜백 포인터 ---
static HttpLogCallback g_log_cb = NULL;

void http_set_log_callback(HttpLogCallback cb) {
    g_log_cb = cb;
}

static void log_printf(const char* fmt, ...) {
    char buf[600];

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    buf[sizeof(buf) - 1] = '\0';

    if (g_log_cb)
        g_log_cb(buf);
    else
        printf("%s", buf);
}

// ---------- 서버 중지 요청 (GUI에서 호출) ----------
void http_server_request_stop(void)
{
    g_serverStopRequested = 1;

    // accept()를 깨우기 위해 리스닝 소켓 닫기
    if (g_serverSocket != INVALID_SOCKET) {
        closesocket(g_serverSocket);
        g_serverSocket = INVALID_SOCKET;
    }
}

// ---------- 파일 읽기 ----------
char* read_file(const char* path, long* out_size) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }

    long size = ftell(fp);
    if (size <= 0) {
        fclose(fp);
        return NULL;
    }

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    char* buffer = (char*)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, (size_t)size, fp);
    fclose(fp);

    buffer[read_size] = '\0';
    if (out_size) *out_size = (long)read_size;
    return buffer;
}

// ---------- MIME ----------
const char* get_mime_type(const char* file) {
    const char* dot = strrchr(file, '.');
    if (!dot) return "text/plain";

    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif"))  return "image/gif";

    return "text/plain";
}

// ----------- 요청 파싱 ----------
void parse_request(const char* raw, HttpRequest* req) {
    if (!raw || !req) return;

    memset(req, 0, sizeof(*req));

    const char* method_end = strchr(raw, ' ');
    if (!method_end) return;

    size_t mlen = (size_t)(method_end - raw);
    if (mlen >= sizeof(req->method)) mlen = sizeof(req->method) - 1;
    memcpy(req->method, raw, mlen);
    req->method[mlen] = '\0';

    const char* uri_start = method_end + 1;
    const char* http_ver = strchr(uri_start, ' ');
    if (!http_ver) return;

    const char* qs = strchr(uri_start, '?');
    const char* uri_end = (qs && qs < http_ver) ? qs : http_ver;

    size_t ulen = (size_t)(uri_end - uri_start);
    if (ulen >= sizeof(req->uri)) ulen = sizeof(req->uri) - 1;
    memcpy(req->uri, uri_start, ulen);
    req->uri[ulen] = '\0';

    if (qs && qs < http_ver) {
        size_t qlen = (size_t)(http_ver - (qs + 1));
        if (qlen >= sizeof(req->query_string)) qlen = sizeof(req->query_string) - 1;
        memcpy(req->query_string, qs + 1, qlen);
        req->query_string[qlen] = '\0';
    }

    if (!strcmp(req->method, "POST")) {
        const char* body = strstr(raw, "\r\n\r\n");
        if (body) req->body = (char*)(body + 4);
    }
}

// ----------- 요청 처리 ----------
void handle_request(const HttpRequest* req, HttpResponse* res) {
    char fullpath[512];

    if (strcmp(req->uri, "/") == 0)
        sprintf(fullpath, "%s\\index.html", WEB_ROOT);
    else
        sprintf(fullpath, "%s%s", WEB_ROOT, req->uri);

    for (char* p = fullpath; *p; p++)
        if (*p == '/') *p = '\\';

    long size = 0;
    char* buf = read_file(fullpath, &size);

    if (buf) {
        res->status_code = 200;
        res->status_text = "OK";
        res->mime_type = get_mime_type(fullpath);
        res->content = buf;
        res->content_size = size;
    }
    else {
        res->status_code = 404;
        res->status_text = "Not Found";
        res->mime_type = "text/html";
        res->content = "<h1>404 Not Found</h1>";
        res->content_size = (long)strlen(res->content);
    }
}

// ----------- 클라이언트 스레드 ----------
unsigned __stdcall handle_client(void* ptr) {
    ThreadArgs* args = (ThreadArgs*)ptr;
    if (!args) {
        _endthreadex(0);
        return 0;
    }

    SOCKET client = args->client_sock;
    char ip[32];
    strcpy(ip, args->client_ip);
    free(args);

    log_printf("[CLIENT] %s 연결\n", ip);

    char buf[MAX_REQ_SIZE];
    int len = recv(client, buf, MAX_REQ_SIZE - 1, 0);

    if (len <= 0) {
        closesocket(client);
        _endthreadex(0);
        return 0;
    }
    buf[len] = '\0';

    HttpRequest req;
    parse_request(buf, &req);

    log_printf("[REQUEST] %s %s ? %s\n",
        req.method,
        req.uri,
        req.query_string[0] ? req.query_string : "-");

    HttpResponse res;
    handle_request(&req, &res);

    char header[512];
    int hlen = sprintf(header,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n\r\n",
        res.status_code, res.status_text,
        res.mime_type, res.content_size);

    send(client, header, hlen, 0);
    send(client, res.content, (int)res.content_size, 0);

    if (res.status_code == 200)
        free(res.content);

    closesocket(client);
    _endthreadex(0);
    return 0;
}

// ----------- 서버 메인 ----------
int run_http_server(int port)
{
    g_serverStopRequested = 0;
    g_serverSocket = INVALID_SOCKET;

    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        log_printf("[ERROR] WSAStartup 실패\n");
        return -1;
    }

    SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET) {
        log_printf("[ERROR] socket 실패\n");
        WSACleanup();
        return -1;
    }

    g_serverSocket = server;

    SOCKADDR_IN addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_printf("[ERROR] bind 실패\n");
        closesocket(server);
        g_serverSocket = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }

    if (listen(server, 5) == SOCKET_ERROR) {
        log_printf("[ERROR] listen 실패\n");
        closesocket(server);
        g_serverSocket = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }

    log_printf("[INFO] 서버 %d 포트 대기중...\n", port);

    while (!g_serverStopRequested) {
        SOCKADDR_IN cli;
        int clen = sizeof(cli);
        SOCKET client = accept(server, (SOCKADDR*)&cli, &clen);

        if (g_serverStopRequested) break;
        if (client == INVALID_SOCKET) {
            log_printf("[ERROR] accept 실패\n");
            continue;
        }

        ThreadArgs* ta = (ThreadArgs*)malloc(sizeof(ThreadArgs));
        if (!ta) {
            log_printf("[ERROR] malloc 실패\n");
            closesocket(client);
            continue;
        }

        ta->client_sock = client;
        const char* ip = inet_ntoa(cli.sin_addr);
        strcpy(ta->client_ip, ip ? ip : "unknown");

        unsigned tid = 0;
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, handle_client, ta, 0, &tid);
        if (!h) {
            log_printf("[ERROR] _beginthreadex 실패\n");
            closesocket(client);
            free(ta);
            continue;
        }
        CloseHandle(h);
    }

    if (server != INVALID_SOCKET)
        closesocket(server);

    g_serverSocket = INVALID_SOCKET;
    WSACleanup();

    log_printf("[INFO] 서버 종료됨.\n");
    return 0;
}
