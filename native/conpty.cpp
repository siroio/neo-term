#include "backend.hpp"
#include <vterm.h>
#include <condition_variable>
#include <array>
#include <cstdlib>

namespace neo {
class Conpty final : public Backend {
    using Create = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using Resize = HRESULT(WINAPI*)(HPCON, COORD);
    using Close = void(WINAPI*)(HPCON);
    HMODULE library_ = nullptr;
    Create create_ = nullptr;
    Resize resize_ = nullptr;
    Close close_ = nullptr;
    HPCON console_ = nullptr;
    Handle input_, output_;
    bool bundled_;
    std::wstring runtime_;
    VTerm* terminal_ = nullptr;
    VTermScreen* screen_ = nullptr;
    std::mutex state_mutex_, queue_mutex_;
    std::condition_variable wake_;
    std::deque<std::string> pending_;
    size_t queued_ = 0;
    std::deque<std::string> history_;
    std::thread reader_, writer_;
    std::atomic<bool> stopping_{false};
    std::string failure_;
    bool visible_ = true, alt_ = false;
    bool history_cleared_ = false;
    std::string title_, pending_title_;
    bool title_overflow_ = false;
    int mouse_ = 0;
    std::array<char, 4096> selection_buffer_{};
    std::string pending_selection_;
    bool selection_overflow_ = false;
    std::deque<std::string> clipboard_;
    std::vector<unsigned char> attributes_;

    static void output_callback(const char* bytes, size_t length, void* user) {
        static_cast<Conpty*>(user)->enqueue(std::string(bytes, length));
    }

    void enqueue(std::string bytes) {
        if (bytes.empty() || stopping_) {
            return;
        }
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (queued_ + bytes.size() > max_frame) {
            failure_ = "Terminal input queue exceeds 4MiB";
            return;
        }
        queued_ += bytes.size();
        pending_.push_back(std::move(bytes));
        wake_.notify_one();
    }

    static int property_callback(VTermProp property, VTermValue* value, void* user) {
        auto& self = *static_cast<Conpty*>(user);
        if (property == VTERM_PROP_CURSORVISIBLE) {
            self.visible_ = value->boolean != 0;
        }
        if (property == VTERM_PROP_ALTSCREEN) {
            self.alt_ = value->boolean != 0;
        }
        if (property == VTERM_PROP_TITLE) {
            const auto fragment = value->string;
            if (fragment.initial) {
                self.pending_title_.clear();
                self.title_overflow_ = false;
            }
            if (self.pending_title_.size() + fragment.len > 16384) {
                self.title_overflow_ = true;
            }
            if (!self.title_overflow_) {
                self.pending_title_.append(fragment.str, fragment.len);
            }
            if (fragment.final && !self.title_overflow_) {
                self.title_ = self.pending_title_;
            }
        }
        if (property == VTERM_PROP_MOUSE) {
            self.mouse_ = value->number;
        }
        return 1;
    }

    static int selection_callback(VTermSelectionMask, VTermStringFragment fragment, void* user) {
        auto& self = *static_cast<Conpty*>(user);
        if (fragment.initial) {
            self.pending_selection_.clear();
            self.selection_overflow_ = false;
        }
        if (self.pending_selection_.size() + fragment.len > 65536) {
            self.selection_overflow_ = true;
        }
        if (!self.selection_overflow_) {
            self.pending_selection_.append(fragment.str, fragment.len);
        }
        if (fragment.final && !self.selection_overflow_) {
            try {
                wide(self.pending_selection_);
                if (self.clipboard_.size() == 8) {
                    self.clipboard_.pop_front();
                }
                self.clipboard_.push_back(self.pending_selection_);
            } catch (const std::exception&) {
            }
        }
        return 1;
    }

    static int cursor_callback(VTermPos, VTermPos, int visible, void* user) {
        static_cast<Conpty*>(user)->visible_ = visible != 0;
        return 1;
    }

    static int pushline_callback(int cols, const VTermScreenCell* cells, void* user) {
        auto& self = *static_cast<Conpty*>(user);
        std::string line;
        for (int col = 0; col < cols; ++col) {
            if (cells[col].chars[0] == UINT32_MAX) {
                continue;
            }
            for (const auto character : cells[col].chars) {
                if (!character) {
                    break;
                }
                line += scalar(character);
            }
            if (!cells[col].chars[0]) {
                line += ' ';
            }
        }
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        if (self.history_.size() == 2000) {
            self.history_.pop_front();
        }
        self.history_.push_back(std::move(line));
        return 1;
    }

    static int clear_callback(void* user) {
        static_cast<Conpty*>(user)->history_.clear();
        return 1;
    }

    void read_output() {
        char bytes[16384];
        DWORD length;
        while (!stopping_ && ReadFile(output_.get(), bytes, sizeof(bytes), &length, nullptr) &&
               length) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            vterm_input_write(terminal_, bytes, length);
            vterm_screen_flush_damage(screen_);
        }
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

    int color(VTermColor value) {
        if (value.type & VTERM_COLOR_DEFAULT_MASK) {
            return -1;
        }
        vterm_screen_convert_color_to_rgb(screen_, &value);
        return (value.rgb.red << 16) | (value.rgb.green << 8) | value.rgb.blue;
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
        if (terminal_) {
            vterm_free(terminal_);
        }
        if (library_) {
            FreeLibrary(library_);
        }
    }

    void initialize() override {
        if (bundled_) {
            auto path = executable_path();
            path.resize(path.find_last_of(L"\\/"));
            runtime_ = path + L"\\runtime\\conpty.dll";
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
        terminal_ = vterm_new(options_.rows, options_.cols);
        if (!terminal_) {
            throw std::bad_alloc();
        }
        vterm_set_utf8(terminal_, 1);
        vterm_output_set_callback(terminal_, output_callback, this);
        screen_ = vterm_obtain_screen(terminal_);
        vterm_screen_enable_altscreen(screen_, 1);
        static const VTermScreenCallbacks callbacks = {nullptr,
                                                       nullptr,
                                                       cursor_callback,
                                                       property_callback,
                                                       nullptr,
                                                       nullptr,
                                                       pushline_callback,
                                                       nullptr,
                                                       clear_callback};
        vterm_screen_set_callbacks(screen_, &callbacks, this);
        static const VTermSelectionCallbacks selection_callbacks = {selection_callback, nullptr};
        vterm_state_set_selection_callbacks(vterm_obtain_state(terminal_),
                                            &selection_callbacks,
                                            this,
                                            selection_buffer_.data(),
                                            selection_buffer_.size());
        vterm_screen_reset(screen_, 1);
        reader_ = std::thread([this] {
            read_output();
        });
        writer_ = std::thread([this] {
            write_input();
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
        if (value.type == 'L') {
            FreeConsole();
            wincheck(AttachConsole(pid()), "AttachConsole for screen clear");
            try {
                Handle output(CreateFileW(L"CONOUT$",
                                          GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          0,
                                          nullptr));
                wincheck(output.get() != INVALID_HANDLE_VALUE, "Open console for screen clear");
                clear_console(output.get());
            } catch (...) {
                FreeConsole();
                throw;
            }
            FreeConsole();
            return;
        }
        if (value.type == 'R') {
            const auto result = resize_(
                console_, {static_cast<SHORT>(value.first), static_cast<SHORT>(value.second)});
            if (FAILED(result)) {
                throw std::runtime_error("ConPTY resize failed");
            }
            std::lock_guard<std::mutex> lock(state_mutex_);
            vterm_set_size(terminal_, value.second, value.first);
            return;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (value.type == 'M') {
            const auto modifiers = static_cast<VTermModifier>(value.fourth);
            vterm_mouse_move(terminal_, value.first, value.second, modifiers);
            if (value.third != 0) {
                vterm_mouse_button(terminal_, std::abs(value.third), value.third > 0, modifiers);
            }
        } else if (value.type == 'H') {
            history_.clear();
            history_cleared_ = true;
        } else if (value.type == 'K') {
            vterm_keyboard_key(terminal_,
                               static_cast<VTermKey>(value.first),
                               static_cast<VTermModifier>(value.second));
        } else if (value.type == 'U') {
            vterm_keyboard_unichar(
                terminal_, value.first, static_cast<VTermModifier>(value.second));
        } else {
            if (value.type == 'P') {
                vterm_keyboard_start_paste(terminal_);
            }
            enqueue(value.text);
            if (value.type == 'P') {
                vterm_keyboard_end_paste(terminal_);
            }
        }
    }

    Screen snapshot() override {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!failure_.empty()) {
                throw std::runtime_error(failure_);
            }
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        Screen result;
        result.history_cleared = std::exchange(history_cleared_, false);
        result.title = title_;
        result.mouse = mouse_;
        result.clipboard.assign(clipboard_.begin(), clipboard_.end());
        clipboard_.clear();
        vterm_get_size(terminal_, &result.rows, &result.cols);
        VTermPos cursor;
        vterm_state_get_cursorpos(vterm_obtain_state(terminal_), &cursor);
        result.x = cursor.col;
        result.y = cursor.row;
        result.visible = visible_;
        result.alt = alt_;
        for (int row = 0; row < result.rows; ++row) {
            std::vector<Cell> line;
            for (int col = 0; col < result.cols; ++col) {
                VTermScreenCell source{};
                vterm_screen_get_cell(screen_, {row, col}, &source);
                if (source.chars[0] == UINT32_MAX) {
                    continue;
                }
                Cell cell;
                cell.text.clear();
                for (const auto character : source.chars) {
                    if (!character) {
                        break;
                    }
                    cell.text += scalar(character);
                }
                if (cell.text.empty()) {
                    cell.text = " ";
                }
                cell.width = source.width > 0 ? source.width : 1;
                cell.fg = color(source.fg);
                cell.bg = color(source.bg);
                cell.attributes = (source.attrs.bold ? 1 : 0) | (source.attrs.underline ? 2 : 0) |
                                  (source.attrs.italic ? 4 : 0) | (source.attrs.reverse ? 8 : 0) |
                                  (source.attrs.strike ? 16 : 0);
                line.push_back(std::move(cell));
            }
            result.lines.push_back(std::move(line));
        }
        for (int index = 0; index < 64 && !history_.empty(); ++index) {
            result.history.push_back(std::move(history_.front()));
            history_.pop_front();
        }
        return result;
    }

    void stop() override {
        if (stopping_.exchange(true)) {
            return;
        }
        Backend::stop();
        wake_.notify_all();
        if (writer_.joinable()) {
            CancelSynchronousIo(writer_.native_handle());
            writer_.join();
        }
        if (reader_.joinable()) {
            CancelSynchronousIo(reader_.native_handle());
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
