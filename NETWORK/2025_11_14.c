//#define _WINSOCK_DEPRECATED_NO_WARNINGS
//#define _CRT_SECURE_NO_WARNINGS
//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
//#include <winsock2.h>
//#include <ws2tcpip.h>
//
//#pragma comment(lib, "ws2_32.lib")
//
//// 서버 설정
//#define WEB_ROOT "C:\\GIMALWORKSPACE" //이것도 GUI에서 설정 가능하게 (1)
//#define MAX_REQ_SIZE 4096 
//
//// --- 1. 데이터 구조 정의 ---
//
////  HTTP 요청 정보 구조체
//typedef struct {
//    char method[10];    // GET, POST 등
//    char uri[256];      // 요청 URI (예: /index.html)
//    char query_string[256]; // 새로 추가: GET 파라미터 (예: name=test&id=123)
//    char* body;         // POST 요청 본문 시작 포인터 (요청 버퍼 내)
//} HttpRequest;
//
//// HTTP 응답 정보 구조체
//typedef struct {
//    int status_code;             // 200, 404 등
//    const char* status_text;     // OK, Not Found 등
//    const char* mime_type;       // Content-Type 
//    char* content;               // 응답 본문 데이터 (동적 할당된 메모리 포인터)
//    long content_size;           // 응답 본문의 크기
//} HttpResponse;
//
//// --- 유틸리티 함수 (공통) ---
//
//void error_handling(const char* message) {
//    fputs(message, stderr);
//    fputc('\n', stderr);
//    exit(1);
//}
//
//// 파일 읽어서 버퍼에 저장 (메모리 동적 할당됨)
//char* read_file(const char* path, long* out_size) {
//    FILE* fp = fopen(path, "rb");
//    if (!fp) return NULL;
//
//    fseek(fp, 0, SEEK_END);
//    long size = ftell(fp);
//    fseek(fp, 0, SEEK_SET);
//
//    char* buffer = (char*)malloc(size + 1);
//    if (!buffer) {
//        fclose(fp);
//        return NULL;
//    }
//
//    fread(buffer, 1, size, fp);
//    buffer[size] = '\0';
//    fclose(fp);
//
//    if (out_size) *out_size = size;
//    return buffer;
//}
//
//// 파일 확장자에 따라 Content-Type (MIME 타입) 결정
//const char* get_mime_type(const char* file_path) {
//    const char* dot = strrchr(file_path, '.');
//    if (!dot || dot == file_path) return "application/octet-stream";
//
//    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
//    if (strcmp(dot, ".css") == 0) return "text/css";
//    if (strcmp(dot, ".js") == 0) return "application/javascript";
//    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
//    if (strcmp(dot, ".png") == 0) return "image/png";
//    if (strcmp(dot, ".gif") == 0) return "image/gif";
//
//    return "text/plain";
//}
//
//
//// --- 2. HTTP 요청 파서 함수 ---
//
///**
// * 소켓으로부터 받은 raw HTTP 요청을 파싱하여 HttpRequest 구조체를 채웁니다.
// * 
// */
//void parse_request(const char* request_raw, HttpRequest* req) {
//
//    // 1. 요청 라인 파싱 (메서드, URI)
//    const char* line_end = strstr(request_raw, "\r\n");
//    if (!line_end) return;
//
//    // 메서드 추출
//    const char* method_end = strchr(request_raw, ' ');
//    if (method_end) {
//        size_t len = method_end - request_raw;
//        if (len < sizeof(req->method)) {
//            strncpy(req->method, request_raw, len);
//            req->method[len] = '\0';
//        }
//    } 
//
//    // URI 추출
//    const char* uri_start = method_end ? method_end + 1 : NULL;
//    const char* uri_end = strchr(uri_start, ' ');
//    if (uri_start && uri_end) {
//        size_t len = uri_end - uri_start;
//        if (len < sizeof(req->uri)) {
//            strncpy(req->uri, uri_start, len);
//            req->uri[len] = '\0';
//        }
//    }
//
//    // 2. POST 본문 시작 위치 파악 (body 포인터 설정)
//    if (strcmp(req->method, "POST") == 0) {
//        const char* body_start = strstr(request_raw, "\r\n\r\n");
//        if (body_start) {
//            req->body = (char*)(body_start + 4); // "\r\n\r\n" 건너뛰기
//        }
//        else {
//            req->body = NULL;
//        }
//    }
//    else {
//        req->body = NULL;
//    }
//
//    printf("[PARSER] 메서드: %s, URI: %s\n", req->method, req->uri);
//    if (req->body) {
//        printf("[PARSER] POST 본문 데이터 감지: %s\n", req->body);
//    }
//}
//
//
//// --- 3. 요청 처리 함수---
//
///**
// * HttpRequest를 분석하여 적절한 HttpResponse를 생성합니다.
// * 
// */
//void handle_request(const HttpRequest* req, HttpResponse* res) {
//
//    // 1. POST 요청 처리 (동적 처리 시뮬레이션)
//    if (strcmp(req->method, "POST") == 0) {
//
//        // 동적 응답 본문을 위한 메모리 할당 (최대 1024바이트로 가정)
//        char* response_buffer = (char*)malloc(1024);
//        if (response_buffer) {
//            sprintf(response_buffer,
//                "<html><body><h1>[POST 처리] 데이터 수신 성공</h1>"
//                "<p>요청 경로: <b>%s</b></p>"
//                "<p>수신 데이터: <pre>%s</pre></p>"
//                "</body></html>",
//                req->uri, req->body ? req->body : "데이터 없음");
//
//            res->status_code = 200;
//            res->status_text = "OK";
//            res->mime_type = "text/html";
//            res->content = response_buffer;
//            res->content_size = strlen(response_buffer);
//        }
//        else {
//            // 메모리 할당 실패 (500)
//            res->status_code = 500;
//            res->status_text = "Internal Server Error";
//            res->mime_type = "text/html";
//            res->content = "<html><body><h1>500 Internal Server Error</h1></body></html>";
//            res->content_size = strlen(res->content);
//        }
//        return;
//    }
//
//    // 2. GET 요청 처리 (정적 파일 서빙)
//    else if (strcmp(req->method, "GET") == 0) {
//
//        char local_uri[256];
//        strcpy(local_uri, req->uri);
//
//        // 루트 요청 '/'의 경우, 'index.html'로 변경
//        if (strcmp(local_uri, "/") == 0) {
//            strcpy(local_uri, "/index.html");
//        }
//
//        // 로컬 파일 경로 생성 (WEB_ROOT + URI)
//        char file_path[512] = { 0, };
//
//        // '\'를 경로 구분자로 사용하기 위해 uri의 '/'를 '\'로 변경
//        for (int i = 0; local_uri[i]; i++) {
//            if (local_uri[i] == '/') local_uri[i] = '\\';
//        }
//        sprintf(file_path, "%s%s", WEB_ROOT, local_uri);
//
//        // 파일 읽기 시도
//        char* file_content = read_file(file_path, &res->content_size);
//
//        if (file_content) {
//            // 파일 읽기 성공 (200 OK)
//            res->status_code = 200;
//            res->status_text = "OK";
//            res->mime_type = get_mime_type(file_path);
//            res->content = file_content; // read_file에서 할당된 메모리 포인터
//            printf("[HANDLER] 파일 로드 성공: %s\n", file_path);
//
//        }
//        else {
//            // 파일 읽기 실패 (404 Not Found)
//            res->status_code = 404;
//            res->status_text = "Not Found";
//            res->mime_type = "text/html";
//            // 404 응답 본문 생성 (정적 문자열)
//            res->content = "<html><body><h1>404 Not Found</h1><p>Resource not found.</p></body></html>";
//            res->content_size = strlen(res->content);
//            printf("[HANDLER] 404 Not Found: %s\n", file_path);
//        }
//        return;
//    }
//
//    // 3. 기타 메서드 처리 (501 Not Implemented)
//    res->status_code = 501;
//    res->status_text = "Not Implemented";
//    res->mime_type = "text/html";
//    res->content = "<html><body><h1>501 Not Implemented</h1></body></html>";
//    res->content_size = strlen(res->content);
//    printf("[HANDLER] 501 Not Implemented: %s\n", req->method);
//}
//
//// --- main 함수 ---
//
//int main(void) {
//    WSADATA wsaData;
//    SOCKET server_sock, client_sock;
//    SOCKADDR_IN server_addr;
//    int port = 8080;
//
//    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
//        error_handling("WSAStartup() error");
//
//    server_sock = socket(AF_INET, SOCK_STREAM, 0);
//    if (server_sock == INVALID_SOCKET) error_handling("socket() error");
//
//    memset(&server_addr, 0, sizeof(server_addr));
//    server_addr.sin_family = AF_INET;
//    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
//    server_addr.sin_port = htons(port);
//
//    if (bind(server_sock, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
//        error_handling("bind() error");
//
//    if (listen(server_sock, 5) == SOCKET_ERROR)
//        error_handling("listen() error");
//
//    printf("HTTP 서버 %d 포트에서 대기 중...\n", port);
//    printf("--- 파일 루트 경로: %s ---\n", WEB_ROOT);
//
//    while (1) {
//        printf("\n[DEBUG] 새로운 클라이언트 연결 대기 중...\n");
//        client_sock = accept(server_sock, NULL, NULL); 
//
//        if (client_sock == INVALID_SOCKET) continue;
//
//        printf("[INFO] 클라이언트 연결 수락됨!\n");
//
//        char request_raw[MAX_REQ_SIZE];
//        int str_len = recv(client_sock, request_raw, sizeof(request_raw) - 1, 0);
//
//        if (str_len <= 0) {
//            closesocket(client_sock);
//            continue;
//        }
//
//        request_raw[str_len] = '\0'; // 수신된 데이터의 끝에 NULL 종단 문자 추가
//
//        // ★★★수신된 HTTP 요청 내용 로그 출력 ★★★
//        printf("----------------------------------------\n");
//        printf("[RAW REQUEST]\n%s", request_raw);
//
//        //요청 파싱 ---
//        HttpRequest request;
//        memset(&request, 0, sizeof(HttpRequest));
//        parse_request(request_raw, &request);
//
//        //요청 처리 ---
//        HttpResponse response;
//        memset(&response, 0, sizeof(HttpResponse));
//        handle_request(&request, &response); 
//
//        // --- 응답 생성 및 전송 ---
//        char header[512];
//
//        // 응답 헤더 생성
//        sprintf(header,
//            "HTTP/1.1 %d %s\r\n"
//            "Content-Type: %s; charset=UTF-8\r\n"
//            "Content-Length: %ld\r\n"
//            "Connection: close\r\n\r\n",
//            response.status_code, response.status_text,
//            response.mime_type, response.content_size);
//
//        // ★★★ 생성된 헤더 내용 CMD에 출력 ★★★
//        printf("----------------------------------------\n");
//        printf("[RESPONSE HEADER]\n%s", header);
//        printf("----------------------------------------\n");
//
//        // 헤더 전송
//        send(client_sock, header, strlen(header), 0);
//
//        // 본문 전송
//        send(client_sock, response.content, response.content_size, 0);
//
//        printf("[INFO] 응답 전송 완료 (%d %s). 소켓 종료.\n", response.status_code, response.status_text);
//
//        // --- 메모리 정리 ---
//        // read_file로 할당받았거나 POST 처리에서 할당받은 메모리 해제
//        // 404/500 응답 본문은 정적 문자열이므로 해제하지 않음.
//        if (response.content != NULL) {
//            if (response.status_code == 200) {
//                free(response.content);
//            }
//        }
//
//        closesocket(client_sock);
//    }
//
//    closesocket(server_sock);
//    WSACleanup();
//    return 0;
//}