#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <WS2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define SERVER_IP "127.0.0.1"
#define MAX_REQ_SIZE 4096

// --- 함수 정의 ---

// 에러 핸들러(서버와 같음)
void error_handling(const char* message) {
	fputs(message, stderr);
	fputc('\n', stderr);
	exit(1);
}

// --- main() ---
void main() {
	// 소켓 변수 선언
	WSADATA wsaData;
	SOCKET server_sock, client_sock;
	SOCKADDR_IN server_addr;
	int port = 8080;

	printf("[DEBUG] WSAStartup()...	");
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		error_handling("WSAStartup() error");
	}
	printf("success!\n");

	while (true) {
		printf("[DEBUG] socket()...	");
		client_sock = socket(AF_INET, SOCK_STREAM, 0);
		if (client_sock == INVALID_SOCKET)
			error_handling("socket() error");
		printf("success!\n");

		memset(&server_addr, 0, sizeof(server_addr));
		server_addr.sin_family = AF_INET;
		inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);
		server_addr.sin_port = htons(port);

		// --- 사용자 입력 처리 ---
		char request_raw[MAX_REQ_SIZE];			// 송신할 데이터 원본
		char request_packet[MAX_REQ_SIZE];		// 가공된 송신 데이터
		char response_raw[MAX_REQ_SIZE];		// 수신받은 데이터 원본
		char* response_body;							// 수신 데이터 중 바디 부분
		int request_len;
		int response_len;
		int response_total_len = 0;
		int response_read_len = 0;

		printf("----------------------------------------\n\n");
		printf("[INFO] 사용자 입력 | 예시) /index.html | 종료: quit\n> ");
		fgets(request_raw, sizeof(request_raw), stdin);
		request_raw[strcspn(request_raw, "\n")] = 0;	// 종단 문자 제거
		request_len = (int)strlen(request_raw);

		if (strcmp(request_raw, "quit") == 0) {
			printf("[DEBUG/quit]  클라이언트 종료...\n");
			break;
		}
		printf("[DEBUG] 입력값: %s\n", request_raw);

		// --- 서버 연결 ---
		printf("[DEBUG] connect()...	");
		if (connect(client_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
			error_handling("connect() error");
		printf("success!\n");

		printf("\n[INFO] 서버 연결 성공\n");
		printf("[INFO] 서버 주소: %s | 서버 포트: %d\n\n", SERVER_IP, port);

		// --- 전송 데이터 처리 ---
		
		sprintf(request_packet,
			"GET %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"\r\n",
			request_raw, SERVER_IP);

		// --- 데이터 송신 ---
		printf("\n[INFO] 서버로 %d바이트의 정보를 전송...	", request_len);
		if (send(client_sock, (char*)&request_packet, (int)strlen(request_packet), 0) == SOCKET_ERROR)
			error_handling("send() error");
		printf("전송 완료.\n");
		

		// --- 데이터 수신 ---
		/*
		1. 서버로부터 모든 데이터 수신 받기
		- 반복문의 조건문으로 response_read_len에 raw 값을 계속 받아서
		*/
		printf("\n[DEBUG] 데이터 수신 시작...\n");
		while ((response_read_len = recv(client_sock, (char*)&response_raw[response_total_len], sizeof(response_raw) - response_total_len - 1, 0)) > 0)
			response_total_len += response_read_len;

		response_raw[response_total_len] = '\0';
		
		// 수신받은 데이터가 없다면...
		if (response_total_len <= 0){
			error_handling("recv() error");
			closesocket(client_sock);
			continue;
		}

		// 바디 데이터를 담을 변수에 데이터 삽입
		response_body = strstr(response_raw, "\r\n\r\n");

		if (response_body != NULL) {
			response_body += 4;

			printf("[INFO] %dbyte의 응답 도착.\n", response_total_len);

			FILE* fp = fopen("response_view.html", "wb");
			if (fp != NULL) {
				int header_len = response_body - response_raw;
				int body_len = response_total_len - header_len;
				fwrite(response_body, 1, body_len, fp);
				fclose(fp);

				printf("[INFO] 기본 브라우저에서 결과 출력 중...\n");
				system("start response_view.html");
			}
		}
		else {
			printf("[INFO/WRAN] 올바르지 않은 응답\n");
			printf("%s\n", response_raw);
		}
		closesocket(client_sock);
		printf("----------------------------------------\n");
	}

	WSACleanup();
	return;
}