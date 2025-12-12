#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

	// 로그 콜백 함수 타입
	typedef void(__stdcall* HttpLogCallback)(const char* msg);

	// GUI 측에서 콜백 등록
	void http_set_log_callback(HttpLogCallback cb);

	// HTTP 서버 실행 (블로킹)
	int run_http_server(int port);

	// 서버 중지 요청 (STOP 버튼에서 호출)
	void http_server_request_stop(void);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H
