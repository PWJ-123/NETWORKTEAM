#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>    // _beginthreadex, _endthreadex
#include <windows.h>    // GetCurrentThreadId, HANDLE

#pragma comment(lib, "ws2_32.lib")

// 서버 설정: 백슬래시를 반드시 두 번 '\\' 사용하여 이스케이프
#define WEB_ROOT "C:\\GIMALWORKSPACE"
#define MAX_REQ_SIZE 4096

// --- 1. 데이터 구조 정의 ---

// HTTP 요청 정보 구조체
typedef struct {
    char method[10];// GET, POST 등
    char uri[256];// 요청 URI (예: /index.html)
    char query_string[256];// 새로 추가: GET 파라미터 (예: name=test&id=123)
    char* body;// POST 요청 본문 시작 포인터 (요청 버퍼 내)
} HttpRequest;

// HTTP 응답 정보 구조체
typedef struct {
    int status_code;// 200, 404 등
    const char* status_text;// OK, Not Found 등
    const char* mime_type;// Content-Type 
    char* content;// 응답 본문 데이터 (동적 할당된 메모리 포인터)
    long content_size;// 응답 본문의 크기
} HttpResponse;

typedef struct {
    SOCKET client_sock;
} ThreadArgs;


void error_handling(const char* message);
char* read_file(const char* path, long* out_size);
const char* get_mime_type(const char* file_path);
void parse_request(const char* request_raw, HttpRequest* req);
void handle_request(const HttpRequest* req, HttpResponse* res);
unsigned __stdcall handle_client(void* arg_ptr); // 담당자 5: 스레드 함수

// --- 유틸리티 함수 (공통) ---

void error_handling(const char* message) {
    fputs(message, stderr);
    fputc('\n', stderr);
    exit(1);
}

// 파일 읽어서 버퍼에 저장 (메모리 동적 할당됨)
char* read_file(const char* path, long* out_size) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    fread(buffer, 1, size, fp);
    buffer[size] = '\0';
    fclose(fp);

    if (out_size) *out_size = size;
    return buffer;
}

// 파일 확장자에 따라 Content-Type (MIME 타입) 결정
const char* get_mime_type(const char* file_path) {
    const char* dot = strrchr(file_path, '.');
    if (!dot || dot == file_path) return "application/octet-stream";

    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".gif") == 0) return "image/gif";

    return "text/plain";
}


// --- 2. HTTP 요청 파서 함수 ---

/**
 * 소켓으로부터 받은 raw HTTP 요청을 파싱하여 HttpRequest 구조체를 채웁니다.
 * 
 */
void parse_request(const char* request_raw, HttpRequest* req) {

    // 1. 요청 라인 파싱 (메서드, URI)
    const char* line_end = strstr(request_raw, "\r\n");
    if (!line_end) return;

    // 메서드 추출
    const char* method_end = strchr(request_raw, ' ');
    if (method_end) {
        size_t len = method_end - request_raw;
        if (len < sizeof(req->method)) {
            strncpy(req->method, request_raw, len);
            req->method[len] = '\0';
        }
    }

    // URI 추출
    const char* uri_start = method_end ? method_end + 1 : NULL;
    const char* http_ver_start = strchr(uri_start, ' '); // URI 뒤 공백 (HTTP/1.1 시작점)
    const char* query_start = strchr(uri_start, '?'); // 물음표(?) 위치

    // URI 끝 지점 결정: 물음표가 있으면 물음표가 끝이고, 없으면 HTTP 버전 앞 공백이 끝
    const char* uri_end = http_ver_start;
    if (query_start && query_start < http_ver_start) {
        uri_end = query_start;
    }

    if (uri_start && uri_end) {
        size_t len = uri_end - uri_start;
        if (len < sizeof(req->uri)) {
            strncpy(req->uri, uri_start, len);
            req->uri[len] = '\0';
        }
    }

    // --- 2. 쿼리 스트링 추출 (GET 파라미터) ---
    if (query_start) {
        // 물음표(?) 다음 문자부터 URI 끝까지 (HTTP/1.1 앞까지) 복사
        const char* qs_start = query_start + 1; // 물음표 건너뛰기
        size_t qs_len = http_ver_start - qs_start;

        if (qs_len < sizeof(req->query_string)) {
            strncpy(req->query_string, qs_start, qs_len);
            req->query_string[qs_len] = '\0';
        }
    }


    // 3. POST 본문 시작 위치 파악 (body 포인터 설정)
    if (strcmp(req->method, "POST") == 0) {
        const char* body_start = strstr(request_raw, "\r\n\r\n");
        if (body_start) {
            req->body = (char*)(body_start + 4); // "\r\n\r\n" 건너뛰기
        }
        else {
            req->body = (char*)(body_start + 4);
        }
    }
}
void handle_request(const HttpRequest* req, HttpResponse* res) {
    char full_path[512];

    // URI가 '/'만 있을 경우 index.html로 처리
    if (strcmp(req->uri, "/") == 0) {
        sprintf(full_path, "%s\\index.html", WEB_ROOT);
    }
    else {
        // WEB_ROOT + URI 결합 (Windows 경로)
        sprintf(full_path, "%s%s", WEB_ROOT, req->uri);
        // 슬래시를 백슬래시로 변환 (Windows 파일 시스템)
        for (char* p = full_path; *p; p++) {
            if (*p == '/') *p = '\\';
        }
    }

    long content_size = 0;
    char* content = read_file(full_path, &content_size);

    if (content) {
        res->status_code = 200;
        res->status_text = "OK";
        res->mime_type = get_mime_type(full_path);
        res->content = content;
        res->content_size = content_size;
    }
    else {
        res->status_code = 404;
        res->status_text = "Not Found";
        res->mime_type = "text/html";
        res->content = "<html><head><title>404 Not Found</title></head><body><h1>404 Not Found</h1><p>Requested resource not found.</p></body></html>";
        res->content_size = strlen(res->content);
    }
}


// --- 3. 요청 처리 함수---

/**
 * HttpRequest를 분석하여 적절한 HttpResponse를 생성
 * 
 */
 */
unsigned __stdcall handle_client(void* arg_ptr) {
    ThreadArgs* args = (ThreadArgs*)arg_ptr;
    SOCKET client_sock = args->client_sock;
    free(arg_ptr);

    char request_raw[MAX_REQ_SIZE];
    int str_len = recv(client_sock, request_raw, sizeof(request_raw) - 1, 0);

    if (str_len <= 0) {
        printf("[LOG] 스레드 %lu: 클라이언트 연결 끊김 또는 데이터 수신 실패. 소켓 종료.\n", GetCurrentThreadId());
        closesocket(client_sock);
        _endthreadex(0); // 스레드 종료
        return 0;
    }

    request_raw[str_len] = '\0';

    printf("-------------------- [NEW THREAD %lu] --------------------\n", GetCurrentThreadId());
    printf("[RAW REQUEST]\n%s", request_raw);

    HttpRequest request;
    memset(&request, 0, sizeof(HttpRequest));
    parse_request(request_raw, &request);

    HttpResponse response;
    memset(&response, 0, sizeof(HttpResponse));
    handle_request(&request, &response);

    char header[512];

    sprintf(header,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s; charset=UTF-8\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n\r\n",
        response.status_code, response.status_text,
        response.mime_type, response.content_size);

    printf("-------------------- [THREAD %lu] --------------------\n", GetCurrentThreadId());
    printf("[RESPONSE HEADER]\n%s", header);
    printf("------------------------------------------------------\n");

    send(client_sock, header, strlen(header), 0);
    send(client_sock, response.content, response.content_size, 0);

    printf("[LOG] 스레드 %lu: 응답 전송 완료 (%d %s). 소켓 종료.\n",
        GetCurrentThreadId(), response.status_code, response.status_text);


    if (response.content != NULL && response.status_code == 200) {
        free(response.content);
    }
    closesocket(client_sock);

    _endthreadex(0); // 스레드 종료
    return 0;
}

// --- main 함수 ---

int main(void) {
    WSADATA wsaData;
    SOCKADDR_IN server_addr;
    int port = 8080;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        error_handling("WSAStartup() error");

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == INVALID_SOCKET) error_handling("socket() error");

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(server_sock, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
        error_handling("bind() error");

    if (listen(server_sock, 5) == SOCKET_ERROR)
        error_handling("listen() error");


    printf("========================================\n");
    printf("HTTP 서버 %d 포트에서 대기 중...\n", port);
    printf("--- 파일 루트 경로: %s ---\n", WEB_ROOT);
    printf("[LOG] 멀티스레드 동시성 모델 사용.\n");
    printf("========================================\n");

    while (1) {
        SOCKET client_sock;

        printf("\n[LOG] 메인 스레드 %lu: 새로운 클라이언트 연결 대기 중...\n", GetCurrentThreadId());
        client_sock = accept(server_sock, NULL, NULL);

        if (client_sock == INVALID_SOCKET) {
            printf("[LOG] accept() 오류 발생. 다시 대기.\n");
            continue; // 이제 while 루프 내부에 있으므로 유효함
        }



        ThreadArgs* args = (ThreadArgs*)malloc(sizeof(ThreadArgs));
        if (args == NULL) {
            printf("[LOG] 메모리 할당 실패 (ThreadArgs). 연결 종료.\n");
            closesocket(client_sock);
            continue;
        }
        args->client_sock = client_sock;

        // 새로운 스레드 생성
        unsigned long thread_id;
        HANDLE hThread = (HANDLE)_beginthreadex(
            NULL,               // 보안 속성
            0,                  // 스택 크기
            handle_client,      // 스레드 함수 포인터
            (void*)args,        // 스레드 함수 인자
            0,                  // 생성 플래그
            &thread_id          // 스레드 ID 저장 변수
        );

        if (hThread == NULL) {
            printf("[LOG] 스레드 생성 실패! 연결 종료.\n");
            free(args);
            closesocket(client_sock);
        }
        else {
            CloseHandle(hThread);
            printf("[LOG] 메인 스레드: 클라이언트 처리를 위한 새 스레드 %lu 생성 완료.\n", thread_id);
        }
    }

    closesocket(server_sock);
    WSACleanup();
    return 0;
}