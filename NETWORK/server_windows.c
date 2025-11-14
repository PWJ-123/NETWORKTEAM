//#define _WINSOCK_DEPRECATED_NO_WARNINGS // inet_ntoa 관련 경고 무시
//
//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
//#include <winsock2.h> // Winsock 헤더 포함
//#include <ws2tcpip.h> // 최신 소켓 함수들을 위한 헤더
//
//// ws2_32.lib 라이브러리를 링크하도록 컴파일러에 지시
//#pragma comment(lib, "ws2_32.lib") 
//
//// 오류 발생 시 메시지를 출력하고 프로그램을 종료하는 함수
//void error_handling(const char* message) {
//    // fputs는 printf와 비슷하지만, 출력 스트림을 지정할 수 있어요. stderr는 에러 출력용 스트림이에요.
//    fputs(message, stderr);
//    fputc('\n', stderr);
//    exit(1);
//}
//
///**
// * @brief 새로운 클라이언트 연결을 수락하고 통신용 소켓을 반환합니다.
// * @param server_sock 서버의 리스닝 소켓
// * @return 성공 시 클라이언트와 통신할 새로운 소켓, 실패 시 INVALID_SOCKET
// */
//SOCKET accept_client_connection(SOCKET server_sock) {
//    SOCKADDR_IN client_addr;
//    int client_addr_size = sizeof(client_addr);
//
//    // accept(): 클라이언트 연결 요청을 수락하고, 통신용 새 소켓(client_sock)을 생성
//    SOCKET client_sock = accept(server_sock, (SOCKADDR*)&client_addr, &client_addr_size);
//
//    if (client_sock == INVALID_SOCKET) {
//        printf("accept() error: %d\n", WSAGetLastError());
//        return INVALID_SOCKET; // 오류 발생 시 INVALID_SOCKET 반환
//    }
//
//    // 연결된 클라이언트 정보 출력
//    printf("===== Client Connected =====\n");
//    printf("IP: %s\n", inet_ntoa(client_addr.sin_addr));
//    printf("Socket Handle: %llu\n", client_sock); // SOCKET은 64비트 정수일 수 있어 %llu 사용
//    printf("==========================\n\n");
//
//    return client_sock;
//}
//
//int main(int argc, char* argv[]) {
//    WSADATA wsaData; // Winsock 라이브러리 정보를 담을 구조체
//
//    if (argc != 2) {
//        printf("사용법: %s <port>\n", argv[0]);
//        exit(1);
//    }
//
//    // 1. Winsock 라이브러리 초기화
//    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
//        error_handling("WSAStartup() error!");
//    }
//    printf("Winsock 라이브러리를 성공적으로 초기화했습니다.\n");
//
//    // 2. TCP/IP 소켓 생성 (socket())
//    SOCKET server_sock = socket(PF_INET, SOCK_STREAM, 0);
//    if (server_sock == INVALID_SOCKET) {
//        error_handling("socket() error");
//    }
//
//    // 서버 주소 정보 설정
//    SOCKADDR_IN server_addr;
//    memset(&server_addr, 0, sizeof(server_addr));
//    server_addr.sin_family = AF_INET;
//    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
//
//    //server_addr.sin_port = htons(atoi(argv[1]));
//    server_addr.sin_port = htons(8080);
//
//    // 3. 서버 포트 바인딩 (bind())
//    if (bind(server_sock, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
//        error_handling("bind() error");
//    }
//
//    // 4. 연결 대기 (listen())
//    if (listen(server_sock, 5) == SOCKET_ERROR) {
//        error_handling("listen() error");
//    }
//
//    printf("서버가 %s 포트에서 연결을 기다리는 중입니다...\n", argv[1]);
//
//    // 5. 서버 메인 루프
//    while (1) {
//        // 6. 클라이언트 연결 수락 및 통신용 소켓 생성
//        SOCKET client_sock = accept_client_connection(server_sock);
//
//        if (client_sock != INVALID_SOCKET) {
//            // 결과물: 새로운 클라이언트와 통신할 소켓이 생성됨
//
//            // 간단한 메시지 전송 예시 (write -> send)
//            const char msg[] = "Hello, Client! Welcome to the Windows server.\r\n";
//            send(client_sock, msg, sizeof(msg) - 1, 0); // NULL 문자는 제외하고 전송
//
//            closesocket(client_sock); // 클라이언트와의 통신 종료 (close -> closesocket)
//        }
//    }
//
//    // 7. Winsock 라이브러리 정리 및 서버 소켓 종료
//    closesocket(server_sock);
//    WSACleanup();
//    return 0;
//}