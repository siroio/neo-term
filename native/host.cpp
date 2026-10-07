#include "backend.hpp"
#include <iostream>
#include <chrono>

namespace neo {
bool read_exact(HANDLE input, char* bytes, size_t length) {
    size_t position = 0;
    while (position < length) {
        DWORD received = 0;
        if (!ReadFile(input, bytes + position, static_cast<DWORD>(length - position), &received, nullptr) || !received) {
            if (position) throw std::runtime_error("Truncated input frame");
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
        wincheck(WriteFile(output, bytes.data() + position, static_cast<DWORD>(bytes.size() - position), &written, nullptr) && written, "Write output frame");
        position += written;
    }
}
class ScreenEncoder {
    std::vector<std::string> previous_;
    int cols_ = 0, rows_ = 0;
    std::string cursor_;
    uint64_t generation_ = 0;
public:
    std::string encode(const Screen& screen) {
        if (cols_ != screen.cols || rows_ != screen.rows) { previous_.assign(screen.rows, {}); cols_ = screen.cols; rows_ = screen.rows; }
        std::string rows = "[";
        bool changed = false;
        for (size_t index = 0; index < screen.lines.size(); ++index) {
            std::string line = "[";
            for (const auto& cell : screen.lines[index]) { if (line.size() > 1) line += ','; line += cell_json(cell); }
            line += ']';
            if (line == previous_[index]) continue;
            if (changed) rows += ',';
            rows += '[' + std::to_string(index) + ',' + line + ']';
            previous_[index] = std::move(line);
            changed = true;
        }
        rows += ']';
        const auto cursor = "\"x\":" + std::to_string(screen.x) + ",\"y\":" + std::to_string(screen.y) + ",\"visible\":" + (screen.visible ? "true" : "false") + ",\"alt\":" + (screen.alt ? "true" : "false");
        if (!changed && cursor == cursor_ && screen.history.empty()) return {};
        cursor_ = cursor;
        std::string history = "[";
        for (const auto& line : screen.history) { if (history.size() > 1) history += ','; history += json_string(line); }
        history += ']';
        return "{\"type\":\"screen\",\"v\":1,\"generation\":" + std::to_string(++generation_) + ",\"cols\":" + std::to_string(screen.cols) + ",\"height\":" + std::to_string(screen.rows) + ',' + cursor + ",\"rows\":" + rows + ",\"history\":" + history + '}';
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
    std::mutex failure_mutex;
    std::string failure;
    bool ready_sent = false;
    try {
        std::vector<std::wstring> arguments;
        for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
        const auto options = parse_options(arguments);
        BackendKind selected = BackendKind::automatic;
        std::string attempts = "[";
        for (const auto kind : candidates(options)) {
            try {
                backend = kind == BackendKind::classic ? make_classic(options) : make_conpty(options, kind == BackendKind::bundled);
                backend->initialize();
                selected = kind;
                break;
            } catch (const std::exception& error) {
                if (attempts.size() > 1) attempts += ',';
                attempts += "{\"backend\":" + json_string(backend_name(kind)) + ",\"error\":" + json_string(error.what()) + '}';
                backend.reset();
                if (options.kind != BackendKind::automatic) throw;
            }
        }
        if (!backend) throw std::runtime_error("No backend could initialize: " + attempts + ']');
        backend->launch();
        send_frame(output, "{\"type\":\"ready\",\"v\":1,\"backend\":" + json_string(backend_name(selected)) + ",\"runtime\":" + json_string(backend->runtime()) + ",\"pid\":" + std::to_string(backend->pid()) + ",\"attempts\":" + attempts + "]}");
        ready_sent = true;
        commands = std::thread([&] {
            try {
                char header[4];
                while (read_exact(input, header, 4)) {
                    std::string payload(frame_length(header), '\0');
                    if (!read_exact(input, payload.data(), payload.size())) throw std::runtime_error("Missing input payload");
                    const auto value = parse_command(payload);
                    if (value.type == 'C') break;
                    backend->command(value);
                }
            } catch (const std::exception& error) { std::lock_guard<std::mutex> lock(failure_mutex); failure = error.what(); }
            stopping = true;
            CancelSynchronousIo(main_thread.get());
        });
        ScreenEncoder encoder;
        int final_samples = 0;
        bool finished = false;
        while (!stopping) {
            const auto frame = encoder.encode(backend->snapshot());
            if (!frame.empty()) send_frame(output, frame);
            if (backend->exited() && ++final_samples >= 5) { finished = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (commands.joinable()) { CancelSynchronousIo(commands.native_handle()); commands.join(); }
        backend->stop();
        { std::lock_guard<std::mutex> lock(failure_mutex); if (!failure.empty()) throw std::runtime_error(failure); }
        if (finished) send_frame(output, "{\"type\":\"exit\",\"v\":1,\"code\":" + std::to_string(backend->exit_code()) + '}');
        return 0;
    } catch (const std::exception& error) {
        stopping = true;
        if (commands.joinable()) { CancelSynchronousIo(commands.native_handle()); commands.join(); }
        if (backend) backend->stop();
        if (ready_sent && GetLastError() == ERROR_OPERATION_ABORTED && failure.empty()) return 0;
        try { send_frame(output, "{\"type\":\"error\",\"v\":1,\"message\":" + json_string(error.what()) + '}'); } catch (...) {}
        return 1;
    }
}
