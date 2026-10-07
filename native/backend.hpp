#pragma once
#include "options.hpp"
#include <memory>
#include <mutex>
#include <deque>
#include <thread>
#include <atomic>
#include <sstream>

namespace neo {
inline void wincheck(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::string(operation) + " (Win32 " + std::to_string(GetLastError()) + ')');
}
class Handle {
    HANDLE value_ = nullptr;
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept { reset(other.release()); return *this; }
    HANDLE get() const { return value_; }
    HANDLE release() { const auto value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); value_ = value; }
};
inline std::wstring executable_path() {
    std::wstring result(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    wincheck(length && length < result.size(), "GetModuleFileNameW");
    result.resize(length);
    return result;
}
inline std::wstring resolve_program(const std::wstring& program) {
    std::wstring path(32768, L'\0');
    const auto length = SearchPathW(nullptr, program.c_str(), L".exe", static_cast<DWORD>(path.size()), path.data(), nullptr);
    wincheck(length && length < path.size(), "Executable lookup");
    path.resize(length);
    return path;
}
inline std::string scalar(uint32_t character) {
    if (character == 0) return " ";
    if (character > 0x10ffff || (character >= 0xd800 && character <= 0xdfff)) character = 0xfffd;
    std::wstring value;
    if (character <= 0xffff) value += static_cast<wchar_t>(character);
    else { character -= 0x10000; value += static_cast<wchar_t>(0xd800 + (character >> 10)); value += static_cast<wchar_t>(0xdc00 + (character & 1023)); }
    return utf8(value);
}
struct Cell {
    std::string text = " ";
    int width = 1;
    int fg = -1;
    int bg = -1;
    int attributes = 0;
};
inline std::string cell_json(const Cell& cell) {
    return '[' + json_string(cell.text) + ',' + std::to_string(cell.width) + ',' + std::to_string(cell.fg) + ',' + std::to_string(cell.bg) + ',' + std::to_string(cell.attributes) + ']';
}
struct Screen {
    int cols = 80, rows = 24, x = 0, y = 0;
    bool visible = true, alt = false;
    std::vector<std::vector<Cell>> lines;
    std::vector<std::string> history;
};
class Backend {
protected:
    Options options_;
    Handle job_, process_;
    void spawn(STARTUPINFOEXW& startup, DWORD flags, bool inherit = false) {
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        wincheck(job_.get() != nullptr, "CreateJobObjectW");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        wincheck(SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit)), "SetInformationJobObject");
        const auto program = resolve_program(options_.program.front());
        std::wstring command = quote_argument(program);
        for (size_t index = 1; index < options_.program.size(); ++index) command += L' ' + quote_argument(options_.program[index]);
        PROCESS_INFORMATION information{};
        wincheck(CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, inherit, flags | CREATE_SUSPENDED, nullptr, nullptr, &startup.StartupInfo, &information), "CreateProcessW");
        process_.reset(information.hProcess);
        Handle thread(information.hThread);
        if (!AssignProcessToJobObject(job_.get(), process_.get())) { const auto error = GetLastError(); TerminateProcess(process_.get(), 1); SetLastError(error); wincheck(false, "AssignProcessToJobObject"); }
        wincheck(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "ResumeThread");
    }
public:
    explicit Backend(Options options) : options_(std::move(options)) {}
    virtual ~Backend() = default;
    virtual void initialize() = 0;
    virtual void launch() = 0;
    virtual Screen snapshot() = 0;
    virtual void command(const Command& command) = 0;
    virtual void stop() { if (job_.get()) TerminateJobObject(job_.get(), 0); }
    virtual std::string runtime() const = 0;
    bool exited() const { return process_.get() && WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0; }
    DWORD exit_code() const { DWORD code = 0; wincheck(GetExitCodeProcess(process_.get(), &code), "GetExitCodeProcess"); return code; }
    DWORD pid() const { return GetProcessId(process_.get()); }
};
std::unique_ptr<Backend> make_conpty(const Options& options, bool bundled);
std::unique_ptr<Backend> make_classic(const Options& options);
}
