#include "backend.hpp"
#include "terminal.hpp"
#include <condition_variable>
#include <algorithm>

namespace neo {
class Conpty final : public Backend {
    using Create = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using Resize = HRESULT(WINAPI*)(HPCON, COORD);
    using Close = void(WINAPI*)(HPCON);
    using Clear = HRESULT(WINAPI*)(HPCON, BOOL);
    HMODULE library_ = nullptr;
    Create create_ = nullptr;
    Resize resize_ = nullptr;
    Close close_ = nullptr;
    Clear clear_ = nullptr;
    HPCON console_ = nullptr;
    Handle input_, output_, update_;
    bool bundled_;
    bool child_exiting_ = false;
    std::wstring runtime_;
    std::unique_ptr<Terminal> terminal_;
    std::mutex state_mutex_, queue_mutex_, console_mutex_;
    std::condition_variable wake_;
    std::condition_variable state_ready_;
    std::atomic<unsigned> snapshots_waiting_{0};
    std::deque<std::string> pending_;
    size_t queued_ = 0;
    std::thread reader_, writer_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> dirty_{true};
    std::atomic<bool> output_ended_{false};
    std::atomic<bool> reader_done_{false}, writer_done_{false};
    std::string failure_;
    std::vector<unsigned char> attributes_;

    void enqueue(std::string bytes) {
        if (bytes.empty() || stopping_) {
            return;
        }
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queued_ + bytes.size() > max_frame) {
            failure_ = "Terminal input queue exceeds 4MiB";
            SetEvent(update_.get());
            return;
        }
        queued_ += bytes.size();
        pending_.push_back(std::move(bytes));
        wake_.notify_one();
    }

    void read_output() {
        char bytes[16384];
        DWORD length;
        while (!stopping_ && ReadFile(output_.get(), bytes, sizeof(bytes), &length, nullptr) &&
               length) {
            std::unique_lock<std::mutex> lock(state_mutex_);
            // A continuously readable pipe must not starve the snapshot worker.
            state_ready_.wait(lock, [this] { return !snapshots_waiting_ || stopping_; });
            if (stopping_) { break; }
            terminal_->feed(std::string_view(bytes, length));
            dirty_ = true;
            SetEvent(update_.get());
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        output_ended_ = true;
        dirty_ = true;
        SetEvent(update_.get());
    }

    void write_input() {
        while (!stopping_) {
            std::string bytes;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                wake_.wait(lock, [&] {
                    return stopping_ || !pending_.empty();
                });
                if (stopping_) {
                    return;
                }
                bytes = std::move(pending_.front());
                pending_.pop_front();
                queued_ -= bytes.size();
            }
            size_t position = 0;
            while (position < bytes.size() && !stopping_) {
                DWORD written = 0;
                if (!WriteFile(input_.get(),
                               bytes.data() + position,
                               static_cast<DWORD>(bytes.size() - position),
                               &written,
                               nullptr) ||
                    !written) {
                    return;
                }
                position += written;
            }
        }
    }

public:
    Conpty(const Options& options, bool bundled) : Backend(options), bundled_(bundled) {
    }

    ~Conpty() override {
        stop();
        if (!attributes_.empty()) {
            DeleteProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.data()));
        }
        if (library_) {
            FreeLibrary(library_);
        }
    }

    void initialize() override {
        if (bundled_) {
            auto path = options_.runtime_directory;
            if (path.empty()) {
                path = executable_path();
                path.resize(path.find_last_of(L"\\/"));
                path += L"\\runtime";
            }
            runtime_ = path + L"\\conpty.dll";
        } else {
            wchar_t directory[MAX_PATH];
            wincheck(GetSystemDirectoryW(directory, MAX_PATH) != 0, "GetSystemDirectoryW");
            runtime_ = std::wstring(directory) + L"\\kernel32.dll";
        }
        library_ = LoadLibraryExW(runtime_.c_str(),
                                  nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        wincheck(library_ != nullptr, "LoadLibraryExW");
        create_ = reinterpret_cast<Create>(GetProcAddress(
            library_, bundled_ ? "ConptyCreatePseudoConsole" : "CreatePseudoConsole"));
        resize_ = reinterpret_cast<Resize>(GetProcAddress(
            library_, bundled_ ? "ConptyResizePseudoConsole" : "ResizePseudoConsole"));
        close_ = reinterpret_cast<Close>(
            GetProcAddress(library_, bundled_ ? "ConptyClosePseudoConsole" : "ClosePseudoConsole"));
        clear_ = reinterpret_cast<Clear>(
            GetProcAddress(library_, bundled_ ? "ConptyClearPseudoConsole" : "ClearPseudoConsole"));
        if (!create_ || !resize_ || !close_) {
            throw std::runtime_error("ConPTY API unavailable");
        }
        HANDLE input_read, input_write, output_read, output_write;
        wincheck(CreatePipe(&input_read, &input_write, nullptr, 65536), "CreatePipe input");
        Handle incoming(input_read);
        input_.reset(input_write);
        wincheck(CreatePipe(&output_read, &output_write, nullptr, 65536), "CreatePipe output");
        output_.reset(output_read);
        Handle outgoing(output_write);
        const auto result =
            create_({static_cast<SHORT>(options_.cols), static_cast<SHORT>(options_.rows)},
                    incoming.get(),
                    outgoing.get(),
                    0,
                    &console_);
        if (FAILED(result)) {
            throw std::runtime_error("ConPTY initialization failed (HRESULT " +
                                     std::to_string(static_cast<unsigned long>(result)) + ')');
        }
        update_.reset(CreateEventW(nullptr, TRUE, TRUE, nullptr));
        wincheck(update_.get() != nullptr, "Create terminal update event");
        terminal_ = std::make_unique<Terminal>(options_.cols, options_.rows,
            [this](const std::string& bytes) { enqueue(bytes); });
        reader_ = std::thread([this] {
            try {
                read_output();
            } catch (const std::exception& error) {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                failure_ = error.what();
                dirty_ = true;
                output_ended_ = true;
                SetEvent(update_.get());
            }
            reader_done_ = true;
        });
        writer_ = std::thread([this] {
            write_input();
            writer_done_ = true;
        });
    }

    void launch() override {
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes_.resize(size);
        startup.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_.data());
        wincheck(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size),
                 "InitializeProcThreadAttributeList");
        wincheck(UpdateProcThreadAttribute(startup.lpAttributeList,
                                           0,
                                           PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                           console_,
                                           sizeof(console_),
                                           nullptr,
                                           nullptr),
                 "UpdateProcThreadAttribute");
        spawn(startup, EXTENDED_STARTUPINFO_PRESENT);
    }

    void command(const Command& value) override {
        std::lock_guard<std::mutex> console_lock(console_mutex_);
        if (!console_) { return; }
        if (value.type == 'L') {
            wincheck(clear_ != nullptr, "Screen clear requires the ConPTY clear API");
            wincheck(SUCCEEDED(clear_(console_, FALSE)), "Clear pseudoconsole");
            std::lock_guard<std::mutex> lock(state_mutex_);
            terminal_->clear_screen();
            dirty_ = true;
            SetEvent(update_.get());
            return;
        }
        if (value.type == 'R') {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                terminal_->resize(value.first, value.second);
                dirty_ = true;
                SetEvent(update_.get());
            }
            wincheck(SUCCEEDED(resize_(console_, {static_cast<SHORT>(value.first),
                                                 static_cast<SHORT>(value.second)})),
                     "ConPTY resize");
            return;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        terminal_->command(value);
        if (terminal_->has_updates()) {
            dirty_ = true;
            SetEvent(update_.get());
        }
    }

    void shell_notification(const std::string& message) override {
        std::lock_guard<std::mutex> lock(state_mutex_);
        terminal_->shell_notification(message);
        if (terminal_->has_updates()) {
            dirty_ = true;
            SetEvent(update_.get());
        }
    }

    bool supports_clear() const override { return clear_ != nullptr; }

    bool has_updates() override {
        if (dirty_) { return true; }
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return !failure_.empty();
    }

    void wait_for_update(DWORD timeout, HANDLE interrupt = nullptr,
                         const std::vector<HANDLE>& additional = {}) override {
        if (!has_updates()) {
            std::vector<HANDLE> events{update_.get()};
            if (interrupt) { events.push_back(interrupt); }
            if (!child_exiting_ && process_.get()) { events.push_back(process_.get()); }
            events.insert(events.end(), additional.begin(), additional.end());
            wincheck(WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(),
                                           FALSE, timeout) != WAIT_FAILED,
                     "Wait for terminal, input, process or shell event");
        }
    }

    void begin_exit() override {
        std::lock_guard<std::mutex> console_lock(console_mutex_);
        child_exiting_ = true;
        Backend::begin_exit();
        if (console_) { close_(console_); console_ = nullptr; }
    }

    bool output_finished() override { return output_ended_ && !has_updates(); }

    Screen snapshot() override {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!failure_.empty()) { throw std::runtime_error(failure_); }
        }
        ++snapshots_waiting_;
        std::lock_guard<std::mutex> lock(state_mutex_);
        struct SnapshotDone {
            Conpty& owner;
            ~SnapshotDone() {
                --owner.snapshots_waiting_;
                owner.state_ready_.notify_all();
            }
        } done{*this};
        auto result = terminal_->snapshot();
        dirty_ = terminal_->has_updates();
        if (!dirty_) { ResetEvent(update_.get()); }
        return result;
    }

    void stop() override {
        {
            // Share the predicate mutex with write_input's condition-variable wait.
            std::scoped_lock lock(queue_mutex_, state_mutex_);
            if (stopping_.exchange(true)) { return; }
        }
        Backend::stop();
        wake_.notify_all();
        state_ready_.notify_all();
        if (writer_.joinable()) {
            while (!writer_done_) {
                CancelSynchronousIo(writer_.native_handle());
                Sleep(2);
            }
            writer_.join();
        }
        if (reader_.joinable()) {
            while (!reader_done_) {
                CancelSynchronousIo(reader_.native_handle());
                Sleep(2);
            }
            reader_.join();
        }
        input_.reset();
        output_.reset();
        if (console_) {
            close_(console_);
            console_ = nullptr;
        }
    }

    std::string runtime() const override {
        return utf8(runtime_);
    }
};

std::unique_ptr<Backend> make_conpty(const Options& options, bool bundled) {
    return std::make_unique<Conpty>(options, bundled);
}
}
