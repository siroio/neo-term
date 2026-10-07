#pragma once
#include "protocol.hpp"
#include <vector>

namespace neo {
enum class BackendKind { automatic, bundled, system, classic };

inline std::string backend_name(BackendKind kind) {
    switch (kind) {
    case BackendKind::bundled:
        return "bundled-conpty";
    case BackendKind::system:
        return "system-conpty";
    case BackendKind::classic:
        return "classic";
    default:
        return "auto";
    }
}

struct Options {
    BackendKind kind = BackendKind::automatic;
    bool no_conpty = false;
    int cols = 80;
    int rows = 24;
    std::vector<std::wstring> program;
};

inline std::vector<BackendKind> candidates(const Options& options) {
    if (options.kind != BackendKind::automatic) {
        if (options.no_conpty && options.kind != BackendKind::classic) {
            throw std::runtime_error("Requested ConPTY backend is forbidden");
        }
        return {options.kind};
    }
    if (options.no_conpty) {
        return {BackendKind::classic};
    }
    return {BackendKind::bundled, BackendKind::system, BackendKind::classic};
}

inline Options parse_options(const std::vector<std::wstring>& args) {
    Options result;
    for (size_t index = 0; index < args.size(); ++index) {
        const auto& argument = args[index];
        if (argument == L"--") {
            result.program.assign(args.begin() + index + 1, args.end());
            break;
        }
        if (argument == L"--no-conpty") {
            result.no_conpty = true;
            continue;
        }
        if (index + 1 >= args.size()) {
            throw std::runtime_error("Missing option value");
        }
        const auto& value = args[++index];
        if (argument == L"--cols") {
            result.cols = integer(utf8(value));
        } else if (argument == L"--rows") {
            result.rows = integer(utf8(value));
        } else if (argument == L"--backend") {
            if (value == L"auto") {
                result.kind = BackendKind::automatic;
            } else if (value == L"bundled-conpty") {
                result.kind = BackendKind::bundled;
            } else if (value == L"system-conpty") {
                result.kind = BackendKind::system;
            } else if (value == L"classic") {
                result.kind = BackendKind::classic;
            } else {
                throw std::runtime_error("Unknown backend");
            }
        } else {
            throw std::runtime_error("Unknown option");
        }
    }
    parse_command("R" + std::to_string(result.cols) + ',' + std::to_string(result.rows));
    candidates(result);
    if (result.program.empty()) {
        throw std::runtime_error("Specify an executable after --");
    }
    return result;
}
}
