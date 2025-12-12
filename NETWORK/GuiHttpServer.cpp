#include <windows.h>
#include <tchar.h>
#include <stdlib.h>

#include "resource.h"
#include "http_server.h"

// 전역
HINSTANCE g_hInst;
HWND g_hDlg = NULL;
HANDLE g_hServerThread = NULL;
BOOL g_serverRunning = FALSE;

// 현재 디렉토리
TCHAR g_CurrentDir[MAX_PATH] = _T("C:\\GIMALWORKSPACE");

#define WM_APP_LOG        (WM_APP + 1)
#define WM_APP_SERVER_END (WM_APP + 2)

// ───────── 로그 출력 ─────────
void AppendLog(LPCTSTR msg) {
    if (!msg || !g_hDlg) return;

    HWND hEdit = GetDlgItem(g_hDlg, IDC_EDIT_LOG);
    if (!hEdit) return;

    int len = GetWindowTextLength(hEdit);
    SendMessage(hEdit, EM_SETSEL, len, len);
    SendMessage(hEdit, EM_REPLACESEL, FALSE, (LPARAM)msg);
}

// ───────── 디렉토리 리스트 갱신 ─────────
void RefreshDirectoryList(HWND hDlg) {
    HWND hList = GetDlgItem(hDlg, IDC_LIST_DIR);
    if (!hList) return;

    SendMessage(hList, LB_RESETCONTENT, 0, 0);

    // 최상단: 상위 폴더
    SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)_T("[.. 상위 폴더]"));

    WIN32_FIND_DATA fd;
    TCHAR sp[MAX_PATH];
    _stprintf_s(sp, _T("%s\\*"), g_CurrentDir);

    HANDLE h = FindFirstFile(sp, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)_T("[경고] 디렉토리 읽기 실패"));
        return;
    }

    TCHAR folders[200][MAX_PATH];
    TCHAR files[200][MAX_PATH];
    int fc = 0, fl = 0;

    do {
        if (_tcscmp(fd.cFileName, _T(".")) == 0 ||
            _tcscmp(fd.cFileName, _T("..")) == 0)
            continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            _stprintf_s(folders[fc++], _T("[DIR] %s"), fd.cFileName);
        else
            _stprintf_s(files[fl++], _T("%s"), fd.cFileName);

    } while (FindNextFile(h, &fd));

    FindClose(h);

    for (int i = 0; i < fc; i++)
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)folders[i]);

    for (int i = 0; i < fl; i++)
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)files[i]);
}

// ───────── 서버 → GUI 로그 콜백 ─────────
void __stdcall GuiLogCallback(const char* msg) {
    if (!msg || !g_hDlg) return;

    char* cp = _strdup(msg);
    if (!cp) return;

    PostMessage(g_hDlg, WM_APP_LOG, 0, (LPARAM)cp);
}

// ───────── 서버 스레드 ─────────
DWORD WINAPI ServerThreadProc(LPVOID p) {
    int port = (int)(INT_PTR)p;

    http_set_log_callback(GuiLogCallback);
    run_http_server(port);

    // 서버 종료 알림 (GUI 스레드에서 버튼 복구)
    PostMessage(g_hDlg, WM_APP_SERVER_END, 0, 0);
    return 0;
}

// ───────── 리스트박스 더블클릭 ─────────
void HandleListDoubleClick(HWND hDlg) {
    HWND hList = GetDlgItem(hDlg, IDC_LIST_DIR);
    if (!hList) return;

    int sel = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;

    TCHAR item[300];
    SendMessage(hList, LB_GETTEXT, sel, (LPARAM)item);

    // 상위 폴더
    if (_tcscmp(item, _T("[.. 상위 폴더]")) == 0) {
        TCHAR* p = _tcsrchr(g_CurrentDir, '\\');
        if (p && p != g_CurrentDir) *p = 0;
        RefreshDirectoryList(hDlg);
        return;
    }

    // 디렉토리 이동
    if (_tcsncmp(item, _T("[DIR] "), 6) == 0) {
        TCHAR folder[MAX_PATH];
        _tcscpy_s(folder, item + 6);
        _stprintf_s(g_CurrentDir, _T("%s\\%s"), g_CurrentDir, folder);
        RefreshDirectoryList(hDlg);
        return;
    }
}

// ───────── 다이얼로그 프로시저 ─────────
INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_INITDIALOG:
        g_hDlg = hDlg;

        SetDlgItemText(hDlg, IDC_EDITPORT, _T("8080"));
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_STOP), FALSE);

        RefreshDirectoryList(hDlg);
        AppendLog(_T("[INFO] GUI 준비됨\r\n"));
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(w)) {

        case IDC_BTN_START:
        {
            if (g_serverRunning) return TRUE;

            TCHAR pbuf[16];
            GetDlgItemText(hDlg, IDC_EDITPORT, pbuf, 16);
            int port = _tstoi(pbuf);
            if (port <= 0 || port > 65535) port = 8080;

            g_serverRunning = TRUE;
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_START), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_BTN_STOP), TRUE);

            g_hServerThread = CreateThread(NULL, 0, ServerThreadProc,
                (LPVOID)(INT_PTR)port, 0, NULL);

            AppendLog(_T("[INFO] 서버 시작됨\r\n"));
        }
        return TRUE;

        case IDC_BTN_STOP:
            http_server_request_stop();
            AppendLog(_T("[INFO] 서버 중지 요청됨\r\n"));
            return TRUE;

        case IDC_BTN_REFRESH:
            RefreshDirectoryList(hDlg);
            return TRUE;

        case IDC_LIST_DIR:
            if (HIWORD(w) == LBN_DBLCLK) {
                HandleListDoubleClick(hDlg);
                return TRUE;
            }
            break;

        case IDCANCEL:
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;

    case WM_APP_LOG:
    {
        char* m = (char*)l;
        if (!m) return TRUE;

#ifdef UNICODE
        int len = MultiByteToWideChar(CP_ACP, 0, m, -1, NULL, 0);
        if (len > 0) {
            WCHAR* bufW = (WCHAR*)malloc(len * sizeof(WCHAR));
            if (bufW) {
                MultiByteToWideChar(CP_ACP, 0, m, -1, bufW, len);
                AppendLog(bufW);
                free(bufW);
            }
        }
#else
        AppendLog(m);
#endif
        free(m);
        return TRUE;
    }

    // ✅ 여기서 받아야 함 (WM_COMMAND 안에 있으면 절대 안 탐)
    case WM_APP_SERVER_END:
        g_serverRunning = FALSE;

        EnableWindow(GetDlgItem(hDlg, IDC_BTN_START), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_BTN_STOP), FALSE);

        // 서버 스레드 핸들 정리 (선택이지만 깔끔)
        if (g_hServerThread) {
            CloseHandle(g_hServerThread);
            g_hServerThread = NULL;
        }

        AppendLog(_T("[INFO] 서버 종료됨\r\n"));
        return TRUE;
    }

    return FALSE;
}

// ───────── WinMain ─────────
int APIENTRY _tWinMain(HINSTANCE h, HINSTANCE, LPTSTR, int) {
    g_hInst = h;
    return (int)DialogBox(h, MAKEINTRESOURCE(IDD_MAIN_DLG), NULL, DlgProc);
}
