/*
* 클라이언트 프로그램 요약
* ===========================================================
* 특징
* O 구형 인코딩 호환성
*   - 서버는 ANSI 인코딩을 사용하지만 클라이언트 WebView는 UTF-8을 표준으로 사용함
*   - 클라이언트 프로그램이 서버 <-> WebView의 중간에서 통역을 수행함
*   - 송신: URL decode ->  UTF-8 -> ANSI | 수신: ANSI -> UTF-8
* O 스크립트 주입
*   - 원본이 되는 순수 HTML 문서를 수정하지 않고 클라이언트 프로그램을 경유하도록 제작
*   - AddScriptToExecuteOnDocumentCreated 사용
* O 비동기 소켓 통신
*   - 스레드를 사용하여 통신 간 UI 프리징 방지
* ===========================================================
* 클라이언트 프로그램의 자체 한계
* X 최신 Web API / AJAX
*   - 웹 뷰가 아닌 자체구현 소켓으로만 통신.
*   - 그러므로 브라우저 자체 통신을 사용하는 기능은 동작하지 않음
* X 이미지 / 파일 처리
*   - 통신 로직이 문자열 기반이기 때문
*   - 바이너리 데이터(이미지, 파일) 데이터 내의 NULL 문자를 오인하여 에러가 생길 수 있음
* X HTTPS
*   - 현재는 평문 통신만 지원
*   - 보안 연결을 위해서는 추가적인 외부 라이브러리(OPENSSL 같은 것들) 필요
* ===========================================================
* 서버 - 클라이언트 연동 간의 한계
* > 자동 리다이렉션
*   - 현재는 클라이언트에서 JS 인젝션을 통해 강제로 이동시킴
* > 동적 페이지
*   - 현재 통신 방식(단발성, 갱신X)의 한계
*/

//#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <stdlib.h>
#include <process.h>    
#include <WebView2.h>
#include <wrl.h>
#include <stdio.h>  // 파일 입출력에 사용
#include "resource.h"

#pragma comment(lib, "ws2_32.lib")

// --- 상수 ---
#define MAX_REQ_SIZE 4096                               // 최대 반환값 사이즈
// 프로시저에서 사용되는 메시지 ID
#define WM_UPDATE_WEBVIEW (WM_USER + 1)     // WebView 화면 갱신 요청
#define WM_APP_LOG (WM_USER + 2)                 // 로그 출력 요청

#define COOKIE_FILE "cookie.txt"                         // 쿠키 저장 텍스트 파일(프로젝트 파일에 저장됨)

// COM 관리를 위한 네임스페이스
using namespace Microsoft::WRL;

// --- 전역 변수 ---
// WebView2 관련
ComPtr<ICoreWebView2Controller> webview_controller; // 컨트롤러 객체. 스마트 포인터 사용에 의한 메모리 자동 관리
ComPtr<ICoreWebView2> webview;                           // 뷰 객체

// 현재 보고 있는 페이지의 주소를 기억하는 변수 (기본값: /index.html)
std::string g_current_uri = "/index.html";

// 네비게이션에 쓰이는 스택(가변배열 vector 사용)
std::vector<std::string> g_back_history;
std::vector<std::string> g_forward_history;

// 쿠키 저장소
std::string g_cookie_storage = "";

// --- 함수 원형 선언 ---
std::string request_HTML(HWND hDlg, const char* server_ip, int port, const char* method, const char* uri, const char* post_data);
std::wstring string_to_wstring(const std::string& s);
std::string utf8_to_ansi(const std::string& utf8);
std::string url_decode(const std::string& raw);
void StartRequestThread(HWND hDlg, const char* method, const char* uri, const char* body, bool is_navigating = false);
void Log(HWND hDlg, const char* fmt, ...);

// 작업 스레드 파라미터 구조체
// 인자를 하나만 받을 수 있으므로 구조체로 전달해서 2개 이상의 정보 전달
struct ThreadParam {
    char method[10] = { 0 };     // 메서드(GET, POST) 유형
    char uri[256] = { 0 };          // 요청 경로
    char ip[32] = { 0 };            // 서버 IP 주소
    int port = 8080;            // 서버 PORT
    std::string body;               // POST 데이터 본문
    HWND hDlg = NULL;        // 메인 다이얼로그 핸들(로그 출력용)
};

// --- 함수 선언 ---

// 쿠키 관련 함수: 프로그램을 종료해도 정보가 유지
// 쿠키 파일 불러오기
void LoadCookieFromFile() {
    FILE* fp = fopen(COOKIE_FILE, "r");
    if (fp) {
        char buf[1024] = { 0 };
        if (fgets(buf, sizeof(buf), fp)) {
            // 개행문자 제거
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n')
                buf[len - 1] = '\0';
            g_cookie_storage = buf;
        }
        fclose(fp);
    }
}

// 쿠키 파일 저장
void SaveCookieToFile() {
    if (g_cookie_storage.empty()) {
        // 쿠키가 없다면 파일 자체를 삭제
        remove(COOKIE_FILE);
        return;
    }
    FILE* fp = fopen(COOKIE_FILE, "w");
    if (fp) {
        fprintf(fp, "%s", g_cookie_storage.c_str());
        fclose(fp);
    }
}


// 백그라운드 작업 스레드 함수: 통신 간 UI 멈춤 방지를 위한 스레드
unsigned __stdcall RequestThreadFunc(void* arg) {
    ThreadParam* param = (ThreadParam*)arg;

    // 소켓 통신 수행
    std::string html = request_HTML(param->hDlg, param->ip, param->port, param->method, param->uri, param->body.c_str());

    // 자동 리다이렉트: 글쓰기 요청이었다면 n초후 자동 리다이렉션
    if (strcmp(param->method, "POST") == 0) {
        // R"()": 여러 줄의 문자열을 간편하게 입력하기 위해 사용
        std::string redirect_script = R"(
            <script>
                setTimeout(function() {
                    window.chrome.webview.postMessage('GET:/board.html');
                }, 5000);
            </script>
            )";
        html = redirect_script + html;
        Log(param->hDlg, "[SYSTEM] 글 작성 완료. 5초 후 목록으로 이동합니다...");
    }

    // 결과물을 메모리(힙)에 할당: 스레드가 끝나도 사라지지 않도록 new 키워드 할당
    std::string* final_content = new std::string(html);

    // 메인 스레드(UI 담당)로 화면 갱신 메시지 송신
    // PostMessage: 논 블로킹 SendMessage(). UI 프리징 또는 교착상태 방지
    PostMessage(param->hDlg, WM_UPDATE_WEBVIEW, 0, (LPARAM)final_content);

    // 파리미터 메모리 해제
    delete param;

    return 0;
}

// 스레드 시작 및 현재 상태 동기화 헬퍼 함수
void StartRequestThread(HWND hDlg, const char* method, const char* uri, const char* body, bool is_navigating) {

    // 일반적인 페이지 이동
    // 현재 주소 변수(g_current_uri): 마지막 주소 저장
    // 글쓰기 요청인 경우에는 갱신하지 않음: 페이지 리다이렉션이 아닌 단순 html 문서만 수신하니까
    if (!is_navigating && uri != NULL && strcmp(method, "POST") != 0) {

        // 현재 페이지가 있고, 이동하려는 페이지와 다르다면 '뒤로가기 목록'에 저장
        if (!g_current_uri.empty() && g_current_uri != uri) {
            g_back_history.push_back(g_current_uri); // [수정] push_back 사용
        }

        // 새로운 길로 가는 것이므로 '앞으로 가기' 목록은 싹 비움
        g_forward_history.clear(); // [수정] clear 사용

        g_current_uri = uri;
    }


    // URL 입력창 텍스트 갱신
    std::wstring w_uri = string_to_wstring(g_current_uri);
    SetDlgItemText(hDlg, IDC_EDIT_URI, w_uri.c_str());

    // 3. 스레드 파라미터 설정
    ThreadParam* param = new ThreadParam;
    strncpy(param->method, method, 9);
    strncpy(param->uri, uri, 255);
    if (body) param->body = body;

    // 다이얼로그
    param->hDlg = hDlg;
    // 포트 번호(없으면 기본값)
    param->port = GetDlgItemInt(hDlg, IDC_EDIT_PORT, NULL, FALSE);
    if (param->port == 0) param->port = 8080;

    // IP 주소 가져오기
    GetDlgItemTextA(hDlg, IDC_EDIT_IP, param->ip, 31);

    // 비어있을 경우의 기본값 설정
    if (strlen(param->ip) == 0)
        strcpy(param->ip, "127.0.0.1");

    // 스레드 시작
    // CreateThread -> _beginThreadex: C++ 환경에서 더 안전한 방식
    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, RequestThreadFunc, param, 0, NULL);
    if (hThread) CloseHandle(hThread);  // 핸들을 닫더라도 스레드는 종료되지 않음
}

// 로그 출력 함수
// 스레드 환경에서의 안정성 확보
void Log(HWND hDlg, const char* fmt, ...) {
    if (!hDlg) return;

    // 가변 인자 처리: 포맷 스트링(%d, %c ...) 사용 가능
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // 메시지를 복사해서 메모리(힙)에 할당(PostMessage 사용을 위해)
    char* msg_copy = new char[strlen(buf) + 1];
    strcpy(msg_copy, buf);

    // 메인 스레드에 로그 출력 요청
    PostMessage(hDlg, WM_APP_LOG, 0, (LPARAM)msg_copy);
}

/*
* 소켓 통신 함수
* > 입력값: 다이얼로그 정보, 요청 방식, 요청 URI, POST 방식에서 사용되는 데이터
* > 반환값: HTML Body(문자열 형태)
* - 소켓을 생성하여 HTTP 프로토콜을 준수하는 패킷을 제작하여 서버와 송/수신
*/
std::string request_HTML(HWND hDlg, const char* server_ip, int port, const char* method, const char* uri, const char* post_data) {
    WSADATA wsaData;
    SOCKET client_sock;
    SOCKADDR_IN server_addr;
    int server_port = port;

    Log(hDlg, "[SYSTEM] 소켓 초기화중...");

    // winsock 초기화
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log(hDlg, "[ERROR] WSAStartup() 실패");
        return "Error: WSAStartup Failed";
    }

    // 소켓 생성
    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock == INVALID_SOCKET) {
        WSACleanup();
        Log(hDlg, "[ERROR] 소켓 생성 실패");
        return "Error: Socket Creation Failed";
    }

    Log(hDlg, "[SYSTEM] 소켓 완료!");

    // 서버 정보 설정
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);
    server_addr.sin_port = htons(server_port);

    Log(hDlg, "[NETWORK] 서버(%s: %d) 연결 시도...", server_ip, server_port);

    // 연결 요청
    if (connect(client_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        // 연결 실패
        closesocket(client_sock);
        WSACleanup();
        Log(hDlg, "[ERROR] 서버 연결 실패 (서버 상태 확인)");
        return "<html><body><h1>Connection Failed</h1><p>Server is offline.</p></body></html>";
    }

    Log(hDlg, "[NETWORK] 서버 연결 성공! 데이터 전송 시작...");

    // HTTP 패킷 생성
    std::string request_packet;
    std::string str_uri = (uri[0] == '/') ? uri : '/' + std::string(uri);

    // request line: METHOD URI HTTP/1.1
    request_packet += std::string(method) + " " + str_uri + " HTTP/1.1\r\n";
    request_packet += "Host: " + std::string(server_ip) + "\r\n";

    // 저장된 쿠키가 있다면 헤더에 실어서 보냄(세션 유지)
    if (!g_cookie_storage.empty()) {
        request_packet += "Cookie: " + g_cookie_storage + "\r\n";
    }

    // POST 데이터 처리
    if (strcmp(method, "POST") == 0 && post_data != NULL) {
        request_packet += "Content-Type: application/x-www-form-urlencoded\r\n";
        request_packet += "Content-Length: " + std::to_string(strlen(post_data)) + "\r\n";
    }
    request_packet += "Connection: close\r\n\r\n";  // header 끝

    // body 추가
    if (strcmp(method, "POST") == 0 && post_data != NULL) {
        request_packet += post_data;
    }

    // 데이터 전송
    if (send(client_sock, request_packet.c_str(), (int)request_packet.length(), 0) == SOCKET_ERROR) {
        // 전송 실패
        closesocket(client_sock);
        WSACleanup();
        Log(hDlg, "[ERROR] 데이터 전송 실패");
        return "Error: Send Failed";
    }

    Log(hDlg, "[NETWORK] 데이터 전송 완료 (%d bytes). 응답 대기 중...", request_packet.length());

    // 응답 수신
    char buffer[MAX_REQ_SIZE];
    std::string response_raw;
    int recv_len;

    // 더이상 데이터가 없을 때까지 읽음
    while ((recv_len = recv(client_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[recv_len] = '\0';
        response_raw += buffer;
    }

    Log(hDlg, "[NETWORK] 응답 수신 완료 (%d bytes). 연결 종료.", response_raw.length());

    closesocket(client_sock);
    WSACleanup();

    // 응답 파싱: Set-Cookie 문자열 찾아서 저장(있으면)
    size_t cookie_pos = response_raw.find("Set-Cookie: ");
    if (cookie_pos != std::string::npos) {
        size_t start = cookie_pos + 12; // Set-Cookie: 길이만큼 뒤
        size_t end = response_raw.find("\r\n", start);
        if (end != std::string::npos) {
            // 새 쿠키로 덮어쓰기
            g_cookie_storage = response_raw.substr(start, end - start);
        }
    }

    // header 제거하고 body만 반환
    // HTTP 프로토콜에서 header, body는 (\r\n\r\n)으로 구분됨
    size_t header_end = response_raw.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        return response_raw.substr(header_end + 4);
    }

    return response_raw;
}

/*
* 수신 문자열 변환 유틸리티 함수
* > 입력값: 변환시킬 char 자료형의 문자열
* > 반환값: 변환된 w_char 자료형의 문자열
* - 서버로부터 받은 데이터의 인코딩 형태를 파악해서 최종적으로 WebView가 이해할 수 있는 유니코드 형태(wchar_t)로 반환
* - 서버: 네이티브 C 프로그램. 1바이트 char 자료형(ANSI: EUC-KR 사용)으로 정보 송신
* - 클라이언트의 WebView: 무조건 2바이트 유니코드 형태(UTF-8)의 자료만 사용
* - 그러므로 클라이언트 프로그램이 임베딩된 웹브라우저에 화면을 출력하기 전 수신한 자료 형태를 검사하고, 필요시 변환해야 함
*/
std::wstring string_to_wstring(const std::string& s) {
    // 빈 문자열
    if (s.empty()) return std::wstring();

    // 1차 시도: UTF-8로 인코딩(엄격하게 검사)
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.length(), NULL, 0);
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.length(), &buf[0], len);
        buf[len] = L'\0';
        return std::wstring(&buf[0]);
    }

    // 2차 시도: ANSI(EUC-KR)로 인코딩
    int len_ansi = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.length(), NULL, 0);
    if (len_ansi > 0) {
        std::vector<wchar_t> buf(len_ansi + 1);
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.length(), &buf[0], len_ansi);
        buf[len_ansi] = L'\0';
        return std::wstring(&buf[0]);
    }

    // 변환 실패(입력값이 있는 상태에서 문자열의 길이 측정 실패 시)
    return L"Conversion Error";
}

/*
* 송신 문자열 변환 전 디코딩 함수
* > 입력값: 임베딩된 웹브라우저로부터 받은 퍼센트 인코딩된 문자열
* > 반환값: 실제 바이트 문자열
* - 웹브라우저로부터 받은 데이터 해석.
* - 이중 인코딩으로 인한 파일 깨짐 문제 방지
*/
std::string url_decode(const std::string& raw) {
    std::string ret;    // 바이트 문자열 변수
    int ii;               // 16진수 문자 변환에 사용되는 임시 변수
    for (size_t i = 0; i < raw.length(); i++) {
        if (raw[i] == '%') {
            // % 뒤의 16진수 두자리 숫자로 반환
            if (sscanf(raw.substr(i + 1, 2).c_str(), "%x", &ii) != EOF) {
                ret += static_cast<char>(ii);
                i += 2;
            }
        }
        else if (raw[i] == '+') {
            ret += ' '; // URL에서 공백은 +로
        }
        else {
            ret += raw[i];
        }
    }
    return ret;
}

/*
* 송신 문자열 변환 유틸리티 함수
* > 입력값: 변환시킬 w_char 자료형의 문자열
* > 출력값: 변환된 char 자료형의 문자열
* - 서버로 보낼 문자열 데이터의 인코딩 형태를 UTF-8에서 ANSI(EUC-KR)로 변환
* - 네이티브 C 프로그램 서버는 EUC-KR을 사용, 클라이언트는 유니코드만 인식하는 WebView2을 사용하기 위해 UTF-8 사용
* - 서버측에서 정상적으로 값을 이용할 수 있도록 인코딩 값을 변환해줌
*/
std::string utf8_to_ansi(const std::string& utf8) {
    if (utf8.empty())
        return "";

    // 전체 과정
    // UTF-8 -> Unicode(WideChar) -> ANSI(Char)

    // UTF-8 -> ANSI
    int len_wide = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (len_wide == 0)
        return "";
    std::vector<wchar_t> wbuf(len_wide);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wbuf[0], len_wide);

    // WideChar -> ANSI(CP_ACP)
    int len_ansi = WideCharToMultiByte(CP_ACP, 0, &wbuf[0], -1, NULL, 0, NULL, NULL);
    if (len_ansi == 0)
        return "";
    std::vector<char> abuf(len_ansi);
    WideCharToMultiByte(CP_ACP, 0, &wbuf[0], -1, &abuf[0], len_ansi, NULL, NULL);

    return std::string(&abuf[0]);
}

// 다이얼로그 프로시저: 윈도우 메시지 처리
INT_PTR CALLBACK DlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        // 초기화
    case WM_INITDIALOG: {
        // 초기 UI 설정
        // g_current_uri에 저장된 주소를 uri 입력창에 삽입
        std::wstring w_start_uri = string_to_wstring(g_current_uri);
        SetDlgItemText(hDlg, IDC_EDIT_URI, w_start_uri.c_str());

        // ip 주소 기본값 설정
        SetDlgItemText(hDlg, IDC_EDIT_IP, L"127.0.0.1");

        // 쿠키 불러오기(쿠키값은 전역변수(g_cookie_storage)에 저장됨)
        LoadCookieFromFile();

        // WebView2 환경 생성(비동기 Callback 방식)
        // 브라우저 엔진 작동
        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [hDlg](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    // 엔진 작동 완료
                    if (FAILED(result) || env == nullptr) {
                        MessageBox(hDlg, L"WebView2 Init Failed", L"Error", MB_ICONERROR);
                        return result;
                    }

                    // WebView 컨트롤러 생성
                    env->CreateCoreWebView2Controller(hDlg,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [hDlg](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                // 컨트롤러 생성 확인
                                if (FAILED(result) || controller == nullptr) return result;

                                webview_controller = controller;
                                // 컨트롤러에게 WebView 화면 객체를 요청
                                webview_controller->get_CoreWebView2(&webview);

                                // WebView 영역 크기 조정 및 표시
                                HWND h_group = GetDlgItem(hDlg, IDC_STATIC_VIEW);
                                if (h_group) {
                                    // 그룹 박스 크기에 맞춰서 창 크기 조절
                                    RECT bounds;
                                    GetWindowRect(h_group, &bounds);
                                    MapWindowPoints(NULL, hDlg, (LPPOINT)&bounds, 2);
                                    bounds.top += 15; bounds.left += 5;
                                    bounds.right -= 5; bounds.bottom -= 5;
                                    webview_controller->put_Bounds(bounds);
                                    SetWindowPos(h_group, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                                    // 화면에 보이기
                                    webview_controller->put_IsVisible(TRUE);

                                    // HTML 문서에 JS 스크립트 주입 및 강제 실행
                                    // - 순수 HTML 문서를 감시하면서 a태그 클릭, form 제출 이벤트를 하이재킹 & 가져온 데이터를 클라이언트 프로그램에서 처리
                                    std::wstring injection_script = LR"(
                                            // 페이지 로드 완료 시 생성
                                            document.addEventListener('DOMContentLoaded', () => {

                                                // 클릭 이벤트 가로채기(GET 요청용)
                                                document.body.addEventListener('click', (e) => {
                                                    // 클릭된 요소가 a 태그이거나 a 태그 안에 있는 경우를 검사
                                                    let target = e.target.closest('a');
         
                                                    // a 태그이고 href가 있다면
                                                    if (target && target.href) {
                                                        e.preventDefault(); // WebView의 기본 이동 막기
            
                                                        // href 속성값 추출
                                                        // 상대 경로 파싱
                                                        let path = target.getAttribute('href');
        
                                                        // 클라이언트 프로그램으로 전달
                                                        window.chrome.webview.postMessage('GET:' + path);
                                                    }
                                                });

                                                // 폼 제출 이벤트 가로채기(POST 요청용)
                                                document.body.addEventListener('submit', (e) => {
                                                    let form = e.target;
                                                    e.preventDefault(); // WebView의 기본 이동 막기
        
                                                    // 폼 데이터 -> 쿼리 스트링 변환
                                                    let formData = new FormData(form);
                                                    let params = new URLSearchParams(formData);
                                                    let queryString = params.toString();

                                                    // action 속성 가져오기
                                                    let action = form.getAttribute('action') || window.location.pathname;

                                                    // 클라이언트 프로그램으로 전달
                                                    window.chrome.webview.postMessage('POST:' + action + ':' + queryString);
                                                });
                                            });
                                        )";

                                    // 모든 페이지 로딩 이후 직전에 생성한 인젝션 스크립트를 자동 실행될 수 있도록 등록
                                    webview->AddScriptToExecuteOnDocumentCreated(injection_script.c_str(), nullptr);
                                }

                                // JS -> C++ 통신
                                // window.chrome.webview.postMessage()를 통해 보낸 데이터를 여기서 수신함(= 페이지에서 postMessage() 사용시 호출)
                                EventRegistrationToken token;
                                webview->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [hDlg](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT {
                                            // 메시지 수신 시 실행 로직들
                                            LPWSTR message_w;
                                            args->TryGetWebMessageAsString(&message_w);

                                            if (message_w) {
                                                // 유니코드 -> std::string 변환
                                                char message_c[MAX_REQ_SIZE];
                                                WideCharToMultiByte(CP_UTF8, 0, message_w, -1, message_c, MAX_REQ_SIZE, NULL, NULL);
                                                std::string msg = message_c;

                                                // 프로토콜(METHOD:URI:DATA) 파싱
                                                std::string method, uri, data = "";
                                                size_t first_colon = msg.find(':');

                                                if (first_colon != std::string::npos) {
                                                    method = msg.substr(0, first_colon);

                                                    if (method == "POST") {
                                                        size_t second_colon = msg.find(':', first_colon + 1);

                                                        if (second_colon != std::string::npos) {
                                                            uri = msg.substr(first_colon + 1, second_colon - (first_colon + 1));
                                                            std::string raw_encoded_data = msg.substr(second_colon + 1);

                                                            // 데이터 흐름: URL Decode -> UTF-8 데이터 ANSI로 변환 -> Server send()
                                                            // 받은 문자열 디코딩
                                                            std::string decoded_utf8 = url_decode(raw_encoded_data);
                                                            // UTF-8 -> ANSI 변환 후 "저장"
                                                            data = utf8_to_ansi(decoded_utf8);
                                                        }
                                                    }
                                                    else {  // GET 방식
                                                        uri = msg.substr(first_colon + 1);
                                                    }

                                                    // 링크 클릭 등의 이벤트로 이동할 때도 백그라운드 스레드 시작
                                                    // (내부에서 주소창 업데이트 + g_current_uri 갱신 자동 수행)
                                                    StartRequestThread(hDlg, method.c_str(), uri.c_str(), data.c_str());
                                                }
                                                CoTaskMemFree(message_w);
                                            }
                                            return S_OK;
                                        }).Get(), &token);

                                // 초기 화면 로딩
                                webview->NavigateToString(L"<h1>클라이언트 준비!</h1><p>연결 준비 완료.</p>");
                                return S_OK;
                            }).Get());  // 컨트롤러 콜백 끝
                    return S_OK;
                }).Get());  // 엔진 콜백 끝

        if (FAILED(hr)) {
            MessageBox(hDlg, L"WebView2 초기화 실패", L"Error", MB_ICONERROR);
        }
        return (INT_PTR)TRUE;
    }
                      // 스레드에서 작업이 끝났을 경우 화면 갱신
    case WM_UPDATE_WEBVIEW: {
        std::string* received_html = (std::string*)lParam;

        if (webview != nullptr && received_html != nullptr) {
            // 수신된 HTML 문서를 화면에 출력할 때 사용되는 유니코드 형태로 변환
            std::wstring converted_html = string_to_wstring(*received_html);
            webview->NavigateToString(converted_html.c_str());
        }

        if (received_html) delete received_html;    // 메모리 해제

        // 로딩 완료
        SetWindowText(hDlg, L"클라이언트 - 준비"); // 제목 변경
        SetCursor(LoadCursor(NULL, IDC_ARROW)); // 커서 복구
        return (INT_PTR)TRUE;
    }
                          // 버튼 이벤트 처리
    case WM_COMMAND: {
        // GO 버튼: 입력창의 내용을 읽어서 이동
        if (LOWORD(wParam) == IDC_BTN_GO) {
            wchar_t w_uri[256];
            GetDlgItemText(hDlg, IDC_EDIT_URI, w_uri, 256);

            char c_uri[256];
            WideCharToMultiByte(CP_UTF8, 0, w_uri, -1, c_uri, 256, NULL, NULL);

            SetWindowText(hDlg, L"클라이언트 - 로딩중..."); // 제목 변경
            SetCursor(LoadCursor(NULL, IDC_WAIT)); // 모래시계 커서

            // 입력한 주소로 이동 (g_current_uri 갱신됨)
            StartRequestThread(hDlg, "GET", c_uri, "");
        }
        // Refresh 버튼: 기억해둔 g_current_uri를 사용하여 다시 로드
        else if (LOWORD(wParam) == IDC_BTN_REFRESH) {
            // 입력창을 읽지 않고, 변수에 저장된 '현재 주소' 사용

            SetWindowText(hDlg, L"클라이언트 - 로딩중..."); // 제목 변경
            SetCursor(LoadCursor(NULL, IDC_WAIT)); // 모래시계 커서

            StartRequestThread(hDlg, "GET", g_current_uri.c_str(), "");
        }
        // Cookie 버튼: 저장중인 쿠키 목록을 메시지박스로 보여줌
        else if (LOWORD(wParam) == IDC_BTN_COOKIE) {
            if (g_cookie_storage.empty()) {
                MessageBox(hDlg, L"현재 저장된 쿠키가 없습니다.", L"Cookie Status", MB_ICONINFORMATION);
            }
            else {
                std::wstring w_cookie = string_to_wstring(g_cookie_storage);
                std::wstring msg = L"저장된 쿠키 값:\n\n" + w_cookie;
                MessageBox(hDlg, msg.c_str(), L"Current Cookie", MB_ICONINFORMATION);
            }
            return (INT_PTR)TRUE;
        }
        // 앞으로 가기
        else if (LOWORD(wParam) == IDC_BTN_FORWARD) {
            // 목록이 비어있지 않다면
            if (!g_forward_history.empty()) {
                // 현재 페이지를 '뒤로 가기' 목록에 넣음
                g_back_history.push_back(g_current_uri);

                // 앞으로 가기 목록의 가장 최근(맨 뒤) 값을 꺼냄
                std::string next = g_forward_history.back(); // 값 확인
                g_forward_history.pop_back();                // 값 삭제

                // 현재 주소 갱신
                g_current_uri = next;

                // 이동
                StartRequestThread(hDlg, "GET", next.c_str(), "", true);
            }
        }
        // 뒤로가기 버튼
        else if (LOWORD(wParam) == IDC_BTN_BACK) {
            // 목록이 비어있지 않다면 (!empty)
            if (!g_back_history.empty()) {
                // 현재 페이지를 '앞으로 가기' 목록에 넣음
                g_forward_history.push_back(g_current_uri);

                // 뒤로가기 목록의 가장 최근(맨 뒤) 값을 꺼냄
                std::string prev = g_back_history.back(); // 값 확인
                g_back_history.pop_back();                // 값 삭제(꺼내기)

                // 현재 주소 갱신
                g_current_uri = prev;

                // 페이지 이동 is_navigating = true로 설정: 꼬임 방지
                StartRequestThread(hDlg, "GET", prev.c_str(), "", true);
            }
        }
        // 다이얼로그 종료
        else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, 0);
            return (INT_PTR)TRUE;
        }
        break;
    }
                   // 클라이언트 창 크기 조절
    case WM_SIZE: {
        RECT bounds;
        GetClientRect(hDlg, &bounds);    // 다이얼로그 전체 내부 크기

        const int TOP_UI_HEIGHT = 50;    // 상단 네비게이션 바 높이
        const int LOG_AREA_HEIGHT = 120; // 하단 로그 영역 전체 높이

        // 1. 그룹박스(테두리) 위치 계산
        RECT rcGroup;
        rcGroup.left = 10;
        rcGroup.top = TOP_UI_HEIGHT;
        rcGroup.right = bounds.right - 10;
        rcGroup.bottom = bounds.bottom - LOG_AREA_HEIGHT;

        // 그룹박스 이동
        HWND hGroup = GetDlgItem(hDlg, IDC_STATIC_VIEW);
        if (hGroup) {
            SetWindowPos(hGroup, NULL,
                rcGroup.left, rcGroup.top,
                rcGroup.right - rcGroup.left, rcGroup.bottom - rcGroup.top,
                SWP_NOZORDER);
        }

        // 그룹박스 안쪽으로 패딩 적용
        if (webview_controller != nullptr) {
            RECT rcWeb = rcGroup;
            rcWeb.top += 15;    // 제목 공간 확보
            rcWeb.left += 5;
            rcWeb.right -= 5;
            rcWeb.bottom -= 5;
            webview_controller->put_Bounds(rcWeb);
        }

        // 하단 로그 영역 계산
        // 리스트박스 위치
        int listTop = bounds.bottom - LOG_AREA_HEIGHT + 15; // 라벨 높이(15)만큼 띄움
        int listHeight = LOG_AREA_HEIGHT - 25;              // 하단 여백(10) 확보

        // 로그 라벨 이동 ("System Log:")
        HWND hLogLabel = GetDlgItem(hDlg, IDC_STATIC_LOG);
        if (hLogLabel) {
            // 리스트박스 바로 위에 위치시킴
            SetWindowPos(hLogLabel, NULL,
                10, listTop - 15, // 리스트박스보다 15픽셀 위
                100, 15,          // 너비 100, 높이 15
                SWP_NOZORDER);
        }

        // 리스트박스 이동
        HWND hList = GetDlgItem(hDlg, IDC_LIST_LOG);
        if (hList) {
            SetWindowPos(hList, NULL,
                10, listTop,
                bounds.right - 20, listHeight,
                SWP_NOZORDER);
        }

        // 화면 강제 다시 그리기 (깨짐 현상 해결)
        // WebView 주변의 잔상을 지우기 위해 다이얼로그 전체를 다시 그리도록 요청
        // TRUE: 배경색도 다시 칠함 (잔상 제거)
        InvalidateRect(hDlg, NULL, TRUE);

        return (INT_PTR)TRUE;
    }
                // 프로그램 종료
    case WM_DESTROY: {
        SaveCookieToFile(); // 쿠키 저장
        break;
    }
                   // 로그 출력 메시지 처리
    case WM_APP_LOG: {
        char* msg = (char*)lParam;

        if (msg) {
            // 리스트박스 핸들 가져오기
            HWND hList = GetDlgItem(hDlg, IDC_LIST_LOG);

            // 문자열 변환
            std::wstring wmsg = string_to_wstring(msg);
            SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)wmsg.c_str());

            // 스크롤을 맨 아래로 이동시킴(자동스크롤)
            int count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
            SendMessage(hList, LB_SETCURSEL, count - 1, 0);
            SendMessage(hList, LB_SETCURSEL, -1, 0);

            delete[] msg;   // 메모리 할당 제거
        }
    }
    }
    return (INT_PTR)FALSE;
}

// --- main 함수 ---
int CALLBACK WinMain(_In_ HINSTANCE h_instance, _In_opt_ HINSTANCE h_prev_instance, _In_ LPSTR lp_cmd_line, _In_ int n_cmd_show) {
    // COM 라이브러리 초기화: WebView2 사용을 위해 필요함
    // 반환값 경고를 제거하기 위해 void로 타입캐스팅
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // 클라이언트 다이얼로그 실행
    DialogBox(h_instance, MAKEINTRESOURCE(IDD_MAIN_DIALOG), NULL, DlgProc);

    // 종료 절차: 종료 전 COM 해제
    CoUninitialize();
    return 0;
}