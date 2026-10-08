#include "backend.hpp"
#include <algorithm>
#include <map>

namespace neo {
class Classic final : public Backend {
    Handle input_, output_;
    std::mutex mutex_;
    bool attached_ = false;
    std::vector<std::string> previous_;
    std::vector<std::vector<Cell>> previous_cells_;
    int previous_top_ = 0;
    bool history_cleared_ = false;
    std::map<int, int> prompt_marks_;
    std::map<int, wchar_t> prompt_starts_;
    int last_viewport_top_ = 0;
    COORD pending_start_{};
    std::wstring pending_prompt_;

    bool mark_prompt_at(const CONSOLE_SCREEN_BUFFER_INFOEX& info, COORD start) {
        auto position = start;
        bool wrapped = false;
        int previous = position.Y * info.dwSize.X + position.X;
        for (const auto character : pending_prompt_) {
            if (character == L'\r') {
                position.X = 0;
                wrapped = false;
                continue;
            }
            if (character == L'\n') {
                position.X = 0;
                ++position.Y;
                wrapped = false;
                continue;
            }
            if (position.Y >= info.dwSize.Y) {
                return false;
            }
            CHAR_INFO cell{};
            SMALL_RECT region{position.X, position.Y, position.X, position.Y};
            if (!ReadConsoleOutputW(output_.get(), &cell, {1, 1}, {0, 0}, &region) ||
                cell.Char.UnicodeChar != character) {
                return false;
            }
            previous = position.Y * info.dwSize.X + position.X;
            wrapped = false;
            position.X += cell.Attributes & COMMON_LVB_LEADING_BYTE ? 2 : 1;
            if (position.X >= info.dwSize.X) {
                position.X = 0;
                ++position.Y;
                wrapped = true;
            }
        }
        if (start.Y != pending_start_.Y &&
            (position.Y != info.dwCursorPosition.Y || position.X != info.dwCursorPosition.X)) {
            return false;
        }
        const auto begin = start.Y * info.dwSize.X;
        const auto end = (position.Y + 1) * info.dwSize.X;
        prompt_marks_.erase(prompt_marks_.lower_bound(begin), prompt_marks_.lower_bound(end));
        prompt_starts_.erase(prompt_starts_.lower_bound(begin), prompt_starts_.lower_bound(end));
        prompt_marks_[begin + start.X] |= 1;
        prompt_starts_[begin + start.X] = pending_prompt_.front();
        if (wrapped) {
            prompt_marks_[previous] |= 4;
        } else {
            prompt_marks_[position.Y * info.dwSize.X + position.X] |= 2;
        }
        pending_prompt_.clear();
        return true;
    }

    void mark_pending_prompt(const CONSOLE_SCREEN_BUFFER_INFOEX& info) {
        if (pending_prompt_.empty() || pending_start_.Y >= info.dwSize.Y ||
            pending_start_.X >= info.dwSize.X) {
            return;
        }
        if (mark_prompt_at(info, pending_start_)) {
            return;
        }
        const auto maximum_rows =
            static_cast<int>((pending_start_.X + pending_prompt_.size() * 2) / info.dwSize.X) +
            static_cast<int>(std::count(pending_prompt_.begin(), pending_prompt_.end(), L'\n'));
        if (pending_start_.Y + maximum_rows < info.dwSize.Y) {
            return;
        }
        for (int shift = 1; shift <= maximum_rows && shift <= pending_start_.Y; ++shift) {
            auto start = pending_start_;
            start.Y -= static_cast<SHORT>(shift);
            if (mark_prompt_at(info, start)) {
                return;
            }
        }
    }

    static BOOL WINAPI ignore_control(DWORD) {
        return TRUE;
    }

    static wchar_t character_for_key(int key) {
        switch (key) {
        case 1:
            return L'\r';
        case 2:
            return L'\t';
        case 3:
            return L'\b';
        case 4:
            return 27;
        default:
            return 0;
        }
    }

    void resize_console(int cols, int rows) {
        CONSOLE_SCREEN_BUFFER_INFO current{};
        wincheck(GetConsoleScreenBufferInfo(output_.get(), &current), "GetConsoleScreenBufferInfo");
        SMALL_RECT small{0,
                         0,
                         static_cast<SHORT>(std::min(cols,
                                                     static_cast<int>(current.srWindow.Right -
                                                                      current.srWindow.Left + 1)) -
                                            1),
                         static_cast<SHORT>(std::min(rows,
                                                     static_cast<int>(current.srWindow.Bottom -
                                                                      current.srWindow.Top + 1)) -
                                            1)};
        wincheck(SetConsoleWindowInfo(output_.get(), TRUE, &small), "SetConsoleWindowInfo shrink");
        wincheck(SetConsoleScreenBufferSize(
                     output_.get(),
                     {static_cast<SHORT>(cols), static_cast<SHORT>(std::max(rows, 1000))}),
                 "SetConsoleScreenBufferSize");
        const auto top = static_cast<SHORT>(
            std::min(static_cast<int>(current.srWindow.Top), std::max(rows, 1000) - rows));
        SMALL_RECT window{0, top, static_cast<SHORT>(cols - 1), static_cast<SHORT>(top + rows - 1)};
        wincheck(SetConsoleWindowInfo(output_.get(), TRUE, &window), "SetConsoleWindowInfo");
        options_.cols = cols;
        options_.rows = rows;
        previous_.clear();
        std::map<int, int> resized_marks;
        for (const auto& mark : prompt_marks_) {
            const auto x = mark.first % current.dwSize.X;
            const auto y = mark.first / current.dwSize.X;
            if (x < cols && y < std::max(rows, 1000)) {
                resized_marks[y * cols + x] = mark.second;
            }
        }
        prompt_marks_ = std::move(resized_marks);
        std::map<int, wchar_t> resized_starts;
        for (const auto& start : prompt_starts_) {
            const auto x = start.first % current.dwSize.X;
            const auto y = start.first / current.dwSize.X;
            if (x < cols) {
                resized_starts[y * cols + x] = start.second;
            }
        }
        prompt_starts_ = std::move(resized_starts);
        last_viewport_top_ = top;
    }

    void write_key(wchar_t character, WORD virtual_key, DWORD modifiers) {
        INPUT_RECORD records[2]{};
        for (auto& record : records) {
            record.EventType = KEY_EVENT;
            record.Event.KeyEvent.wRepeatCount = 1;
            record.Event.KeyEvent.wVirtualKeyCode = virtual_key;
            record.Event.KeyEvent.wVirtualScanCode =
                static_cast<WORD>(MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC));
            record.Event.KeyEvent.uChar.UnicodeChar = character;
            record.Event.KeyEvent.dwControlKeyState = modifiers;
        }
        records[0].Event.KeyEvent.bKeyDown = TRUE;
        DWORD written = 0;
        wincheck(WriteConsoleInputW(input_.get(), records, 2, &written) && written == 2,
                 "WriteConsoleInputW");
    }

    static int rgb(COLORREF color) {
        return (GetRValue(color) << 16) | (GetGValue(color) << 8) | GetBValue(color);
    }

public:
    explicit Classic(const Options& options) : Backend(options) {
    }

    ~Classic() override {
        stop();
        if (attached_) {
            FreeConsole();
        }
    }

    void initialize() override {
        FreeConsole();
        wincheck(AllocConsole(), "AllocConsole");
        attached_ = true;
        ShowWindow(GetConsoleWindow(), SW_HIDE);
        SetConsoleCtrlHandler(ignore_control, TRUE);
        input_.reset(CreateFileW(L"CONIN$",
                                 GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 0,
                                 nullptr));
        output_.reset(CreateFileW(L"CONOUT$",
                                  GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  0,
                                  nullptr));
        wincheck(input_.get() != INVALID_HANDLE_VALUE && output_.get() != INVALID_HANDLE_VALUE,
                 "Open console handles");
        wincheck(SetConsoleCP(CP_UTF8) && SetConsoleOutputCP(CP_UTF8),
                 "Set console UTF-8 codepage");
        DWORD mode;
        wincheck(GetConsoleMode(input_.get(), &mode), "GetConsoleMode");
        wincheck(
            SetConsoleMode(input_.get(), (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE),
            "SetConsoleMode");
        resize_console(options_.cols, options_.rows);
    }

    void launch() override {
        wincheck(SetHandleInformation(input_.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT),
                 "SetHandleInformation input");
        wincheck(SetHandleInformation(output_.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT),
                 "SetHandleInformation output");
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        wincheck(InitializeProcThreadAttributeList(attributes, 1, 0, &size),
                 "InitializeProcThreadAttributeList");
        HANDLE handles[] = {input_.get(), output_.get()};
        if (!UpdateProcThreadAttribute(attributes,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles,
                                       sizeof(handles),
                                       nullptr,
                                       nullptr)) {
            const auto error = GetLastError();
            DeleteProcThreadAttributeList(attributes);
            SetLastError(error);
            wincheck(false, "UpdateProcThreadAttribute handles");
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input_.get();
        startup.StartupInfo.hStdOutput = output_.get();
        startup.StartupInfo.hStdError = output_.get();
        startup.StartupInfo.wShowWindow = SW_HIDE;
        try {
            spawn(startup, EXTENDED_STARTUPINFO_PRESENT, true);
        } catch (...) {
            DeleteProcThreadAttributeList(attributes);
            throw;
        }
        DeleteProcThreadAttributeList(attributes);
    }

    void command(const Command& value) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (value.type == 'M') {
            return;
        }
        if (value.type == 'L') {
            prompt_marks_.clear();
            prompt_starts_.clear();
            pending_prompt_.clear();
            clear_console(output_.get());
            previous_.clear();
            previous_top_ = 0;
            return;
        }
        if (value.type == 'H') {
            previous_.clear();
            CONSOLE_SCREEN_BUFFER_INFO information{};
            wincheck(GetConsoleScreenBufferInfo(output_.get(), &information),
                     "GetConsoleScreenBufferInfo");
            previous_top_ = information.srWindow.Top;
            history_cleared_ = true;
            return;
        }
        if (value.type == 'R') {
            pending_prompt_.clear();
            resize_console(value.first, value.second);
            return;
        }
        DWORD modifiers = (value.second & 1 ? SHIFT_PRESSED : 0) |
                          (value.second & 2 ? LEFT_ALT_PRESSED : 0) |
                          (value.second & 4 ? LEFT_CTRL_PRESSED : 0);
        if (value.type == 'K') {
            static const WORD keys[] = {0,
                                        VK_RETURN,
                                        VK_TAB,
                                        VK_BACK,
                                        VK_ESCAPE,
                                        VK_UP,
                                        VK_DOWN,
                                        VK_LEFT,
                                        VK_RIGHT,
                                        VK_INSERT,
                                        VK_DELETE,
                                        VK_HOME,
                                        VK_END,
                                        VK_PRIOR,
                                        VK_NEXT};
            const auto key = value.first <= 14 ? keys[value.first]
                                               : static_cast<WORD>(VK_F1 + value.first - 257);
            const wchar_t character = character_for_key(value.first);
            write_key(character, key, modifiers);
            return;
        }
        if (value.type == 'U' && value.second & 4 && (value.first == 'c' || value.first == 'C')) {
            DWORD mode;
            wincheck(GetConsoleMode(input_.get(), &mode), "GetConsoleMode");
            if (mode & ENABLE_PROCESSED_INPUT) {
                wincheck(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0), "GenerateConsoleCtrlEvent");
                return;
            }
        }
        const auto text = wide(value.type == 'U' ? scalar(value.first) : value.text);
        for (auto character : text) {
            WORD key = 0;
            const auto mapped = VkKeyScanW(character);
            if (mapped != -1) {
                key = LOBYTE(mapped);
            }
            if (value.type == 'U' && (value.second & 4) && character >= L'@' && character <= L'z') {
                character &= 31;
            }
            write_key(character, key, modifiers);
        }
    }

    void shell_notification(const std::string& message) override {
        if (message.rfind("prompt;", 0) != 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        const auto first = message.find(';', 7);
        const auto second = first == std::string::npos ? first : message.find(';', first + 1);
        if (second == std::string::npos) {
            return;
        }
        try {
            const auto x = integer(message.substr(7, first - 7));
            const auto y = integer(message.substr(first + 1, second - first - 1));
            if (x < 0 || x >= 300 || y < 0 || y >= 32767) {
                return;
            }
            pending_start_ = {static_cast<SHORT>(x), static_cast<SHORT>(y)};
            pending_prompt_ = wide(message.substr(second + 1));
            if (pending_prompt_.size() > 1024) {
                pending_prompt_.clear();
            }
        } catch (const std::exception&) {
            pending_prompt_.clear();
        }
    }

    Screen snapshot() override {
        std::lock_guard<std::mutex> lock(mutex_);
        CONSOLE_SCREEN_BUFFER_INFOEX information{};
        information.cbSize = sizeof(information);
        wincheck(GetConsoleScreenBufferInfoEx(output_.get(), &information),
                 "GetConsoleScreenBufferInfoEx");
        CONSOLE_CURSOR_INFO cursor{};
        wincheck(GetConsoleCursorInfo(output_.get(), &cursor), "GetConsoleCursorInfo");
        Screen result;
        result.history_cleared = std::exchange(history_cleared_, false);
        std::wstring title(32768, L'\0');
        title.resize(GetConsoleTitleW(title.data(), static_cast<DWORD>(title.size())));
        result.title = utf8(title);
        result.cols = information.srWindow.Right - information.srWindow.Left + 1;
        result.rows = information.srWindow.Bottom - information.srWindow.Top + 1;
        result.x = information.dwCursorPosition.X - information.srWindow.Left;
        result.y = information.dwCursorPosition.Y - information.srWindow.Top;
        result.visible = cursor.bVisible != 0;
        result.cursor_shape = cursor.dwSize >= 50 ? 1 : 2;
        result.cursor_blink = GetCaretBlinkTime() != INFINITE && GetCaretBlinkTime() != 0;
        if (information.srWindow.Top < last_viewport_top_) {
            prompt_marks_.clear();
            prompt_starts_.clear();
        }
        last_viewport_top_ = information.srWindow.Top;
        result.continuations.assign(result.rows, false);
        result.content_widths.assign(result.rows, result.cols);
        auto read_region = [&](SMALL_RECT region, bool annotate) {
            const int height = region.Bottom - region.Top + 1;
            std::vector<CHAR_INFO> source(static_cast<size_t>(result.cols) * height);
            wincheck(
                ReadConsoleOutputW(output_.get(),
                                   source.data(),
                                   {static_cast<SHORT>(result.cols), static_cast<SHORT>(height)},
                                   {0, 0},
                                   &region),
                "ReadConsoleOutputW");
            std::vector<std::vector<Cell>> lines;
            for (int row = 0; row < height; ++row) {
                std::vector<Cell> line;
                for (int col = 0; col < result.cols; ++col) {
                    const auto cell_column = col;
                    const auto& value = source[static_cast<size_t>(row) * result.cols + col];
                    if (value.Attributes & COMMON_LVB_TRAILING_BYTE) {
                        continue;
                    }
                    Cell cell;
                    uint32_t character = value.Char.UnicodeChar;
                    cell.width = value.Attributes & COMMON_LVB_LEADING_BYTE ? 2 : 1;
                    if (character >= 0xd800 && character <= 0xdbff && col + 1 < result.cols) {
                        const auto low = source[static_cast<size_t>(row) * result.cols + col + 1]
                                             .Char.UnicodeChar;
                        if (low >= 0xdc00 && low <= 0xdfff) {
                            character = 0x10000 + ((character - 0xd800) << 10) + low - 0xdc00;
                            cell.width = 2;
                            ++col;
                        }
                    }
                    cell.text = scalar(character);
                    const auto index =
                        (region.Top + row) * information.dwSize.X + region.Left + cell_column;
                    auto mark = prompt_marks_.find(index);
                    if (annotate && mark != prompt_marks_.end()) {
                        const auto start = prompt_starts_.find(index);
                        if (start != prompt_starts_.end() && start->second != L'\r' &&
                            start->second != L'\n' && start->second != value.Char.UnicodeChar) {
                            prompt_starts_.erase(start);
                            prompt_marks_.erase(mark);
                        } else {
                            cell.prompt = mark->second;
                        }
                    }
                    cell.fg = rgb(information.ColorTable[value.Attributes & 15]);
                    cell.bg = rgb(information.ColorTable[(value.Attributes >> 4) & 15]);
                    cell.attributes = (value.Attributes & COMMON_LVB_UNDERSCORE ? 2 : 0) |
                                      (value.Attributes & COMMON_LVB_REVERSE_VIDEO ? 8 : 0);
                    line.push_back(std::move(cell));
                }
                lines.push_back(std::move(line));
            }
            return lines;
        };
        result.lines = read_region(information.srWindow, false);
        std::vector<std::string> text_lines;
        for (const auto& line : result.lines) {
            std::string text;
            for (const auto& cell : line) {
                text += cell.text;
            }
            text_lines.push_back(std::move(text));
        }
        const int moved = information.srWindow.Top - previous_top_;
        auto append_history = [&](std::vector<Cell> cells) {
            std::string text;
            for (const auto& cell : cells) {
                text += cell.text;
            }
            result.history.push_back(std::move(text));
            const auto background =
                rgb(information.ColorTable[(information.wAttributes >> 4) & 15]);
            while (!cells.empty() && cells.back().text == " " && cells.back().bg == background &&
                   cells.back().attributes == 0 && cells.back().prompt == 0) {
                cells.pop_back();
            }
            result.history_rows.push_back({std::move(cells), false});
        };
        if (moved > 0) {
            const int count = std::min(moved, 64);
            auto region = information.srWindow;
            region.Top = static_cast<SHORT>(previous_top_);
            region.Bottom = static_cast<SHORT>(previous_top_ + count - 1);
            for (auto& line : read_region(region, true)) {
                append_history(std::move(line));
            }
            previous_top_ += count;
        } else if (previous_.size() == text_lines.size() && !text_lines.empty() &&
                   previous_[0] != text_lines[0]) {
            for (size_t shift = 1; shift < previous_.size(); ++shift) {
                const auto matched = previous_.size() - shift - (previous_.size() > 2 ? 1 : 0);
                if (matched > 0 && std::equal(text_lines.begin(),
                                              text_lines.begin() + matched,
                                              previous_.begin() + shift)) {
                    for (size_t row = 0; row < shift; ++row) {
                        append_history(previous_cells_[row]);
                    }
                    std::map<int, int> shifted_marks;
                    std::map<int, wchar_t> shifted_starts;
                    const auto distance = static_cast<int>(shift) * information.dwSize.X;
                    for (const auto& mark : prompt_marks_) {
                        if (mark.first >= distance) {
                            shifted_marks[mark.first - distance] = mark.second;
                        }
                    }
                    for (const auto& start : prompt_starts_) {
                        if (start.first >= distance) {
                            shifted_starts[start.first - distance] = start.second;
                        }
                    }
                    prompt_marks_ = std::move(shifted_marks);
                    prompt_starts_ = std::move(shifted_starts);
                    break;
                }
            }
        }
        mark_pending_prompt(information);
        result.lines = read_region(information.srWindow, true);
        previous_ = std::move(text_lines);
        previous_cells_ = result.lines;
        return result;
    }

    std::string runtime() const override {
        return "Windows classic console / UTF-8 codepage / 16-color cells";
    }
};

std::unique_ptr<Backend> make_classic(const Options& options) {
    return std::make_unique<Classic>(options);
}
}
