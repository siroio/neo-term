#pragma once
#include "backend.hpp"
#include <array>
#include <wincrypt.h>

namespace neo {
inline std::wstring environment_value(const wchar_t* name) {
    const auto length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) {
        return {};
    }
    std::wstring value(length, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), length));
    return value;
}

class ShellBridge {
    Handle pipe_;
    Handle event_;
    OVERLAPPED operation_{};
    std::array<char, 16385> bytes_{};
    bool connecting_ = false;
    bool reading_ = false;

    void prepare_operation() {
        ResetEvent(event_.get());
        operation_ = {};
        operation_.hEvent = event_.get();
    }

    void connect() {
        prepare_operation();
        connecting_ = !ConnectNamedPipe(pipe_.get(), &operation_);
        if (connecting_) {
            const auto error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connecting_ = false;
            } else {
                wincheck(error == ERROR_IO_PENDING, "Accept shell notification connection");
            }
        }
    }

    std::string finish(DWORD received, bool valid) {
        const std::string message =
            valid && received <= 16384 ? std::string(bytes_.data(), received) : std::string{};
        reading_ = false;
        DisconnectNamedPipe(pipe_.get());
        connect();
        return message;
    }

public:
    ShellBridge() {
        const auto name = L"neo-term-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(GetTickCount64());
        const auto path = L"\\\\.\\pipe\\" + name;
        pipe_.reset(CreateNamedPipeW(
            path.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            16384,
            16384,
            0,
            nullptr));
        wincheck(pipe_.get() != INVALID_HANDLE_VALUE, "Create shell notification pipe");
        event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        wincheck(event_.get() != nullptr, "Create shell notification event");
        wincheck(SetEnvironmentVariableW(L"NEO_TERM_PIPE", name.c_str()), "Set shell pipe");
        wincheck(SetEnvironmentVariableW(L"NEO_TERM_HOST", executable_path().c_str()),
                 "Set shell helper");
        connect();
    }

    ~ShellBridge() {
        CancelIoEx(pipe_.get(), &operation_);
        DWORD transferred = 0;
        GetOverlappedResult(pipe_.get(), &operation_, &transferred, TRUE);
    }

    std::string poll() {
        DWORD received = 0;
        if (connecting_ || reading_) {
            if (!GetOverlappedResult(pipe_.get(), &operation_, &received, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) {
                    return {};
                }
                return finish(0, false);
            }
            if (reading_) {
                return finish(received, true);
            }
            connecting_ = false;
        }
        prepare_operation();
        if (ReadFile(pipe_.get(),
                     bytes_.data(),
                     static_cast<DWORD>(bytes_.size()),
                     &received,
                     &operation_)) {
            return finish(received, true);
        }
        if (GetLastError() == ERROR_IO_PENDING) {
            reading_ = true;
            return {};
        }
        return finish(0, false);
    }
};

inline void send_shell_notification(const std::string& message) {
    const auto name = environment_value(L"NEO_TERM_PIPE");
    if (name.empty() || message.size() > 16384) {
        throw std::runtime_error("Not inside a neo-term session or notification too large");
    }
    const auto path = L"\\\\.\\pipe\\" + name;
    Handle connection;
    const auto deadline = GetTickCount64() + 2000;
    while (GetTickCount64() < deadline) {
        connection.reset(
            CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        if (connection.get() != INVALID_HANDLE_VALUE) {
            break;
        }
        if (GetLastError() != ERROR_PIPE_BUSY) {
            wincheck(false, "Connect shell notification pipe");
        }
        WaitNamedPipeW(path.c_str(), 100);
    }
    wincheck(connection.get() != INVALID_HANDLE_VALUE, "Connect shell notification pipe");
    DWORD written = 0;
    wincheck(WriteFile(connection.get(),
                       message.data(),
                       static_cast<DWORD>(message.size()),
                       &written,
                       nullptr) &&
                 written == message.size(),
             "Send shell notification");
}

inline int shell_helper(int argc, wchar_t** argv) {
    try {
        const std::wstring action = argc > 2 ? argv[2] : L"";
        if (action == L"open" && argc >= 4 && argc <= 6) {
            std::wstring path(32768, L'\0');
            const auto length =
                GetFullPathNameW(argv[3], static_cast<DWORD>(path.size()), path.data(), nullptr);
            wincheck(length > 0 && length < path.size(), "Resolve file path");
            path.resize(length);
            const auto line = argc > 4 ? integer(utf8(argv[4])) : 1;
            const auto column = argc > 5 ? integer(utf8(argv[5])) : 1;
            if (line < 1 || column < 1) {
                throw std::runtime_error("Line and column must be positive");
            }
            send_shell_notification("51;neo-term;{\"file\":" + json_string(utf8(path)) +
                                    ",\"line\":" + std::to_string(line) +
                                    ",\"column\":" + std::to_string(column) + '}');
        } else if (action == L"event" && argc == 4) {
            send_shell_notification(utf8(argv[3]));
        } else if (action == L"encoded" && argc == 4) {
            const auto encoded = utf8(argv[3]);
            DWORD size = 0;
            wincheck(CryptStringToBinaryA(encoded.c_str(),
                                          static_cast<DWORD>(encoded.size()),
                                          CRYPT_STRING_BASE64,
                                          nullptr,
                                          &size,
                                          nullptr,
                                          nullptr),
                     "Decode shell notification size");
            if (size > 16384) {
                throw std::runtime_error("Shell notification too large");
            }
            std::string message(size, '\0');
            wincheck(CryptStringToBinaryA(encoded.c_str(),
                                          static_cast<DWORD>(encoded.size()),
                                          CRYPT_STRING_BASE64,
                                          reinterpret_cast<BYTE*>(message.data()),
                                          &size,
                                          nullptr,
                                          nullptr),
                     "Decode shell notification");
            send_shell_notification(message);
        } else if (action == L"cwd" && argc == 3) {
            std::wstring directory(32768, L'\0');
            directory.resize(
                GetCurrentDirectoryW(static_cast<DWORD>(directory.size()), directory.data()));
            send_shell_notification("cwd;" + utf8(directory));
        } else {
            throw std::runtime_error(
                "Usage: --notify open FILE [LINE [COLUMN]] | event TEXT | cwd");
        }
        return 0;
    } catch (const std::exception& error) {
        const auto message = std::string("neo-term: ") + error.what() + '\n';
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE),
                  message.data(),
                  static_cast<DWORD>(message.size()),
                  &written,
                  nullptr);
        return 1;
    }
}
}
