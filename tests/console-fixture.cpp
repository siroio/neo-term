#include <windows.h>
#include <string>
#include <cstdio>

static BOOL WINAPI interrupted(DWORD type) {
    if (type != CTRL_C_EVENT) {
        return FALSE;
    }
    const wchar_t message[] = L"\r\nINTERRUPTED\r\n";
    DWORD count;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE),
                  message,
                  static_cast<DWORD>(wcslen(message)),
                  &count,
                  nullptr);
    return TRUE;
}

static void print(const std::wstring& text) {
    DWORD count;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE),
                  text.data(),
                  static_cast<DWORD>(text.size()),
                  &count,
                  nullptr);
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    const std::wstring mode = argc > 1 ? argv[1] : L"unicode";
    if (mode == L"sleeper") {
        Sleep(INFINITE);
        return 0;
    }
    if (mode == L"descendant") {
        wchar_t path[32768];
        GetModuleFileNameW(nullptr, path, 32768);
        std::wstring command = L"\"" + std::wstring(path) + L"\" sleeper";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(path,
                            command.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            0,
                            nullptr,
                            nullptr,
                            &startup,
                            &child)) {
            return 1;
        }
        print(L"DESCENDANT_PID=" + std::to_wstring(child.dwProcessId) + L"\r\n");
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        Sleep(INFINITE);
        return 0;
    }
    if (mode == L"unicode") {
        print(L"日本語😀\r\n");
        Sleep(250);
        return 7;
    }
    if (mode == L"arguments") {
        for (int index = 2; index < argc; ++index) {
            print(L"ARG" + std::to_wstring(index) + L"=" + argv[index] + L"\r\n");
        }
        Sleep(250);
        return 0;
    }
    if (mode == L"vt") {
        DWORD output_mode;
        GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &output_mode);
        SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE),
                       output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        print(L"\x1b[?1049h\x1b[2J\x1b[H\x1b[38;2;18;171;52mALT_SCREEN\x1b[0m");
        Sleep(500);
        print(L"\x1b[?1049l");
        for (int index = 0; index < 3000; ++index) {
            print(L"burst " + std::to_wstring(index) + L"\r\n");
        }
        print(L"BURST_DONE\r\n");
        Sleep(300);
        return 0;
    }
    SetConsoleCtrlHandler(interrupted, TRUE);
    DWORD input_mode;
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode);
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE),
                   (input_mode | ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT) &
                       ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT));
    if (mode == L"history") {
        for (int index = 0; index < 300; ++index) {
            print(L"history " + std::to_wstring(index) + L"\r\n");
        }
    }
    print(L"FIXTURE_READY\r\n");
    INPUT_RECORD event;
    DWORD count;
    while (ReadConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &event, 1, &count)) {
        if (event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown) {
            print(L"KEY=" + std::to_wstring(event.Event.KeyEvent.wVirtualKeyCode) + L" CHAR=" +
                  std::to_wstring(event.Event.KeyEvent.uChar.UnicodeChar) + L"\r\n");
        }
    }
    return 0;
}
