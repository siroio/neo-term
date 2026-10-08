#include <vterm.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static std::vector<uint32_t> characters(VTerm* terminal) {
    int rows, cols;
    vterm_get_size(terminal, &rows, &cols);
    auto screen = vterm_obtain_screen(terminal);
    std::vector<uint32_t> result;
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            VTermScreenCell cell{};
            vterm_screen_get_cell(screen, {row, col}, &cell);
            if (cell.chars[0] == UINT32_MAX) {
                continue;
            }
            for (const auto character : cell.chars) {
                if (!character) {
                    break;
                }
                result.push_back(character);
            }
        }
    }
    return result;
}

static void require(bool value, const char* specification) {
    if (!value) {
        throw std::runtime_error(specification);
    }
}

int main() {
    auto terminal = vterm_new(2, 4);
    auto screen = vterm_obtain_screen(terminal);
    vterm_set_utf8(terminal, 1);
    vterm_screen_enable_reflow(screen, true);
    vterm_screen_reset(screen, 1);
    const char* text = "ABCDEFGHIJKLMNOPQRST";
    vterm_input_write(terminal, text, std::strlen(text));
    require(vterm_state_get_lineinfo(vterm_obtain_state(terminal), 0)->continuation,
            "live text continues the preceding scrollback row");
    vterm_set_size(terminal, 2, 8);
    for (int column = 0; column < 8; ++column) {
        VTermScreenCell cell{};
        vterm_screen_get_cell(screen, {0, column}, &cell);
        require(cell.chars[0] == static_cast<unsigned char>("MNOPQRST"[column]),
                "widening a continuation at row zero must preserve its text");
    }
    require(vterm_state_get_lineinfo(vterm_obtain_state(terminal), 0)->continuation,
            "widening must preserve the continuation into scrollback");
    vterm_free(terminal);
    terminal = vterm_new(6, 6);
    screen = vterm_obtain_screen(terminal);
    vterm_set_utf8(terminal, 1);
    vterm_screen_enable_reflow(screen, true);
    vterm_screen_reset(screen, 1);
    text = "ABCD\r\nEF\x1b[1;5H";
    vterm_input_write(terminal, text, std::strlen(text));
    vterm_set_size(terminal, 6, 4);
    require(!vterm_state_get_lineinfo(vterm_obtain_state(terminal), 1)->continuation,
            "a cursor at the resized boundary must not join a following hard line");
    require(characters(terminal) == std::vector<uint32_t>{'A', 'B', 'C', 'D', 'E', 'F'},
            "resizing a boundary cursor must preserve the following hard line");
    vterm_free(terminal);
    terminal = vterm_new(6, 4);
    screen = vterm_obtain_screen(terminal);
    vterm_set_utf8(terminal, 1);
    vterm_screen_enable_reflow(screen, true);
    vterm_screen_reset(screen, 1);
    text = "A日B";
    vterm_input_write(terminal, text, std::strlen(text));
    for (const int cols : {2, 3, 4, 7}) {
        vterm_set_size(terminal, 6, cols);
    }
    vterm_input_write(terminal, "Z", 1);
    require(characters(terminal) == std::vector<uint32_t>{'A', 0x65e5, 'B', 'Z'},
            "input after repeated reflow must append without overwriting the final glyph");
    vterm_free(terminal);
    terminal = vterm_new(32, 8);
    screen = vterm_obtain_screen(terminal);
    vterm_set_utf8(terminal, 1);
    vterm_screen_enable_reflow(screen, true);
    vterm_screen_reset(screen, 1);
    text = "\x1b[31;1mA日B😀éF日本語GHI日J";
    vterm_input_write(terminal, text, std::strlen(text));
    const auto original = characters(terminal);
    for (const int cols : {2, 3, 4, 7, 16, 3, 8}) {
        vterm_set_size(terminal, 32, cols);
        require(characters(terminal) == original,
                "repeated shrinking and widening must preserve Unicode and combining text");
        for (int row = 0; row < 32; ++row) {
            int width = 0;
            for (int col = 0; col < cols; ++col) {
                VTermScreenCell value{};
                vterm_screen_get_cell(screen, {row, col}, &value);
                if (value.chars[0] == UINT32_MAX) {
                    require(col > 0, "a wide trailing cell cannot start a row");
                } else {
                    width += value.width;
                    if (value.chars[0]) {
                        require(value.attrs.bold, "glyph attributes survive reflow");
                    }
                }
            }
            require(width == cols, "reflow must leave complete terminal cell widths");
        }
        VTermPos cursor;
        vterm_state_get_cursorpos(vterm_obtain_state(terminal), &cursor);
        require(cursor.row >= 0 && cursor.row < 32 && cursor.col >= 0 && cursor.col < cols,
                "cursor stays inside the resized screen");
    }
    vterm_free(terminal);
    terminal = vterm_new(6, 4);
    screen = vterm_obtain_screen(terminal);
    vterm_set_utf8(terminal, 1);
    vterm_screen_enable_reflow(screen, true);
    vterm_screen_reset(screen, 1);
    text = "A日B";
    vterm_input_write(terminal, text, std::strlen(text));
    vterm_set_size(terminal, 6, 2);
    VTermScreenCell cell{};
    vterm_screen_get_cell(screen, {1, 0}, &cell);
    require(cell.chars[0] == 0x65e5 && cell.width == 2,
            "a wide glyph must move to the next row as one complete cell");
    vterm_screen_get_cell(screen, {2, 0}, &cell);
    require(cell.chars[0] == 'B', "text after a wide glyph must survive reflow");
    vterm_set_size(terminal, 6, 4);
    vterm_screen_get_cell(screen, {0, 1}, &cell);
    require(cell.chars[0] == 0x65e5 && cell.width == 2,
            "widening must reunite a logical line without copying padding");
    vterm_screen_get_cell(screen, {0, 3}, &cell);
    require(cell.chars[0] == 'B', "widening must preserve text after a wide glyph");
    vterm_free(terminal);
    std::cout << "VTERM_REFLOW_TESTS=PASS\n";
}
