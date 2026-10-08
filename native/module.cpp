#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include "backend.hpp"
#include "shell.hpp"
#include "screen-encoder.hpp"
struct emacs_runtime;
extern "C" __declspec(dllexport) int emacs_module_init(emacs_runtime*) noexcept;
extern "C" {
#include "../vendor/emacs-module.h"
}
#include <chrono>
#include <cstring>

extern "C" __declspec(dllexport) int plugin_is_GPL_compatible = 1;

namespace neo {
class Session : public std::enable_shared_from_this<Session> {
    SOCKET output_ = INVALID_SOCKET;
    std::mutex socket_mutex_;
    bool winsock_started_ = false;
    unsigned short port_;
    std::string token_;
    Options options_;
    Handle wake_;
    std::mutex mutex_;
    std::deque<std::pair<Command, size_t>> commands_;
    size_t queued_ = 0;
    std::atomic<bool> stopping_{false}, done_{true}, supports_clear_{false};

    void close_output() {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (output_ != INVALID_SOCKET) {
            closesocket(output_);
            output_ = INVALID_SOCKET;
        }
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
    }

    void send(const std::string& payload) {
        const auto bytes = encode_frame(payload);
        size_t position = 0;
        while (position < bytes.size() && !stopping_) {
            const auto written = ::send(output_, bytes.data() + position,
                                        static_cast<int>(bytes.size() - position), 0);
            if (written <= 0) {
                throw std::runtime_error("Module output connection closed");
            }
            position += static_cast<size_t>(written);
        }
    }

    void run() noexcept {
        std::unique_ptr<Backend> backend;
        try {
            send("{\"type\":\"auth\",\"token\":" + json_string(token_) + '}');
            BackendKind selected = BackendKind::automatic;
            std::string attempts = "[";
            for (const auto kind : candidates(options_)) {
                if (stopping_) {
                    break;
                }
                try {
                    backend = make_conpty(options_, kind == BackendKind::bundled);
                    backend->initialize();
                    selected = kind;
                    break;
                } catch (const std::exception& error) {
                    if (attempts.size() > 1) {
                        attempts += ',';
                    }
                    attempts += "{\"backend\":" + json_string(backend_name(kind)) +
                                ",\"error\":" + json_string(error.what()) + '}';
                    backend.reset();
                    if (options_.kind != BackendKind::automatic) {
                        throw;
                    }
                }
            }
            if (!backend) {
                throw std::runtime_error("No ConPTY runtime could initialize: " + attempts + ']');
            }
            supports_clear_ = backend->supports_clear();
            ShellBridge shell(token_);
            backend->set_shell_environment(shell.name(), shell.port(), token_);
            if (!stopping_) {
                backend->launch();
                send("{\"type\":\"ready\",\"v\":1,\"transport\":\"dll\",\"backend\":" +
                     json_string(backend_name(selected)) + ",\"runtime\":" +
                     json_string(backend->runtime()) + ",\"clear\":" + (supports_clear_ ? "true" : "false") + ",\"pid\":" + std::to_string(backend->pid()) +
                     ",\"attempts\":" + attempts + "]}");
            }
            ScreenEncoder encoder;
            bool child_exited = false;
            while (!stopping_) {
                std::deque<std::pair<Command, size_t>> commands;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    commands.swap(commands_);
                    queued_ = 0;
                    ResetEvent(wake_.get());
                }
                for (const auto& command : commands) {
                    if (stopping_) {
                        break;
                    }
                    backend->command(command.first);
                }
                if (!child_exited && backend->exited()) {
                    backend->begin_exit();
                    child_exited = true;
                }
                const auto notification = shell.poll();
                if (!notification.empty()) {
                    backend->shell_notification(notification);
                }
                std::string frame;
                bool history_pending = false;
                if (backend->has_updates() || !notification.empty()) {
                    auto screen = backend->snapshot();
                    if (!notification.empty() && notification.rfind("prompt;", 0) != 0) {
                        screen.shell_events.push_back(notification);
                    }
                    history_pending = screen.history_rows.size() >= 64;
                    frame = encoder.encode(screen);
                    if (!frame.empty()) {
                        send(frame);
                    }
                }
                if (child_exited && backend->output_finished() && !history_pending) {
                    const auto code = backend->exit_code();
                    backend->stop();
                    send("{\"type\":\"exit\",\"v\":1,\"code\":" + std::to_string(code) + '}');
                    break;
                }
                if (history_pending) {
                    continue;
                }
                if (!frame.empty()) {
                    WaitForSingleObject(wake_.get(), 8);
                }
                backend->wait_for_update(20, wake_.get());
            }
        } catch (const std::exception& error) {
            if (!stopping_) {
                try {
                    send("{\"type\":\"error\",\"v\":1,\"message\":" + json_string(error.what()) + '}');
                } catch (...) {
                }
            }
        } catch (...) {
        }
        if (backend) {
            backend->stop();
            backend.reset();
        }
        close_output();
        done_ = true;
    }

public:
    Session(unsigned short port, Options options) : port_(port), options_(std::move(options)) {
        unsigned char bytes[32];
        wincheck(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0,
                 "Create module authentication token");
        static const char digits[] = "0123456789abcdef";
        for (const auto byte : bytes) {
            token_ += digits[byte >> 4];
            token_ += digits[byte & 15];
        }
        wake_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        wincheck(wake_.get() != nullptr, "Create module command event");
    }

    ~Session() {
        close_output();
    }

    const std::string& token() const {
        return token_;
    }

    void start() {
        try {
            WSADATA data;
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
                throw std::runtime_error("Initialize module connection");
            }
            winsock_started_ = true;
            {
                std::lock_guard<std::mutex> lock(socket_mutex_);
                output_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (output_ == INVALID_SOCKET) {
                    throw std::runtime_error("Create module connection");
                }
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port_);
            const BOOL no_delay = TRUE;
            setsockopt(output_, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
            const int send_buffer = 256 * 1024;
            setsockopt(output_, SOL_SOCKET, SO_SNDBUF,
                       reinterpret_cast<const char*>(&send_buffer), sizeof(send_buffer));
            u_long nonblocking = 1;
            ioctlsocket(output_, FIONBIO, &nonblocking);
            if (connect(output_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
                if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    throw std::runtime_error("Connect module notification channel");
                }
                fd_set writable;
                FD_ZERO(&writable);
                FD_SET(output_, &writable);
                timeval timeout{0, 100000};
                int error = 1, length = sizeof(error);
                if (select(0, nullptr, &writable, nullptr, &timeout) != 1 ||
                    getsockopt(output_, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&error), &length) != 0 || error) {
                    throw std::runtime_error("Module connection timed out");
                }
            }
            nonblocking = 0;
            ioctlsocket(output_, FIONBIO, &nonblocking);
        } catch (...) {
            close_output();
            throw;
        }
        const auto self = shared_from_this();
        done_ = false;
        try {
            std::thread([self] { self->run(); }).detach();
        } catch (...) {
            done_ = true;
            throw;
        }
    }

    void stop() {
        if (done_ || stopping_.exchange(true)) {
            return;
        }
        SetEvent(wake_.get());
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (output_ != INVALID_SOCKET) {
            shutdown(output_, SD_BOTH);
        }
    }

    void command(const std::string& payload) {
        const auto command = parse_command(payload);
        if (command.type == 'L' && !supports_clear_) {
            throw std::runtime_error("Screen clear requires the ConPTY clear API");
        }
        if (command.type == 'C') {
            stop();
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || done_) {
            throw std::runtime_error("Terminal has exited");
        }
        if (queued_ + payload.size() > max_frame) {
            throw std::runtime_error("Module input queue exceeds 4MiB");
        }
        commands_.emplace_back(command, payload.size());
        queued_ += payload.size();
        SetEvent(wake_.get());
    }
};

using SessionPointer = std::shared_ptr<Session>;

void finalize_session(void* pointer) noexcept {
    auto session = static_cast<SessionPointer*>(pointer);
    try {
        (*session)->stop();
    } catch (...) {
    }
    delete session;
}

std::string module_string(emacs_env* env, emacs_value value) {
    ptrdiff_t length = 0;
    if (!env->copy_string_contents(env, value, nullptr, &length)) {
        throw std::runtime_error("Expected a string");
    }
    if (length <= 0 || length > max_frame + 1) {
        throw std::runtime_error("Module string exceeds 4MiB");
    }
    std::string result(static_cast<size_t>(length), '\0');
    if (!env->copy_string_contents(env, value, result.data(), &length)) {
        throw std::runtime_error("Cannot read module string");
    }
    result.resize(static_cast<size_t>(length - 1));
    if (result.find('\0') != std::string::npos) {
        throw std::runtime_error("Module string contains a null character");
    }
    return result;
}

void module_error(emacs_env* env, const char* message) {
    if (env->non_local_exit_check(env) == emacs_funcall_exit_return) {
        auto text = env->make_string(env, message, static_cast<ptrdiff_t>(std::strlen(message)));
        auto data = env->funcall(env, env->intern(env, "list"), 1, &text);
        env->non_local_exit_signal(env, env->intern(env, "error"), data);
    }
}

emacs_value module_start(emacs_env* env, ptrdiff_t, emacs_value* args, void*) noexcept {
    const auto nil = env->intern(env, "nil");

    try {
        std::vector<std::wstring> arguments;
        const auto count = env->vec_size(env, args[1]);
        if (count < 1 || count > 4096) {
            throw std::runtime_error("Invalid module argument count");
        }
        for (ptrdiff_t index = 0; index < count; ++index) {
            arguments.push_back(wide(module_string(env, env->vec_get(env, args[1], index))));
        }
        auto options = parse_options(arguments);
        options.directory = wide(module_string(env, args[2]));
        options.runtime_directory = wide(module_string(env, args[3]));
        const auto variables = env->vec_size(env, args[4]);
        if (variables < 0 || variables > 32768) {
            throw std::runtime_error("Invalid environment variable count");
        }
        for (ptrdiff_t index = 0; index < variables; ++index) {
            options.environment.push_back(wide(module_string(env, env->vec_get(env, args[4], index))));
        }
        const auto port = env->extract_integer(env, args[0]);
        if (port < 1 || port > 65535) {
            throw std::runtime_error("Invalid module notification port");
        }
        auto session = std::make_shared<Session>(static_cast<unsigned short>(port), std::move(options));
        auto pointer = std::make_unique<SessionPointer>(session);
        const auto value = env->make_user_ptr(env, finalize_session, pointer.get());
        if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
            return nil;
        }
        pointer.release();
        emacs_value values[] = {value, env->make_string(env, session->token().data(),
                                                       static_cast<ptrdiff_t>(session->token().size()))};
        const auto result = env->funcall(env, env->intern(env, "vector"), 2, values);
        if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
            return nil;
        }
        session->start();
        return result;
    } catch (const std::exception& error) {
        module_error(env, error.what());
        return nil;
    }
}

emacs_value module_command(emacs_env* env, ptrdiff_t, emacs_value* args, void*) noexcept {
    const auto nil = env->intern(env, "nil");
    try {
        if (env->get_user_finalizer(env, args[0]) != finalize_session) {
            throw std::runtime_error("Invalid neo-term module session");
        }
        auto pointer = static_cast<SessionPointer*>(env->get_user_ptr(env, args[0]));
        if (env->non_local_exit_check(env) != emacs_funcall_exit_return) {
            return nil;
        }
        (*pointer)->command(module_string(env, args[1]));
    } catch (const std::exception& error) {
        module_error(env, error.what());
    }
    return nil;
}
}

extern "C" __declspec(dllexport) int emacs_module_init(emacs_runtime* runtime) noexcept {
    auto env = runtime->get_environment(runtime);
    if (env->size < static_cast<ptrdiff_t>(offsetof(emacs_env, make_user_ptr) + sizeof(env->make_user_ptr))) {
        return 1;
    }
    emacs_value bindings[] = {
        env->intern(env, "neo-term--module-start"),
        env->make_function(env, 5, 5, neo::module_start, "Start an asynchronous ConPTY session.", nullptr)};
    env->funcall(env, env->intern(env, "fset"), 2, bindings);
    bindings[0] = env->intern(env, "neo-term--module-command");
    bindings[1] = env->make_function(env, 2, 2, neo::module_command, "Queue a terminal command.", nullptr);
    env->funcall(env, env->intern(env, "fset"), 2, bindings);
    return 0;
}
