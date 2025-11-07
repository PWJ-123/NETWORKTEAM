#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <time.h>

#include "common.h"

#define BUFSIZE 256
#define SERVER_PORT 8080

void main() {
	int retval;

	// Winsock 초기화
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		err_display("WSAStartup()");
		return;
	}
	else {
		printf("[SERVER] Winsock 초기화 성공!\n");
	}

	// 서버 소켓 생성(UDP version)
	SOCKET serverSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (serverSocket == INVALID_SOCKET) {
		err_display("socket()");
		return;
	}
	else {
		printf("[SERVER] 서버 소켓 생성 성공.\n");
	}


	// 서버 주소 생성 ~ 바인딩
	struct sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serverAddr.sin_port = htons(SERVER_PORT);
	retval = bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr));
	if (retval == SOCKET_ERROR) {
		err_display("bind()");
		return;
	}
	else {
		printf("[SERVER] 서버 소켓 바인딩 성공.\n");
	}


	// 통신 상대 주소 저장 변수
	struct sockaddr_in peerAddr;
	int addrLen;

	char buf[BUFSIZE];
	printf("========================================================\n");
	while (TRUE) {
		addrLen = sizeof(peerAddr);
		retval = recvfrom(serverSocket, buf, BUFSIZE, 0, (struct sockaddr*)&peerAddr, &addrLen);
		if (retval == SOCKET_ERROR) {
			err_display("recvfrom()");
			break;		// 반복문 강제종료 후 프로그램 종료 절차 시행
		}

		buf[retval] = '\0';
		printf("[SERVER] %d 바이트 수신 완료. 수신 내용 >> %s", retval, buf);

		// 수신받은 정보를 그대로 송신측에 다시 전달함
		retval = sendto(serverSocket, buf, retval, 0, (struct sockaddr*)&peerAddr, addrLen);
		if (retval == SOCKET_ERROR) {
			err_display("sendto()");
		}
		printf("[SERVER] 수신 정보 재전송...\n");
	}

	closesocket(serverSocket);
	WSACleanup();
}