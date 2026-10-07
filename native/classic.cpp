#include "backend.hpp"
#include <algorithm>

namespace neo {
class Classic final : public Backend {
    Handle input_, output_;
    std::mutex mutex_;
    bool attached_ = false;
    std::vector<std::string> previous_;

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
        SMALL_RECT window{0, 0, static_cast<SHORT>(cols - 1), static_cast<SHORT>(rows - 1)};
        wincheck(SetConsoleWindowInfo(output_.get(), TRUE, &window), "SetConsoleWindowInfo");
        options_.cols = cols;
        options_.rows = rows;
        previous_.clear();
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
        if (value.type == 'R') {
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

    Screen snapshot() override {
        std::lock_guard<std::mutex> lock(mutex_);
        CONSOLE_SCREEN_BUFFER_INFOEX information{};
        information.cbSize = sizeof(information);
        wincheck(GetConsoleScreenBufferInfoEx(output_.get(), &information),
                 "GetConsoleScreenBufferInfoEx");
        CONSOLE_CURSOR_INFO cursor{};
        wincheck(GetConsoleCursorInfo(output_.get(), &cursor), "GetConsoleCursorInfo");
        Screen result;
        result.cols = information.srWindow.Right - information.srWindow.Left + 1;
        result.rows = information.srWindow.Bottom - information.srWindow.Top + 1;
        result.x = information.dwCursorPosition.X - information.srWindow.Left;
        result.y = information.dwCursorPosition.Y - information.srWindow.Top;
        result.visible = cursor.bVisible != 0;
        std::vector<CHAR_INFO> source(static_cast<size_t>(result.cols) * result.rows);
        auto region = information.srWindow;
        wincheck(
            ReadConsoleOutputW(output_.get(),
                               source.data(),
                               {static_cast<SHORT>(result.cols), static_cast<SHORT>(result.rows)},
                               {0, 0},
                               &region),
            "ReadConsoleOutputW");
        std::vector<std::string> text_lines;
        for (int row = 0; row < result.rows; ++row) {
            std::vector<Cell> line;
            std::string text;
            for (int col = 0; col < result.cols; ++col) {
                const auto& value = source[static_cast<size_t>(row) * result.cols + col];
                if (value.Attributes & COMMON_LVB_TRAILING_BYTE) {
                    continue;
                }
                Cell cell;
                uint32_t character = value.Char.UnicodeChar;
                cell.width = value.Attributes & COMMON_LVB_LEADING_BYTE ? 2 : 1;
                if (character >= 0xd800 && character <= 0xdbff && col + 1 < result.cols) {
                    const auto low =
                        source[static_cast<size_t>(row) * result.cols + col + 1].Char.UnicodeChar;
                    if (low >= 0xdc00 && low <= 0xdfff) {
                        character = 0x10000 + ((character - 0xd800) << 10) + low - 0xdc00;
                        cell.width = 2;
                        ++col;
                    }
                }
                cell.text = scalar(character);
                cell.fg = rgb(information.ColorTable[value.Attributes & 15]);
                cell.bg = rgb(information.ColorTable[(value.Attributes >> 4) & 15]);
                cell.attributes = (value.Attributes & COMMON_LVB_UNDERSCORE ? 2 : 0) |
                                  (value.Attributes & COMMON_LVB_REVERSE_VIDEO ? 8 : 0);
                text += cell.text;
                line.push_back(std::move(cell));
            }
            text_lines.push_back(std::move(text));
            result.lines.push_back(std::move(line));
        }
        if (previous_.size() == text_lines.size() && !text_lines.empty() &&
            previous_[0] != text_lines[0]) {
            for (size_t shift = 1; shift < previous_.size(); ++shift) {
                if (std::equal(
                        text_lines.begin(), text_lines.end() - shift, previous_.begin() + shift)) {
                    result.history.assign(previous_.begin(), previous_.begin() + shift);
                    break;
                }
            }
        }
        previous_ = std::move(text_lines);
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
