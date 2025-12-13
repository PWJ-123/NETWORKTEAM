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

#include "resource.h"

#pragma comment(lib, "ws2_32.lib")

#define MAX_REQ_SIZE 4096
#define WM_UPDATE_WEBVIEW (WM_USER + 1)

using namespace Microsoft::WRL;

// --- 전역 변수 ---
ComPtr<ICoreWebView2Controller> webview_controller;
ComPtr<ICoreWebView2> webview;

// [추가] 현재 보고 있는 페이지의 주소를 기억하는 변수 (기본값: /index.html)
std::string g_current_uri = "/index.html";

// --- 함수 선언 ---
std::string request_HTML(const char* method, const char* uri, const char* post_data);
std::wstring string_to_wstring(const std::string& s);
void StartRequestThread(HWND hDlg, const char* method, const char* uri, const char* body);

// 작업 스레드 파라미터 구조체
struct ThreadParam {
    char method[10] = { 0 };
    char uri[256] = { 0 };
    std::string body;
    HWND hDlg = NULL;
};

// 백그라운드 작업 스레드 함수
unsigned __stdcall RequestThreadFunc(void* arg) {
    ThreadParam* param = (ThreadParam*)arg;

    // 서버 요청 수행
    std::string html = request_HTML(param->method, param->uri, param->body.c_str());

    std::string meta_tag = "<head><meta charset=\"UTF-8\"></head>";
    std::string* final_content = new std::string(meta_tag + html);

    PostMessage(param->hDlg, WM_UPDATE_WEBVIEW, 0, (LPARAM)final_content);

    delete param;
    return 0;
}

// [핵심 수정] 스레드 시작 및 "현재 상태 동기화" 헬퍼 함수
void StartRequestThread(HWND hDlg, const char* method, const char* uri, const char* body) {
    // 1. 현재 주소 변수(g_current_uri) 업데이트
    //    이제 어디로 이동하든 이 변수에 마지막 주소가 남습니다.
    if (uri != NULL) {
        g_current_uri = uri;
    }

    // 2. URL 입력창(GUI)도 같이 업데이트
    //    링크를 클릭해서 이동했을 때도 주소창이 바뀌도록 만듭니다.
    std::wstring w_uri = string_to_wstring(g_current_uri);
    SetDlgItemText(hDlg, IDC_EDIT_URI, w_uri.c_str());

    // 3. 스레드 파라미터 생성
    ThreadParam* param = new ThreadParam;
    strncpy(param->method, method, 9);
    strncpy(param->uri, uri, 255);
    if (body) param->body = body;

    param->hDlg = hDlg;

    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, RequestThreadFunc, param, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

// 소켓 통신 함수
std::string request_HTML(const char* method, const char* uri, const char* post_data) {
    WSADATA wsaData;
    SOCKET client_sock;
    SOCKADDR_IN server_addr;
    const char* SERVER_IP = "127.0.0.1";
    int port = 8080;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return "Error: WSAStartup Failed";

    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock == INVALID_SOCKET) {
        WSACleanup();
        return "Error: Socket Creation Failed";
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
    server_addr.sin_port = htons(port);

    if (connect(client_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        closesocket(client_sock);
        WSACleanup();
        return "<html><body><h1>Connection Failed</h1><p>Server is likely offline.</p></body></html>";
    }

    std::string request_packet;
    std::string str_uri = (uri[0] == '/') ? uri : '/' + std::string(uri);

    request_packet += std::string(method) + " " + str_uri + " HTTP/1.1\r\n";
    request_packet += "Host: " + std::string(SERVER_IP) + "\r\n";

    if (strcmp(method, "POST") == 0 && post_data != NULL) {
        request_packet += "Content-Type: application/x-www-form-urlencoded\r\n";
        request_packet += "Content-Length: " + std::to_string(strlen(post_data)) + "\r\n";
    }
    request_packet += "Connection: close\r\n\r\n";

    if (strcmp(method, "POST") == 0 && post_data != NULL) {
        request_packet += post_data;
    }

    if (send(client_sock, request_packet.c_str(), (int)request_packet.length(), 0) == SOCKET_ERROR) {
        closesocket(client_sock);
        WSACleanup();
        return "Error: Send Failed";
    }

    char buffer[MAX_REQ_SIZE];
    std::string response_raw;
    int recv_len;

    while ((recv_len = recv(client_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[recv_len] = '\0';
        response_raw += buffer;
    }

    closesocket(client_sock);
    WSACleanup();

    size_t header_end = response_raw.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        return response_raw.substr(header_end + 4);
    }

    return response_raw;
}

// 문자열 변환 유틸리티
std::wstring string_to_wstring(const std::string& s) {
    if (s.empty()) return std::wstring();

    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), NULL, 0);
    if (len > 0) {
        std::vector<wchar_t> buf(len + 1);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), &buf[0], len);
        buf[len] = L'\0';
        return std::wstring(&buf[0]);
    }

    int len_ansi = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.length(), NULL, 0);
    if (len_ansi > 0) {
        std::vector<wchar_t> buf(len_ansi + 1);
        MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.length(), &buf[0], len_ansi);
        buf[len_ansi] = L'\0';
        return std::wstring(&buf[0]);
    }

    return L"Conversion Error";
}

// 다이얼로그 프로시저
INT_PTR CALLBACK DlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {

    case WM_INITDIALOG:
    {
        // 초기값 설정 시에도 전역 변수 사용
        std::wstring w_start_uri = string_to_wstring(g_current_uri);
        SetDlgItemText(hDlg, IDC_EDIT_URI, w_start_uri.c_str());

        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [hDlg](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(result) || env == nullptr) {
                        MessageBox(hDlg, L"WebView2 Init Failed", L"Error", MB_ICONERROR);
                        return result;
                    }

                    env->CreateCoreWebView2Controller(hDlg,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [hDlg](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(result) || controller == nullptr) return result;

                                webview_controller = controller;
                                webview_controller->get_CoreWebView2(&webview);

                                HWND hGroup = GetDlgItem(hDlg, IDC_STATIC_VIEW);
                                if (hGroup) {
                                    RECT bounds;
                                    GetWindowRect(hGroup, &bounds);
                                    MapWindowPoints(NULL, hDlg, (LPPOINT)&bounds, 2);
                                    bounds.top += 15; bounds.left += 5;
                                    bounds.right -= 5; bounds.bottom -= 5;
                                    webview_controller->put_Bounds(bounds);
                                    SetWindowPos(hGroup, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                                    webview_controller->put_IsVisible(TRUE);
                                }

                                EventRegistrationToken token;
                                webview->add_WebMessageReceived(
                                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [hDlg](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT {
                                            LPWSTR message_w;
                                            args->TryGetWebMessageAsString(&message_w);

                                            if (message_w) {
                                                char message_c[MAX_REQ_SIZE];
                                                WideCharToMultiByte(CP_UTF8, 0, message_w, -1, message_c, MAX_REQ_SIZE, NULL, NULL);
                                                std::string msg = message_c;

                                                std::string method, uri, data = "";
                                                size_t first_colon = msg.find(':');

                                                if (first_colon != std::string::npos) {
                                                    method = msg.substr(0, first_colon);

                                                    if (method == "POST") {
                                                        size_t second_colon = msg.find(':', first_colon + 1);
                                                        if (second_colon != std::string::npos) {
                                                            uri = msg.substr(first_colon + 1, second_colon - (first_colon + 1));
                                                            data = msg.substr(second_colon);
                                                        }
                                                    }
                                                    else {
                                                        uri = msg.substr(first_colon + 1);
                                                    }

                                                    // 링크 클릭 등의 이벤트로 이동할 때도 StartRequestThread 호출
                                                    // (내부에서 주소창 업데이트 + g_current_uri 갱신 자동 수행)
                                                    StartRequestThread(hDlg, method.c_str(), uri.c_str(), data.c_str());
                                                }
                                                CoTaskMemFree(message_w);
                                            }
                                            return S_OK;
                                        }).Get(), &token);

                                webview->NavigateToString(L"<h1>Ready</h1><p>Client initialized.</p>");
                                return S_OK;
                            }).Get());
                    return S_OK;
                }).Get());

        if (FAILED(hr)) {
            MessageBox(hDlg, L"Failed to create WebView2 environment", L"Error", MB_ICONERROR);
        }
        return (INT_PTR)TRUE;
    }

    case WM_UPDATE_WEBVIEW:
    {
        std::string* received_html = (std::string*)lParam;
        if (webview != nullptr && received_html != nullptr) {
            std::wstring converted_html = string_to_wstring(*received_html);
            webview->NavigateToString(converted_html.c_str());
        }
        if (received_html) delete received_html;
        return (INT_PTR)TRUE;
    }

    case WM_COMMAND:
        // [CASE 1] GO 버튼: 입력창의 내용을 읽어서 이동
        if (LOWORD(wParam) == IDC_BTN_GO) {
            wchar_t w_uri[256];
            GetDlgItemText(hDlg, IDC_EDIT_URI, w_uri, 256);

            char c_uri[256];
            WideCharToMultiByte(CP_UTF8, 0, w_uri, -1, c_uri, 256, NULL, NULL);

            // 입력한 주소로 이동 (g_current_uri 갱신됨)
            StartRequestThread(hDlg, "GET", c_uri, "");
        }
        // [CASE 2] Refresh 버튼: 기억해둔 g_current_uri를 사용하여 다시 로드
        else if (LOWORD(wParam) == IDC_BTN_REFRESH) {
            // 입력창을 읽지 않고, 변수에 저장된 '현재 주소' 사용
            StartRequestThread(hDlg, "GET", g_current_uri.c_str(), "");
        }
        else if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, 0);
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

int CALLBACK WinMain(_In_ HINSTANCE h_instance, _In_opt_ HINSTANCE h_prev_instance, _In_ LPSTR lp_cmd_line, _In_ int n_cmd_show) {
    (void)CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DialogBox(h_instance, MAKEINTRESOURCE(IDD_MAIN_DIALOG), NULL, DlgProc);
    CoUninitialize();
    return 0;
}