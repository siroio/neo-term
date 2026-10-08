#pragma once
#include "options.hpp"
#include <memory>
#include <mutex>
#include <deque>
#include <thread>
#include <atomic>
#include <sstream>
#include <utility>
#include <algorithm>

namespace neo {
inline void wincheck(bool success, const char* operation) {
    if (!success) {
        throw std::runtime_error(std::string(operation) + " (Win32 " +
                                 std::to_string(GetLastError()) + ')');
    }
}

class Handle {
    HANDLE value_ = nullptr;

public:
    Handle() = default;

    explicit Handle(HANDLE value) : value_(value) {
    }

    ~Handle() {
        reset();
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : value_(other.release()) {
    }

    Handle& operator=(Handle&& other) noexcept {
        reset(other.release());
        return *this;
    }

    HANDLE get() const {
        return value_;
    }

    HANDLE release() {
        const auto value = value_;
        value_ = nullptr;
        return value;
    }

    void reset(HANDLE value = nullptr) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = value;
    }
};

inline std::wstring executable_path() {
    std::wstring result(32768, L'\0');
    const auto length =
        GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    wincheck(length && length < result.size(), "GetModuleFileNameW");
    result.resize(length);
    return result;
}

inline std::wstring resolve_program(std::wstring program, const std::wstring& directory = {},
                                    const std::vector<std::wstring>& environment = {}) {
    std::wstring search;
    if (!directory.empty()) {
        if (program.find_first_of(L"/\\") != std::wstring::npos &&
            program.front() != L'/' && program.front() != L'\\' &&
            (program.size() < 2 || program[1] != L':')) {
            program = directory + L"/" + program;
        }
        wchar_t system[MAX_PATH];
        wincheck(GetSystemDirectoryW(system, MAX_PATH) != 0, "Get system directory");
        search = directory + L';' + system;
        for (const auto& variable : environment) {
            if (_wcsnicmp(variable.c_str(), L"PATH=", 5) == 0) {
                search += L';' + variable.substr(5);
                break;
            }
        }
    }
    std::wstring path(32768, L'\0');
    const auto length = SearchPathW(
        search.empty() ? nullptr : search.c_str(), program.c_str(), L".exe",
        static_cast<DWORD>(path.size()), path.data(), nullptr);
    wincheck(length && length < path.size(), "Executable lookup");
    path.resize(length);
    return path;
}

inline std::string scalar(uint32_t character) {
    if (character == 0) {
        return " ";
    }
    if (character > 0x10ffff || (character >= 0xd800 && character <= 0xdfff)) {
        character = 0xfffd;
    }
    std::wstring value;
    if (character <= 0xffff) {
        value += static_cast<wchar_t>(character);
    } else {
        character -= 0x10000;
        value += static_cast<wchar_t>(0xd800 + (character >> 10));
        value += static_cast<wchar_t>(0xdc00 + (character & 1023));
    }
    return utf8(value);
}

struct Cell {
    std::string text = " ";
    int width = 1;
    int fg = -1;
    int bg = -1;
    int attributes = 0;
    int prompt = 0;
};

inline std::string cell_json(const Cell& cell) {
    return '[' + json_string(cell.text) + ',' + std::to_string(cell.width) + ',' +
           std::to_string(cell.fg) + ',' + std::to_string(cell.bg) + ',' +
           std::to_string(cell.attributes) + ',' + std::to_string(cell.prompt) + ']';
}

struct Screen {
    int cols = 80;
    int rows = 24;
    int x = 0;
    int y = 0;
    bool visible = true;
    int cursor_shape = 1;
    bool cursor_blink = true;
    bool alt = false;
    bool history_cleared = false;
    std::string title;
    int mouse = 0;
    std::vector<std::string> clipboard;
    std::vector<std::string> shell_events;
    std::vector<std::vector<Cell>> lines;
    std::vector<std::string> history;

    struct HistoryRow {
        std::vector<Cell> cells;
        bool continuation = false;
    };

    std::vector<HistoryRow> history_rows;
    std::vector<bool> continuations;
    std::vector<int> content_widths;
};

class Backend {
protected:
    Options options_;
    Handle job_;
    Handle process_;

    void spawn(STARTUPINFOEXW& startup, DWORD flags, bool inherit = false) {
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        wincheck(job_.get() != nullptr, "CreateJobObjectW");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        wincheck(SetInformationJobObject(
                     job_.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit)),
                 "SetInformationJobObject");
        const auto program = resolve_program(options_.program.front(), options_.directory,
                                             options_.environment);
        std::wstring command = quote_argument(program);
        for (size_t index = 1; index < options_.program.size(); ++index) {
            command += L' ' + quote_argument(options_.program[index]);
        }
        PROCESS_INFORMATION information{};
        std::wstring environment;
        for (const auto& variable : options_.environment) {
            environment += variable;
            environment += L'\0';
        }
        environment += L'\0';
        wincheck(CreateProcessW(program.c_str(),
                                command.data(),
                                nullptr,
                                nullptr,
                                inherit,
                                flags | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                                options_.environment.empty() ? nullptr : environment.data(),
                                options_.directory.empty() ? nullptr : options_.directory.c_str(),
                                &startup.StartupInfo,
                                &information),
                 "CreateProcessW");
        process_.reset(information.hProcess);
        Handle thread(information.hThread);
        if (!AssignProcessToJobObject(job_.get(), process_.get())) {
            const auto error = GetLastError();
            TerminateProcess(process_.get(), 1);
            SetLastError(error);
            wincheck(false, "AssignProcessToJobObject");
        }
        wincheck(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "ResumeThread");
    }

public:
    explicit Backend(Options options) : options_(std::move(options)) {
    }

    virtual ~Backend() = default;
    virtual void initialize() = 0;
    virtual void launch() = 0;
    virtual Screen snapshot() = 0;
    virtual void command(const Command& command) = 0;

    virtual bool supports_clear() const {
        return false;
    }

    virtual bool has_updates() {
        return true;
    }

    virtual void wait_for_update(DWORD timeout, HANDLE interrupt = nullptr) {
        if (interrupt) {
            WaitForSingleObject(interrupt, timeout);
        } else {
            Sleep(timeout);
        }
    }

    void set_shell_environment(const std::wstring& pipe, unsigned short port, const std::string& token) {
        if (options_.environment.empty()) {
            const auto environment = GetEnvironmentStringsW();
            wincheck(environment != nullptr, "Read process environment");
            for (auto entry = environment; *entry; entry += wcslen(entry) + 1) {
                options_.environment.emplace_back(entry);
            }
            FreeEnvironmentStringsW(environment);
        }
        auto& variables = options_.environment;
        variables.erase(std::remove_if(variables.begin(), variables.end(), [](const auto& value) {
            return _wcsnicmp(value.c_str(), L"NEO_TERM_PIPE=", 14) == 0 ||
                   _wcsnicmp(value.c_str(), L"NEO_TERM_PORT=", 14) == 0 ||
                   _wcsnicmp(value.c_str(), L"NEO_TERM_TOKEN=", 15) == 0 ||
                   _wcsnicmp(value.c_str(), L"NEO_TERM_HOST=", 14) == 0;
        }), variables.end());
        variables.push_back(L"NEO_TERM_PIPE=" + pipe);
        variables.push_back(L"NEO_TERM_PORT=" + std::to_wstring(port));
        variables.push_back(L"NEO_TERM_TOKEN=" + wide(token));
        const auto name = [](const std::wstring& value) {
            return value.substr(0, value.find(L'=', 1));
        };
        std::stable_sort(variables.begin(), variables.end(), [&](const auto& left, const auto& right) {
            return _wcsicmp(name(left).c_str(), name(right).c_str()) < 0;
        });
        variables.erase(std::unique(variables.begin(), variables.end(), [&](const auto& left, const auto& right) {
            return _wcsicmp(name(left).c_str(), name(right).c_str()) == 0;
        }), variables.end());
        variables.erase(std::remove_if(variables.begin(), variables.end(), [](const auto& value) {
            return value.find(L'=', 1) == std::wstring::npos;
        }), variables.end());
    }

    virtual void shell_notification(const std::string&) {
    }

    void terminate_children() {
        if (job_.get()) {
            TerminateJobObject(job_.get(), 0);
        }
    }

    virtual void begin_exit() {
        terminate_children();
    }

    virtual bool output_finished() {
        return true;
    }

    virtual void stop() {
        terminate_children();
    }

    virtual std::string runtime() const = 0;

    bool exited() const {
        return process_.get() && WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0;
    }

    DWORD exit_code() const {
        DWORD code = 0;
        wincheck(GetExitCodeProcess(process_.get(), &code), "GetExitCodeProcess");
        return code;
    }

    DWORD pid() const {
        return GetProcessId(process_.get());
    }
};

std::unique_ptr<Backend> make_conpty(const Options& options, bool bundled);
}
