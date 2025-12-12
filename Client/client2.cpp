//#define WIN32_LEAN_AND_MEAN			// 윈도우 헤더 충돌 방지
#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <stdlib.h>

#include <WebView2.h>
#include <wrl.h>				// COM 스마트 포인터용

#pragma comment(lib, "ws2_32.lib")

#define MAX_REQ_SIZE 4096

using namespace Microsoft::WRL;

/*
URL 요청 함수 ver2: GET / POST 요청 처리 구분 추가
- 매개변수:
	- method: GET or POST
	- uri: URI 주소
	- post_data: post 요청일 경우의 바디 정보(GET일 경우 null or "")
- 반환값: HTML 본문 전체(String)
*/
std::string request_HTML(const char* method, const char* uri, const char* post_data) {
	// 변수 선언
	WSADATA wsaData;
	SOCKET client_sock;
	SOCKADDR_IN server_addr;
	const char* SERVER_IP = "127.0.0.1";
	// const int MAX_REQ_SIZE = 4096;
	int port = 8080;

	std::string resultHTML = "";

	// WSAStartup()
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return "Exception in WSAStartup()";

	// socket()
	client_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (client_sock == INVALID_SOCKET)
		return "Exception in socket()";

	// server_addr settings
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
	server_addr.sin_port = htons(port);

	if (connect(client_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
		closesocket(client_sock);
		return "<html><body><h1>Connection Failed.</h1><p>Check the server!</p></body></html>";
	}

	// 요청 패킷 변수 선언
	// char request_packet[MAX_REQ_SIZE];
	std::string request_packet;
	std::string str_uri = (uri[0] == '/') ? uri : '/' + std::string(uri);		// 클라이언트가 '/'를 빼먹었을 경우 자동으로 채워줌

	// 요청 패킷 생성
	request_packet += std::string(method) + " " + str_uri + " HTTP/1.1\r\n";
	request_packet += "Host: " + std::string(SERVER_IP) + "\r\n";

	// method가 POST일 경우 헤더 추가
	if (strcmp(method, "POST") == 0 && post_data != NULL) {
		request_packet += "Content-Type: application/x-www-form-urlencoded\r\n";

		// Content-Length 계산 및 추가
		std::string len_str = std::to_string(strlen(post_data));
		request_packet += "Content-Length: " + len_str + "\r\n";
	}

	request_packet += "Connnection: close\r\n";
	request_packet += "\r\n";

	// method가 POST일 경우 바디 추가
	if (strcmp(method, "POST") == 0 && post_data != NULL) {
		request_packet += post_data;
	}

	// 요청 전송 및 오류 처리
	if (send(client_sock, request_packet.c_str(), (int)request_packet.length(), 0) == SOCKET_ERROR) {
		closesocket(client_sock);
		WSACleanup();
		return "Send Error";
	}

	// 응답 수신
	char buffer[MAX_REQ_SIZE];
	std::string response_raw;
	int recv_len;

	// 수신 받은 값이 존재한다면 서버의 수신이 끝날 때 까지 반복
	while ((recv_len = recv(client_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
		buffer[recv_len] = '\0';
		response_raw += buffer;
	}

	closesocket(client_sock);
	WSACleanup();

	// 헤더 / 바디 분리

	size_t header_end = response_raw.find("\r\n\r\n");
	if (header_end != std::string::npos) {
		return response_raw.substr(header_end + 4);
	}

	return response_raw;	// 헤더 분리 실패 시 디버깅 용 전체 값 반환
}	// request_HTML END

// --- GUI / WebView2 ---

// 전역변수
// ComPtr: 스마트 포인터. 메모리 해제가 자동으로 됨. 
ComPtr<ICoreWebView2Controller> webview_controller;	// 브라우저 창의 크기 및 위치 관리
ComPtr<ICoreWebView2> webview;								// 브라우저 내용 제어
HWND h_edit;		// 주소 입력창 핸들
HWND h_button;	// 버튼 핸들


/*
--- 문자열 변환(string -> wstring) 함수 ---
- WebView2가 유니코드만 인식할 수 있기 때문에
  변환하는 과정을 거침
- 첫 시도에서는 UTF-8로 변환, 실패 시 ANSI규격 사용,
  실패 시 에러 메시지 문자열 반환
*/
std::wstring string_to_wstring(const std::string& s) {
	if (s.empty()) return std::wstring();	// 빈 문자열일 경우 바로 반환

	int len;

	// 1. 받은 문자열을 UTF-8로 인코딩
	len = MultiByteToWideChar(
		CP_UTF8,							// 코드 페이지(CP_UTF8: UTF-8 코드 페이지. 규격이 맞지 않으면 에러 발생)
		MB_ERR_INVALID_CHARS,				// dwFlags(UTF-8 변환 실패 시 0 반환)
		s.c_str(),							// 변환할 문자열 포인터
		(int)s.length(),					// 변환할 문자열 크기
		NULL,								// 변환된 문자열을 가지는 버퍼의 포인터(지금은 len에 크기를 대입할 것이기 때문에 0)
		0									// 변환된 문자열을 가지는 버퍼의 크기
	);

	// UTF-8 변환 성공
	if (len > 0) {
		// null terminator 공간
		std::vector<wchar_t> buf(len + 1);
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), &buf[0], len);
		buf[len] = L'\0';
		return std::wstring(&buf[0]);
	}
	// 변환 실패
	// 2. ANSI(EUC-KR)로 변환 시도
	else {
		int len_ansi = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.length(), NULL, 0);

		if (len_ansi > 0) {
			std::vector<wchar_t> buf(len_ansi + 1);
			MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.length(), &buf[0], len_ansi);
			buf[len_ansi] = L'\0';
			return std::wstring(&buf[0]);
		}
	}

	// 두 방법 모두 실패한 경우
	return L"Encoding Error";
}

/*
--- 윈도우 프로시저 함수 ---
- 창의 모든 이벤트 제어
*/
LRESULT CALLBACK wnd_proc(HWND h_wnd, UINT message, WPARAM w_param, LPARAM l_param) {
	switch (message) {
	case WM_CREATE:
		// 1. edit control 만들기
		h_edit = CreateWindow(L"EDIT", L"/index.html",	// 초기 입력값
			WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
			10, 10, 300, 25, h_wnd, NULL, NULL, NULL);

		// 2. button 만들기
		h_button = CreateWindow(L"BUTTON", L"GO",
			WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
			320, 10, 50, 25, h_wnd, (HMENU)1, NULL, NULL);

		// 3. WebView2 초기화
		// 비동기 방식이기 때문에 한 단계씩 명령을 해야 함

		// 1. 브라우저 환경 생성 요청
		CreateCoreWebView2EnvironmentWithOptions(nullptr, nullptr, nullptr,
			// 환경 준비 후 호출되는 함수
			Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
				[h_wnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {

					// 실패 확인(DLL이 없음 or COM 초기화 실패)
					if (FAILED(result) || env == nullptr) {
						wchar_t msg[256];
						swprintf_s(msg, 256, L"WebView2 초기화 실패.\nError Code: 0x%08X\n(x86/x64 설정 확인)", result);
						MessageBox(h_wnd, msg, L"Critical Error", MB_ICONERROR);
						return result;
					}
					// 2. 환경 준비 후, 제작한 윈도우 객체(h_wnd)에 브라우저를 임베딩 요청
					env->CreateCoreWebView2Controller(h_wnd,
						// 컨트롤러 연결이 끝나면 실행되는 호출되는 함수
						Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
							[h_wnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
								if (FAILED(result) || controller == nullptr) {
									MessageBox(h_wnd, L"WebView2 컨트롤러 생성 실패", L"오류", MB_ICONERROR);
									return result;
								}

								// 3. 임베딩 성공. 변수에 컨트롤러 저장 및 이용
								webview_controller = controller;
								webview_controller->get_CoreWebView2(&webview);

								// 브라우저 크기 조절
								RECT bounds;	// 초기 크기 설정
								GetClientRect(h_wnd, &bounds);
								bounds.top += 50;	// 상단의 입력창 공간 확보
								webview_controller->put_Bounds(bounds);

								// Javascript 이벤트 리스너
								EventRegistrationToken token;
								webview->add_WebMessageReceived(
									Callback<ICoreWebView2WebMessageReceivedEventHandler>(
										[h_wnd](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT {
											// 1. 페이지의 Javascript를 통해서 메시지 받기
											LPWSTR message_w;
											args->TryGetWebMessageAsString(&message_w);

											// 메시지가 있다면
											if (message_w) {
												// WideChar -> MultiByte 변환
												char message_c[MAX_REQ_SIZE];
												WideCharToMultiByte(
													CP_UTF8,
													0,
													message_w,
													-1,
													message_c,
													MAX_REQ_SIZE,
													NULL,
													NULL
												);
												std::string msg = message_c;

												// 2. 메시지 파싱
												std::string method, uri, data = "";

												size_t first_colon = msg.find(':');
												if (first_colon != std::string::npos) {
													method = msg.substr(0, first_colon);	// GET or POST

													if (method == "POST") {	// POST
														size_t second_colon = msg.find(':', first_colon + 1);
														if (second_colon != std::string::npos) {
															uri = msg.substr(first_colon + 1, second_colon - (first_colon + 1));
															data = msg.substr(second_colon);
														}
													}
													else {	// GET
														uri = msg.substr(first_colon + 1);
													}

													// 3. Winsock 요청 수행
													// request_HTML 함수 사용
													std::string html_content = request_HTML(method.c_str(), uri.c_str(), data.c_str());

													// 4. 결과 갱신
													std::wstring w_html = string_to_wstring(html_content);
													sender->NavigateToString(w_html.c_str());
												}
												CoTaskMemFree(message_w);	// 메모리 해제
											}
											return S_OK;
										}).Get(), &token);

								// 초기 화면(시작 페이지 출력)
								webview->NavigateToString(L"<h1>Ready to Connect</h1><p>Type URI and click GO</p>");

								return S_OK;

							}).Get());	// 두번째 콜백 함수 종료
					return S_OK;
				}).Get());	// 첫번째 콜백 함수 종료
		break;

	case WM_SIZE:
		if (webview_controller != nullptr) {
			RECT bounds;
			GetClientRect(h_wnd, &bounds);
			bounds.top += 50;	// 입력창 아래부터 브라우저 표시
			webview_controller->put_Bounds(bounds);
		}
		break;

	case WM_COMMAND:
		if (LOWORD(w_param) == 1) {
			try {
				// 1. 입력창에서 uri 읽기
				wchar_t w_uri[256];
				GetWindowText(h_edit, w_uri, 256);

				char c_uri[256];
				// request_HTML() 함수에 넣기 위해 char 형태로 변환
				WideCharToMultiByte(CP_UTF8, 0, w_uri, -1, c_uri, 256, NULL, NULL);

				// 2. Winsock 함수(http 요청) 호출
				std::string html_content;

				/*
				* --- 요청 방식 구분 ---
				*/
				// 주소창 입력값은 GET 방식으로 고정(추후 수정 예정)
				html_content = request_HTML("GET", c_uri, "");

				// HTML <head> 태그 내부에 인코딩 방법을 UTF-8로 설정하는 코드 삽입
				std::string meta_tag = "<head><meta charset=\"UTF-8\"></head>";
				std::string final_content = meta_tag + html_content;

				// 3. 결과를 WebView2에 주입
				if (webview != nullptr) {
					std::wstring converted_html = string_to_wstring(final_content);
					webview->NavigateToString(converted_html.c_str());
				}
				else
					MessageBox(h_wnd, L"WebView is not ready yet.", L"wait", MB_OK);
			}
			catch (const std::exception& e) { // C++ 예외 발생 시 메시지 박스 출력
				std::string err_msg = "Error: ";
				err_msg += e.what();
				MessageBoxA(h_wnd, err_msg.c_str(), "Exception Caught", MB_ICONERROR);
			}
			catch (...) { // 알 수 없는 에러 발생
				MessageBox(h_wnd, L"Unknown Error occurred.", L"Critical Error", MB_ICONERROR);
			}
		}
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		return DefWindowProc(h_wnd, message, w_param, l_param);
	}
	return 0;
}

// main
int CALLBACK WinMain(HINSTANCE h_instance, HINSTANCE h_prev_instance, LPSTR lp_cmd_line, int n_cmd_show) {
	// COM 라이브러리 초기화
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	// 윈도우 클래스 등록
	WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, wnd_proc, 0, 0, h_instance, NULL, NULL, (HBRUSH)(COLOR_WINDOW + 1), NULL, L"my_web_client", NULL };
	RegisterClassEx(&wc);

	// 윈도우 창 생성
	HWND h_wnd = CreateWindow(L"my_web_client", L"My Custom Web Client", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, h_instance, NULL);

	// 화면에 출력
	ShowWindow(h_wnd, n_cmd_show);
	UpdateWindow(h_wnd);

	MSG msg;
	// 메시지 받기 / 보내기 반복
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	// 종료 전 COM 라이브러리 해제
	CoUninitialize();

	return (int)msg.wParam;
}