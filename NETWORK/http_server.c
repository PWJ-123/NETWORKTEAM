#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>      // isxdigit
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

// 게시글 ID 증가용 (멀티스레드에서도 안전하게)
static LONG g_postIdCounter = 0;

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

// ---------- URL 디코더 (in-place) ----------
// application/x-www-form-urlencoded용: '+' -> ' ', "%xx" -> 바이트
static void url_decode_inplace(char* s) {
    if (!s) return;

    char* src = s;
    char* dst = s;

    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        }
        else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// ---------- x-www-form-urlencoded 파싱 ----------
// body 예: "nickname=우정&content=안녕테스트"
static void parse_form_body(
    const char* body,
    char* outNickname, size_t nickSize,
    char* outContent, size_t contentSize
) {
    if (!outNickname || !outContent) return;

    if (nickSize > 0)   outNickname[0] = '\0';
    if (contentSize > 0) outContent[0] = '\0';

    if (!body) return;

    char* temp = _strdup(body);
    if (!temp) return;

    char* context = NULL;
    char* token = strtok_s(temp, "&", &context);

    while (token) {
        char* eq = strchr(token, '=');
        if (eq) {
            *eq = '\0';
            char* key = token;
            char* val = eq + 1;

            url_decode_inplace(key);
            url_decode_inplace(val);

            if (strcmp(key, "nickname") == 0) {
                strncpy(outNickname, val, nickSize - 1);
                outNickname[nickSize - 1] = '\0';
            }
            else if (strcmp(key, "content") == 0) {
                strncpy(outContent, val, contentSize - 1);
                outContent[contentSize - 1] = '\0';
            }
        }

        token = strtok_s(NULL, "&", &context);
    }

    free(temp);

    // 닉네임이 비어 있으면 기본값
    if (outNickname[0] == '\0' && nickSize > 0) {
        strncpy(outNickname, "anonymous", nickSize - 1);
        outNickname[nickSize - 1] = '\0';
    }
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
// 여기서 POST /post 저장 + 기존 GET 정적파일 처리
void handle_request(const HttpRequest* req, HttpResponse* res) {
    // 1) POST /post : 글 저장 처리
    if (strcmp(req->method, "POST") == 0 && strcmp(req->uri, "/post") == 0) {

        char nickname[128] = { 0 };
        char content[2048] = { 0 };

        parse_form_body(req->body, nickname, sizeof(nickname), content, sizeof(content));

        log_printf("[POST] nickname='%s'\n", nickname);
        log_printf("[POST] content='%s'\n", content);

        // posts 폴더 경로 생성
        char postsDir[MAX_PATH];
        _snprintf_s(postsDir, sizeof(postsDir), _TRUNCATE, "%s\\posts", WEB_ROOT);

        // 폴더 생성 (이미 있으면 실패해도 상관없음)
        CreateDirectoryA(postsDir, NULL);

        // 파일 이름: post_YYYYMMDD_HHMMSS_ID.txt
        SYSTEMTIME st;
        GetLocalTime(&st);

        LONG id = InterlockedIncrement(&g_postIdCounter);

        char filePath[MAX_PATH];
        _snprintf_s(
            filePath, sizeof(filePath), _TRUNCATE,
            "%s\\post_%04d%02d%02d_%02d%02d%02d_%ld.txt",
            postsDir,
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond,
            (long)id
        );

        FILE* fp = fopen(filePath, "wb");
        if (!fp) {
            const char* msg = "<html><body><h1>500 Internal Server Error</h1><p>글 저장 실패</p></body></html>";
            res->status_code = 500;
            res->status_text = "Internal Server Error";
            res->mime_type = "text/html";
            // malloc 실패해도 최소한 static 문자열은 살아있게
            char* dyn = (char*)malloc(strlen(msg) + 1);
            if (dyn) {
                strcpy(dyn, msg);
                res->content = dyn;
            }
            else {
                res->content = (char*)msg; // 해제 대상 아님 (status_code 500이라 free 안 함)
            }
            res->content_size = (long)strlen(res->content);
            log_printf("[ERROR] 글 파일 열기 실패: %s\n", filePath);
            return;
        }

        // 파일에 저장 포맷 (나중에 읽기 쉽게 key=value 형태로 저장)
        fprintf(fp, "nickname=%s\r\n", nickname);
        fprintf(fp, "content=%s\r\n", content);
        fclose(fp);

        log_printf("[POST] 글 저장 완료: %s\n", filePath);

        // 클라이언트에게 돌려줄 HTML 응답 본문 만들기
        const char* tpl =
            "<html><body>"
            "<h1>글이 등록되었습니다.</h1>"
            "<p><b>닉네임:</b> %s</p>"
            "<p><b>내용:</b></p><pre>%s</pre>"
            "</body></html>";

        size_t needSize = strlen(tpl) + strlen(nickname) + strlen(content) + 32;
        char* html = (char*)malloc(needSize);
        if (!html) {
            const char* msg = "<html><body><h1>등록 완료</h1><p>메모리 부족으로 내용 표시 불가</p></body></html>";
            res->status_code = 200;
            res->status_text = "OK";
            res->mime_type = "text/html";
            char* dyn = (char*)malloc(strlen(msg) + 1);
            if (dyn) {
                strcpy(dyn, msg);
                res->content = dyn;
            }
            else {
                res->content = (char*)msg; // 200인데 static이라 free하면 안되지만, 아래에서 status_code==200일 때만 free라 안전
            }
            res->content_size = (long)strlen(res->content);
            return;
        }

        _snprintf_s(html, needSize, _TRUNCATE, tpl, nickname, content);
        res->status_code = 200;
        res->status_text = "OK";
        res->mime_type = "text/html";
        res->content = html;
        res->content_size = (long)strlen(html);
        return;
    }

    // 2) 그 외: 기존과 동일한 정적 파일 처리 (GET 등)
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

    // 200 OK 이면서 동적 메모리인 경우만 free
    if (res.status_code == 200) {
        // 정적 파일(read_file) or malloc(html) 둘 다 free 대상
        if (res.content != NULL) {
            free(res.content);
        }
    }

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
