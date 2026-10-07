#include "backend.hpp"
#include <iostream>
#include <chrono>

namespace neo {
bool read_exact(HANDLE input, char* bytes, size_t length) {
    size_t position = 0;
    while (position < length) {
        DWORD received = 0;
        if (!ReadFile(input,
                      bytes + position,
                      static_cast<DWORD>(length - position),
                      &received,
                      nullptr) ||
            !received) {
            if (position) {
                throw std::runtime_error("Truncated input frame");
            }
            return false;
        }
        position += received;
    }
    return true;
}

void send_frame(HANDLE output, const std::string& payload) {
    const auto bytes = encode_frame(payload);
    size_t position = 0;
    while (position < bytes.size()) {
        DWORD written = 0;
        wincheck(WriteFile(output,
                           bytes.data() + position,
                           static_cast<DWORD>(bytes.size() - position),
                           &written,
                           nullptr) &&
                     written,
                 "Write output frame");
        position += written;
    }
}

class ScreenEncoder {
    std::vector<std::string> previous_;
    int cols_ = 0;
    int rows_ = 0;
    std::string cursor_;
    uint64_t generation_ = 0;

public:
    std::string encode(const Screen& screen) {
        if (cols_ != screen.cols || rows_ != screen.rows) {
            previous_.assign(screen.rows, {});
            cols_ = screen.cols;
            rows_ = screen.rows;
        }
        std::string rows = "[";
        bool changed = false;
        for (size_t index = 0; index < screen.lines.size(); ++index) {
            std::string line = "[";
            for (const auto& cell : screen.lines[index]) {
                if (line.size() > 1) {
                    line += ',';
                }
                line += cell_json(cell);
            }
            line += ']';
            if (line == previous_[index]) {
                continue;
            }
            if (changed) {
                rows += ',';
            }
            rows += '[' + std::to_string(index) + ',' + line + ']';
            previous_[index] = std::move(line);
            changed = true;
        }
        rows += ']';
        const auto cursor = "\"x\":" + std::to_string(screen.x) +
                            ",\"y\":" + std::to_string(screen.y) +
                            ",\"visible\":" + (screen.visible ? "true" : "false") +
                            ",\"alt\":" + (screen.alt ? "true" : "false");
        if (!changed && cursor == cursor_ && screen.history.empty()) {
            return {};
        }
        cursor_ = cursor;
        std::string history = "[";
        for (const auto& line : screen.history) {
            if (history.size() > 1) {
                history += ',';
            }
            history += json_string(line);
        }
        history += ']';
        return "{\"type\":\"screen\",\"v\":1,\"generation\":" + std::to_string(++generation_) +
               ",\"cols\":" + std::to_string(screen.cols) +
               ",\"height\":" + std::to_string(screen.rows) + ',' + cursor + ",\"rows\":" + rows +
               ",\"history\":" + history + '}';
    }
};
}

int wmain(int argc, wchar_t** argv) {
    using namespace neo;
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    Handle main_thread(OpenThread(THREAD_TERMINATE, FALSE, GetCurrentThreadId()));
    std::unique_ptr<Backend> backend;
    std::thread commands;
    std::atomic<bool> stopping{false};
    std::atomic<bool> main_done{false};
    std::atomic<bool> commands_done{false};
    std::mutex failure_mutex;
    std::string failure;
    bool ready_sent = false;
    auto join_commands = [&] {
        main_done = true;
        if (!commands.joinable()) {
            return;
        }
        while (!commands_done) {
            CancelSynchronousIo(commands.native_handle());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        commands.join();
    };
    try {
        std::vector<std::wstring> arguments;
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        const auto options = parse_options(arguments);
        BackendKind selected = BackendKind::automatic;
        std::string attempts = "[";
        for (const auto kind : candidates(options)) {
            try {
                if (kind == BackendKind::classic) {
                    backend = make_classic(options);
                } else {
                    const bool use_bundled_runtime = kind == BackendKind::bundled;
                    backend = make_conpty(options, use_bundled_runtime);
                }
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
                if (options.kind != BackendKind::automatic) {
                    throw;
                }
            }
        }
        if (!backend) {
            throw std::runtime_error("No backend could initialize: " + attempts + ']');
        }
        backend->launch();
        send_frame(
            output,
            "{\"type\":\"ready\",\"v\":1,\"backend\":" + json_string(backend_name(selected)) +
                ",\"runtime\":" + json_string(backend->runtime()) +
                ",\"pid\":" + std::to_string(backend->pid()) + ",\"attempts\":" + attempts + "]}");
        ready_sent = true;
        commands = std::thread([&] {
            try {
                char header[4];
                while (!main_done && read_exact(input, header, 4)) {
                    std::string payload(frame_length(header), '\0');
                    if (!read_exact(input, payload.data(), payload.size())) {
                        throw std::runtime_error("Missing input payload");
                    }
                    const auto value = parse_command(payload);
                    if (value.type == 'C') {
                        break;
                    }
                    backend->command(value);
                }
            } catch (const std::exception& error) {
                if (!main_done) {
                    std::lock_guard<std::mutex> lock(failure_mutex);
                    failure = error.what();
                }
            }
            stopping = true;
            while (!main_done) {
                CancelSynchronousIo(main_thread.get());
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            commands_done = true;
        });
        ScreenEncoder encoder;
        int final_samples = 0;
        bool finished = false;
        while (!stopping) {
            const auto frame = encoder.encode(backend->snapshot());
            if (!frame.empty()) {
                send_frame(output, frame);
            }
            if (backend->exited() && ++final_samples >= 5) {
                finished = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (finished && !stopping) {
            send_frame(output,
                       "{\"type\":\"exit\",\"v\":1,\"code\":" +
                           std::to_string(backend->exit_code()) + '}');
        }
        join_commands();
        backend->stop();
        {
            std::lock_guard<std::mutex> lock(failure_mutex);
            if (!failure.empty()) {
                throw std::runtime_error(failure);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        const auto last_error = GetLastError();
        const bool requested_stop = stopping;
        stopping = true;
        join_commands();
        if (backend) {
            backend->stop();
        }
        if (ready_sent && requested_stop) {
            return last_error == ERROR_OPERATION_ABORTED && failure.empty() ? 0 : 1;
        }
        try {
            send_frame(output,
                       "{\"type\":\"error\",\"v\":1,\"message\":" + json_string(error.what()) +
                           '}');
        } catch (...) {
        }
        return 1;
    }
}
