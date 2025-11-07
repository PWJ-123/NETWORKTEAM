#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <time.h>

#include "common.h"

#define SERVER_PORT 8080			// 서버 포트
#define BUF_SIZE 256					// 전송 버퍼 사이즈

char* serverIp = (char*)"127.0.0.1";	// 테스트용 서버 IP(루프백 주소)

DWORD WINAPI recvThread(SOCKET clientSocket) {
	printf("[CLIENT/recvThread] 스레드 시작...\n");
	struct sockaddr_in peerAddr;
	int addrLen;
	char recvBuf[BUF_SIZE];
	int retval;

	while (TRUE) {
		addrLen = sizeof(peerAddr);
		retval = recvfrom(clientSocket, recvBuf, BUF_SIZE, 0, (struct sockaddr*)&peerAddr, &addrLen);

		if (retval == SOCKET_ERROR) {
			err_display("recvfrom()");
			break;
		}

		recvBuf[retval] = '\0';
		printf("[CLIENT/recvThread] 서버로부터 %d 바이트 수신 >> %s", retval, recvBuf);
	}

	printf("[CLIENT/recvThread] 스레드 종료...\n");
	return 0;
}

void main() {
	int retval;	// 함수 실행 결과를 저장할 변수

	// 스레드 관련 변수 선언
	HANDLE hThread;
	DWORD threadId;
	DWORD exitCode;

	// Winsock 초기화
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		err_display("WSAStartup()");
		return;
	}
	else {
		printf("[CLIENT] Winsock 초기화 성공.\n");
	}

	// 클라이언트 소켓(UDP version) 생성
	SOCKET clientSocket = socket(
		AF_INET, 			// 주소 체계(AF_INET == 2(TCP/IP)
		SOCK_DGRAM,			// 소켓 타입
		0					// 프로토콜
	);
	// 소켓 생성 검사
	if (clientSocket == INVALID_SOCKET) {
		err_display("socket()");
	}
	else {
		printf("[CLIENT] TCP 소켓 생성 성공.\n");
	}

	// 통신할 서버 관련 변수 설정
	struct sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	inet_pton(AF_INET, serverIp, &serverAddr.sin_addr);
	serverAddr.sin_port = htons(SERVER_PORT);

	// 서버의 sendto() 응답을 받기 위해 클라이언트 측도 바인드를 시행
	struct sockaddr_in clientAddr;
	memset(&clientAddr, 0, sizeof(clientAddr));
	clientAddr.sin_family = AF_INET;
	clientAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	clientAddr.sin_port = htons(0);		// OS에서 임의로 결정

	retval = bind(clientSocket, (struct sockaddr*)&clientAddr, sizeof(clientAddr));
	if (retval == SOCKET_ERROR) {
		err_display("bind()");
		return;
	}
	printf("[CLINET] 소켓 바인드 성공.\n");

	// 스레드 생성 코드
	hThread = CreateThread(
		NULL,					// 보안 속성
		0,						// 스택 크기 (기본값)
		recvThread,         // 스레드 함수
		clientSocket,		// 파라미터
		0,						// 플래그
		&threadId			// 스레드 ID
	);
	

	char buf[BUF_SIZE];
	printf("========================================================\n");
	while (TRUE) {
		Sleep(1000);
		printf("[CLIENT] 값을 입력하세요. >> ");
		fgets(buf, sizeof(buf), stdin);

		retval = sendto(clientSocket, buf, (int)strlen(buf), 0, (struct sockaddr*)&serverAddr, sizeof(serverAddr));

		if (retval == SOCKET_ERROR) {
			err_display("sendto()");
			return 0;
		}
		printf("[CLIENT] %d 바이트를 보냈습니다. 전송 내용 >> %s", retval, buf);
	}

	
	CloseHandle(hThread);
	closesocket(clientSocket);
	WSACleanup();
}