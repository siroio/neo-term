#pragma once
#include "backend.hpp"

namespace neo {
class ScreenEncoder {
    std::vector<std::string> previous_;
    int cols_ = 0;
    int rows_ = 0;
    std::string cursor_;
    uint64_t generation_ = 0;

public:
    std::string encode(const Screen& screen) {
        if (screen.cols <= 0 || screen.rows <= 0 ||
            screen.lines.size() > static_cast<size_t>(screen.rows)) {
            throw std::runtime_error("Invalid screen dimensions or row count");
        }
        if (screen.incremental) {
            for (const auto index : screen.changed_rows) {
                if (index < 0 || index >= screen.rows ||
                    static_cast<size_t>(index) >= screen.lines.size()) {
                    throw std::runtime_error("Dirty screen row is out of bounds");
                }
            }
        }
        const bool resized = cols_ != screen.cols || rows_ != screen.rows;
        if (resized) {
            previous_.assign(screen.rows, {});
            cols_ = screen.cols;
            rows_ = screen.rows;
        }
        std::string rows = "[";
        bool changed = false;
        const auto encode_row = [&](size_t index) {
            std::string line = "[";
            for (const auto& cell : screen.lines[index]) {
                if (line.size() > 1) {
                    line += ',';
                }
                line += cell_json(cell);
            }
            line += ']';
            if (line == previous_[index]) {
                return;
            }
            if (changed) {
                rows += ',';
            }
            rows += '[' + std::to_string(index) + ',' + line + ']';
            previous_[index] = std::move(line);
            changed = true;
        };
        if (screen.incremental) {
            for (const auto index : screen.changed_rows) {
                encode_row(static_cast<size_t>(index));
            }
        } else {
            for (size_t index = 0; index < screen.lines.size(); ++index) {
                encode_row(index);
            }
        }
        rows += ']';
        const auto cursor = "\"x\":" + std::to_string(screen.x) +
                            ",\"y\":" + std::to_string(screen.y) +
                            ",\"visible\":" + (screen.visible ? "true" : "false") +
                            ",\"cursor_shape\":" + std::to_string(screen.cursor_shape) +
                            ",\"cursor_blink\":" + (screen.cursor_blink ? "true" : "false") +
                            ",\"alt\":" + (screen.alt ? "true" : "false") +
                            ",\"mouse\":" + std::to_string(screen.mouse) +
                            ",\"title\":" + json_string(screen.title);
        std::string continuations = "[";
        for (const auto continuation : screen.continuations) {
            if (continuations.size() > 1) {
                continuations += ',';
            }
            continuations += continuation ? "true" : "false";
        }
        continuations += ']';
        std::string content_widths = "[";
        for (const auto width : screen.content_widths) {
            if (content_widths.size() > 1) {
                content_widths += ',';
            }
            content_widths += std::to_string(width);
        }
        content_widths += ']';
        const auto state = cursor + ",\"continuations\":" + continuations +
                           ",\"content_widths\":" + content_widths;
        if (!resized && !changed && state == cursor_ && screen.history.empty() && screen.history_rows.empty() &&
            !screen.history_cleared && screen.clipboard.empty() && screen.shell_events.empty()) {
            return {};
        }
        cursor_ = state;
        std::string history = "[";
        for (const auto& line : screen.history) {
            if (history.size() > 1) {
                history += ',';
            }
            history += json_string(line);
        }
        history += ']';
        std::string history_rows = "[";
        for (const auto& row : screen.history_rows) {
            if (history_rows.size() > 1) {
                history_rows += ',';
            }
            history_rows += "[[";
            bool first = true;
            for (const auto& cell : row.cells) {
                if (!first) {
                    history_rows += ',';
                }
                first = false;
                history_rows += cell_json(cell);
            }
            history_rows += row.continuation ? "],true]" : "],false]";
        }
        history_rows += ']';
        std::string clipboard = "[";
        for (const auto& text : screen.clipboard) {
            if (clipboard.size() > 1) {
                clipboard += ',';
            }
            clipboard += json_string(text);
        }
        clipboard += ']';
        std::string shell_events = "[";
        for (const auto& event : screen.shell_events) {
            if (shell_events.size() > 1) {
                shell_events += ',';
            }
            shell_events += json_string(event);
        }
        shell_events += ']';
        return "{\"type\":\"screen\",\"v\":1,\"generation\":" + std::to_string(++generation_) +
               ",\"cols\":" + std::to_string(screen.cols) +
               ",\"height\":" + std::to_string(screen.rows) + ',' + state + ",\"rows\":" + rows +
               ",\"history\":" + history + ",\"history_rows\":" + history_rows +
               ",\"history_cleared\":" + (screen.history_cleared ? "true" : "false") +
               ",\"clipboard\":" + clipboard + ",\"shell_events\":" + shell_events + '}';
    }
};
}
