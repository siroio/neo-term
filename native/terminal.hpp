#pragma once
#include "backend.hpp"
#include <functional>
#include <string_view>

namespace neo {
// Single owner; ConPTY serializes calls using its state mutex.
class Terminal {
    struct State;
    std::unique_ptr<State> state_;

public:
    Terminal(int cols, int rows, std::function<void(const std::string&)> output);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    void feed(std::string_view bytes);
    void resize(int cols, int rows);
    void clear_screen();
    void command(const Command& command);
    void shell_notification(const std::string& value);
    bool has_updates() const;
    Screen snapshot();
};
}
