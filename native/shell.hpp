#pragma once
#include "backend.hpp"
#include <array>
#include <wincrypt.h>

namespace neo {
class Socket {
    SOCKET value_ = INVALID_SOCKET;
public:
    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { reset(); }
    SOCKET get() const { return value_; }
    void reset(SOCKET value = INVALID_SOCKET) {
        if (value_ != INVALID_SOCKET) { closesocket(value_); }
        value_ = value;
    }
};
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
    Socket listener_, connection_;
    std::string token_, pending_;
    unsigned short port_ = 0;
    ULONGLONG deadline_ = 0;
    std::wstring name_;
    Handle pipe_;
    Handle event_;
    Handle listener_event_, connection_event_;
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
                SetEvent(event_.get());
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
    explicit ShellBridge(std::string token) : token_(std::move(token)) {
        listener_.reset(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        wincheck(listener_.get() != INVALID_SOCKET, "Create shell notification socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        wincheck(bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                 "Bind shell notification socket");
        int size = sizeof(address);
        wincheck(getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &size) == 0,
                 "Read shell notification port");
        port_ = ntohs(address.sin_port);
        wincheck(listen(listener_.get(), 4) == 0, "Listen for shell notification");
        listener_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        connection_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        wincheck(listener_event_.get() && connection_event_.get(), "Create shell socket events");
        wincheck(WSAEventSelect(listener_.get(), listener_event_.get(), FD_ACCEPT) == 0,
                 "Observe shell notification connections");
        u_long nonblocking = 1;
        wincheck(ioctlsocket(listener_.get(), FIONBIO, &nonblocking) == 0,
                 "Set shell notification socket mode");
        static std::atomic<unsigned long> sequence{0};
        name_ = L"neo-term-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(++sequence);
        const auto path = L"\\\\.\\pipe\\" + name_;
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
        connect();
    }

    const std::wstring& name() const {
        return name_;
    }

    unsigned short port() const { return port_; }

    std::vector<HANDLE> wait_handles() const {
        return {event_.get(), connection_.get() == INVALID_SOCKET
                                  ? listener_event_.get() : connection_event_.get()};
    }

    DWORD wait_timeout() const {
        if (connection_.get() == INVALID_SOCKET) { return INFINITE; }
        const auto now = GetTickCount64();
        return now >= deadline_ ? 0 : static_cast<DWORD>(deadline_ - now);
    }

    std::string poll_socket() {
        WSANETWORKEVENTS events{};
        if (connection_.get() == INVALID_SOCKET) {
            wincheck(WSAEnumNetworkEvents(listener_.get(), listener_event_.get(), &events) == 0,
                     "Read shell listener events");
            connection_.reset(accept(listener_.get(), nullptr, nullptr));
            if (connection_.get() == INVALID_SOCKET) { return {}; }
            wincheck(WSAEventSelect(connection_.get(), connection_event_.get(),
                                   FD_READ | FD_CLOSE) == 0, "Observe shell message input");
            u_long nonblocking = 1;
            ioctlsocket(connection_.get(), FIONBIO, &nonblocking);
            pending_.clear();
            deadline_ = GetTickCount64() + 2000;
        }
        wincheck(WSAEnumNetworkEvents(connection_.get(), connection_event_.get(), &events) == 0,
                 "Read shell connection events");
        char bytes[4096];
        while (pending_.size() <= 16449 && GetTickCount64() < deadline_) {
            const int length = recv(connection_.get(), bytes, sizeof(bytes), 0);
            if (length > 0) {
                pending_.append(bytes, length);
                continue;
            }
            if (length < 0 && WSAGetLastError() == WSAEWOULDBLOCK) { return {}; }
            connection_.reset();
            ResetEvent(connection_event_.get());
            if (length == 0 && pending_.size() <= 16449 &&
                pending_.compare(0, token_.size() + 1, token_ + '\n') == 0) {
                return pending_.substr(token_.size() + 1);
            }
            pending_.clear();
            return {};
        }
        connection_.reset();
        ResetEvent(connection_event_.get());
        pending_.clear();
        return {};
    }

    ~ShellBridge() {
        CancelIoEx(pipe_.get(), &operation_);
        DWORD transferred = 0;
        GetOverlappedResult(pipe_.get(), &operation_, &transferred, TRUE);
    }

    std::string poll() {
        const auto message = poll_socket();
        if (!message.empty()) { return message; }
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

}
