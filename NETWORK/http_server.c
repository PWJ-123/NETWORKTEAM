#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "http_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define WEB_ROOT "C:\\GIMALWORKSPACE"
#define MAX_REQ_SIZE 4096

#define MAX_IP_TRACK     256
#define RATE_WINDOW_MS   5000     // 5초
#define RATE_MAX_REQ     3       // 허용 요청 수
#define BAN_TIME_MS      10000    // 차단 시간(ms)



typedef struct {
    char  ip[32];
    int   request_count;
    DWORD window_start;
    DWORD banned_until;
} IpRateInfo;


static IpRateInfo g_ipTable[MAX_IP_TRACK];
static CRITICAL_SECTION g_ipLock;


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
    int owns_content; // ✅ free 해야 하면 1, 아니면 0
} HttpResponse;

typedef struct {
    SOCKET client_sock;
    char client_ip[32];
} ThreadArgs;

static void log_printf(const char* fmt, ...);


static int check_ip_rate_limit(const char* ip) {
    DWORD now = GetTickCount();

    EnterCriticalSection(&g_ipLock);

    for (int i = 0; i < MAX_IP_TRACK; i++) {
        IpRateInfo* e = &g_ipTable[i];

        if (e->ip[0] == '\0') {
            strcpy(e->ip, ip);
            e->request_count = 1;
            e->window_start = now;
            e->banned_until = 0;
            LeaveCriticalSection(&g_ipLock);
            return 1;
        }

        if (strcmp(e->ip, ip) == 0) {

            if (e->banned_until > now) {
                LeaveCriticalSection(&g_ipLock);
                return 0;
            }

            if (now - e->window_start > RATE_WINDOW_MS) {
                e->window_start = now;
                e->request_count = 1;
                LeaveCriticalSection(&g_ipLock);
                return 1;
            }

            e->request_count++;
            if (e->request_count > RATE_MAX_REQ) {
                e->banned_until = now + BAN_TIME_MS;
                log_printf("[SECURITY] IP 차단: %s\n", ip);
                LeaveCriticalSection(&g_ipLock);
                return 0;
            }

            LeaveCriticalSection(&g_ipLock);
            return 1;
        }
    }

    LeaveCriticalSection(&g_ipLock);
    return 1;
}


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
    if (size < 0) {
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
static void parse_form_body(
    const char* body,
    char* outNickname, size_t nickSize,
    char* outContent, size_t contentSize
) {
    if (!outNickname || !outContent) return;

    if (nickSize > 0) outNickname[0] = '\0';
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
                if (nickSize > 0) {
                    strncpy(outNickname, val, nickSize - 1);
                    outNickname[nickSize - 1] = '\0';
                }
            }
            else if (strcmp(key, "content") == 0) {
                if (contentSize > 0) {
                    strncpy(outContent, val, contentSize - 1);
                    outContent[contentSize - 1] = '\0';
                }
            }
        }
        token = strtok_s(NULL, "&", &context);
    }

    free(temp);

    if (nickSize > 0 && outNickname[0] == '\0') {
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

/* =========================
   ✅ board.html 동적 생성용 유틸
   ========================= */

static void sb_ensure(char** s, size_t* cap, size_t need) {
    if (*cap >= need) return;
    size_t ncap = (*cap == 0) ? 4096 : *cap;
    while (ncap < need) ncap *= 2;
    char* p = (char*)realloc(*s, ncap);
    if (!p) return;
    *s = p;
    *cap = ncap;
}

static void sb_append(char** s, size_t* cap, size_t* len, const char* text) {
    if (!text) return;
    size_t add = strlen(text);
    size_t need = (*len) + add + 1;
    sb_ensure(s, cap, need);
    if (!*s) return;
    memcpy((*s) + (*len), text, add);
    *len += add;
    (*s)[*len] = '\0';
}

static void sb_appendf(char** s, size_t* cap, size_t* len, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    char tmp[2048];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);

    va_end(ap);

    if (n <= 0) return;
    tmp[sizeof(tmp) - 1] = '\0';
    sb_append(s, cap, len, tmp);
}

// HTML escape (& < > " ')
static char* html_escape_dup(const char* in) {
    if (!in) return _strdup("");
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p; ++p) {
        switch (*p) {
        case '&': n += 5; break;   // &amp;
        case '<': n += 4; break;   // &lt;
        case '>': n += 4; break;   // &gt;
        case '"': n += 6; break;   // &quot;
        case '\'': n += 5; break;  // &#39;
        default: n += 1; break;
        }
    }
    char* out = (char*)malloc(n + 1);
    if (!out) return _strdup("");
    char* d = out;
    for (const unsigned char* p = (const unsigned char*)in; *p; ++p) {
        switch (*p) {
        case '&': memcpy(d, "&amp;", 5); d += 5; break;
        case '<': memcpy(d, "&lt;", 4); d += 4; break;
        case '>': memcpy(d, "&gt;", 4); d += 4; break;
        case '"': memcpy(d, "&quot;", 6); d += 6; break;
        case '\'': memcpy(d, "&#39;", 5); d += 5; break;
        default: *d++ = (char)*p; break;
        }
    }
    *d = '\0';
    return out;
}

// posts\post_*.txt 파일 1개 파싱: nickname, content
static void parse_post_file(const char* path, char* outNick, size_t nickSz, char** outContentDyn) {
    if (nickSz > 0) outNick[0] = '\0';
    if (outContentDyn) *outContentDyn = NULL;

    long sz = 0;
    char* raw = read_file(path, &sz);
    if (!raw) return;

    // nickname=... \r\n
    char* nickPos = strstr(raw, "nickname=");
    if (nickPos) {
        nickPos += 9;
        char* eol = strstr(nickPos, "\r\n");
        size_t nlen = eol ? (size_t)(eol - nickPos) : strlen(nickPos);
        if (nlen >= nickSz) nlen = nickSz - 1;
        if (nickSz > 0) {
            memcpy(outNick, nickPos, nlen);
            outNick[nlen] = '\0';
        }
    }

    // content=... (이후는 파일 끝까지 content로 간주)
    char* contPos = strstr(raw, "content=");
    if (contPos) {
        contPos += 8;
        // 뒤쪽 불필요한 마지막 개행 정리
        size_t clen = strlen(contPos);
        while (clen > 0 && (contPos[clen - 1] == '\n' || contPos[clen - 1] == '\r')) {
            contPos[clen - 1] = '\0';
            clen--;
        }
        if (outContentDyn) *outContentDyn = _strdup(contPos);
    }
    else {
        if (outContentDyn) *outContentDyn = _strdup("");
    }

    if (nickSz > 0 && outNick[0] == '\0') {
        strncpy(outNick, "anonymous", nickSz - 1);
        outNick[nickSz - 1] = '\0';
    }

    free(raw);
}

// 파일명 정렬 (내림차순)
static int __cdecl cmp_desc(const void* a, const void* b) {
    const char* const* pa = (const char* const*)a;
    const char* const* pb = (const char* const*)b;
    return strcmp(*pb, *pa);
}

// GET /board.html 동적 페이지 생성
static char* build_board_html(long* outSize) {
    char postsDir[MAX_PATH];
    _snprintf_s(postsDir, sizeof(postsDir), _TRUNCATE, "%s\\posts", WEB_ROOT);
    CreateDirectoryA(postsDir, NULL);

    // 파일 목록 수집
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\post_*.txt", postsDir);

    WIN32_FIND_DATAA ffd;
    HANDLE hFind = FindFirstFileA(pattern, &ffd);

    char** names = NULL;
    size_t count = 0, cap = 0;

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (count + 1 > cap) {
                    size_t ncap = (cap == 0) ? 32 : cap * 2;
                    char** p = (char**)realloc(names, ncap * sizeof(char*));
                    if (!p) break;
                    names = p;
                    cap = ncap;
                }
                names[count++] = _strdup(ffd.cFileName);
            }
        } while (FindNextFileA(hFind, &ffd));
        FindClose(hFind);
    }

    if (count > 1) {
        qsort(names, count, sizeof(char*), cmp_desc);
    }

    char* html = NULL;
    size_t hcap = 0, hlen = 0;

    sb_append(&html, &hcap, &hlen,
        "<!doctype html>"
        "<html><head><meta charset='utf-8'/>"
        "<title>Board</title>"
        "<style>"
        "body{font-family:Arial, sans-serif; margin:20px;}"
        ".wrap{max-width:900px; margin:0 auto;}"
        ".post{border:1px solid #ddd; padding:12px; border-radius:8px; margin:10px 0;}"
        ".meta{font-size:12px; color:#666; margin-bottom:8px;}"
        "pre{white-space:pre-wrap; word-break:break-word; margin:0;}"
        "input,textarea{width:100%; padding:8px; box-sizing:border-box;}"
        "button{padding:10px 14px; cursor:pointer;}"
        "</style>"
        "</head><body><div class='wrap'>"
        "<h1>게시판</h1>"
        "<form method='POST' action='/post'>"
        "<label>닉네임</label><input name='nickname' placeholder='닉네임'/>"
        "<label style='display:block; margin-top:10px;'>내용</label>"
        "<textarea name='content' rows='5' placeholder='내용을 입력하세요'></textarea>"
        "<div style='margin-top:10px;'><button type='submit'>등록</button></div>"
        "</form>"
        "<hr/>"
        "<h2>글 목록</h2>"
    );

    if (count == 0) {
        sb_append(&html, &hcap, &hlen, "<p>아직 등록된 글이 없습니다.</p>");
    }
    else {
        for (size_t i = 0; i < count; i++) {
            char path[MAX_PATH];
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", postsDir, names[i]);

            char nick[128] = { 0 };
            char* content = NULL;

            parse_post_file(path, nick, sizeof(nick), &content);

            char* nickEsc = html_escape_dup(nick);
            char* contEsc = html_escape_dup(content ? content : "");

            sb_appendf(&html, &hcap, &hlen,
                "<div class='post'>"
                "<div class='meta'><b>%s</b> <span style='margin-left:10px;'>(%s)</span></div>"
                "<pre>%s</pre>"
                "</div>",
                nickEsc, names[i], contEsc
            );

            free(nickEsc);
            free(contEsc);
            if (content) free(content);
        }
    }

    sb_append(&html, &hcap, &hlen, "</div></body></html>");

    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);

    if (outSize) *outSize = (long)hlen;
    return html ? html : _strdup("<html><body><h1>Board Error</h1></body></html>");
}

// ----------- 요청 처리 ----------
void handle_request(const HttpRequest* req, HttpResponse* res) {
    if (!req || !res) return;

    // 기본값
    res->status_code = 500;
    res->status_text = "Internal Server Error";
    res->mime_type = "text/html";
    res->content = NULL;
    res->content_size = 0;
    res->owns_content = 0;

    // ✅ 0) GET /board.html : 동적 게시판(저장된 txt들을 HTML로 렌더)
    if (strcmp(req->method, "GET") == 0 && strcmp(req->uri, "/board.html") == 0) {
        long size = 0;
        char* page = build_board_html(&size);
        res->status_code = 200;
        res->status_text = "OK";
        res->mime_type = "text/html";
        res->content = page;
        res->content_size = size;
        res->owns_content = 1;
        return;
    }

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

        CreateDirectoryA(postsDir, NULL);

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
            char* dyn = (char*)malloc(strlen(msg) + 1);
            if (dyn) { strcpy(dyn, msg); }

            res->status_code = 500;
            res->status_text = "Internal Server Error";
            res->mime_type = "text/html";
            res->content = dyn ? dyn : NULL;
            res->content_size = dyn ? (long)strlen(dyn) : 0;
            res->owns_content = dyn ? 1 : 0;

            log_printf("[ERROR] 글 파일 열기 실패: %s\n", filePath);
            return;
        }

        fprintf(fp, "nickname=%s\r\n", nickname);
        fprintf(fp, "content=%s\r\n", content);
        fclose(fp);

        log_printf("[POST] 글 저장 완료: %s\n", filePath);

        // 클라이언트에게 돌려줄 HTML 응답 본문 만들기 (기존 기능 유지)
        const char* tpl =
            "<html><body>"
            "<h1>글이 등록되었습니다.</h1>"
            "<p><b>닉네임:</b> %s</p>"
            "<p><b>내용:</b></p><pre>%s</pre>"
            "<p style='color:#666;'>잠시 후 목록으로 이동합니다.</p>"
            "</body></html>";

        size_t needSize = strlen(tpl) + strlen(nickname) + strlen(content) + 64;
        char* html = (char*)malloc(needSize);
        if (!html) {
            const char* msg = "<html><body><h1>등록 완료</h1><p>메모리 부족으로 내용 표시 불가</p></body></html>";
            char* dyn = (char*)malloc(strlen(msg) + 1);
            if (dyn) strcpy(dyn, msg);

            res->status_code = 200;
            res->status_text = "OK";
            res->mime_type = "text/html";
            res->content = dyn ? dyn : NULL;
            res->content_size = dyn ? (long)strlen(dyn) : 0;
            res->owns_content = dyn ? 1 : 0;
            return;
        }

        _snprintf_s(html, needSize, _TRUNCATE, tpl, nickname, content);
        res->status_code = 200;
        res->status_text = "OK";
        res->mime_type = "text/html";
        res->content = html;
        res->content_size = (long)strlen(html);
        res->owns_content = 1;
        return;
    }

    // 2) 그 외: 기존과 동일한 정적 파일 처리 (GET 등)
    {
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
            res->owns_content = 1;
        }
        else {
            res->status_code = 404;
            res->status_text = "Not Found";
            res->mime_type = "text/html";
            res->content = "<h1>404 Not Found</h1>";
            res->content_size = (long)strlen(res->content);
            res->owns_content = 0;
        }
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

    // 🔐 IP 과다 요청 차단
    if (!check_ip_rate_limit(ip)) {
        log_printf("[SECURITY] 요청 차단됨: %s\n", ip);
        closesocket(client);
        _endthreadex(0);
        return 0;
    }



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
    memset(&res, 0, sizeof(res));
    handle_request(&req, &res);

    char header[512];
    int hlen = sprintf(header,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n\r\n",
        res.status_code, res.status_text,
        res.mime_type ? res.mime_type : "text/plain",
        res.content_size);

    send(client, header, hlen, 0);

    if (res.content && res.content_size > 0) {
        send(client, res.content, (int)res.content_size, 0);
    }

    // ✅ 안전한 free (동적 할당된 것만)
    if (res.owns_content && res.content) {
        free(res.content);
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
    InitializeCriticalSection(&g_ipLock);
    memset(g_ipTable, 0, sizeof(g_ipTable));

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
