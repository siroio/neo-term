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

static int record_title(VTermProp property, VTermValue* value, void* user) {
    if (property == VTERM_PROP_TITLE) {
        auto& title = *static_cast<std::string*>(user);
        if (value->string.initial) {
            title.clear();
        }
        title.append(value->string.str, value->string.len);
    }
    return 1;
}

int main() {
    auto clear_terminal = vterm_new(3, 10);
    auto clear_screen = vterm_obtain_screen(clear_terminal);
    vterm_set_utf8(clear_terminal, 1);
    vterm_screen_reset(clear_screen, 1);
    const char* partial_color = "old\x1b[31";
    vterm_input_write(clear_terminal, partial_color, std::strlen(partial_color));
    vterm_state_clear_screen(vterm_obtain_state(clear_terminal));
    vterm_input_write(clear_terminal, "mX", 2);
    VTermScreenCell clear_cell{};
    vterm_screen_get_cell(clear_screen, {0, 0}, &clear_cell);
    require(clear_cell.chars[0] == 'X',
            "screen clear preserves a color sequence split between output reads");
    require(clear_cell.fg.type & VTERM_COLOR_INDEXED && clear_cell.fg.indexed.idx == 1,
            "the pending foreground color is applied after screen clear");
    std::string title;
    VTermScreenCallbacks clear_callbacks{};
    clear_callbacks.settermprop = record_title;
    vterm_screen_set_callbacks(clear_screen, &clear_callbacks, &title);
    const char* partial_title = "\x1b]0;partial";
    vterm_input_write(clear_terminal, partial_title, std::strlen(partial_title));
    vterm_state_clear_screen(vterm_obtain_state(clear_terminal));
    vterm_input_write(clear_terminal, "rest\x07Z", 6);
    require(title == "partialrest", "screen clear preserves a title split between output reads");
    vterm_screen_get_cell(clear_screen, {0, 0}, &clear_cell);
    require(clear_cell.chars[0] == 'Z', "title fragments remain control data after screen clear");
    vterm_free(clear_terminal);
    auto unicode_terminal = vterm_new(2, 20);
    auto unicode_screen = vterm_obtain_screen(unicode_terminal);
    vterm_set_utf8(unicode_terminal, 1);
    vterm_screen_reset(unicode_screen, 1);
    const char* unicode_text = "e\u1AB0B";
    vterm_input_write(unicode_terminal, unicode_text, std::strlen(unicode_text));
    VTermPos unicode_cursor{};
    vterm_state_get_cursorpos(vterm_obtain_state(unicode_terminal), &unicode_cursor);
    require(unicode_cursor.col == 2, "new combining marks do not advance the cursor");
    VTermScreenCell unicode_cell{};
    vterm_screen_get_cell(unicode_screen, {0, 0}, &unicode_cell);
    require(unicode_cell.chars[0] == 'e' && unicode_cell.chars[1] == 0x1AB0,
            "new combining marks remain attached to their base character");
    vterm_screen_reset(unicode_screen, 1);
    unicode_text = "A🫩B";
    vterm_input_write(unicode_terminal, unicode_text, std::strlen(unicode_text));
    vterm_state_get_cursorpos(vterm_obtain_state(unicode_terminal), &unicode_cursor);
    vterm_screen_get_cell(unicode_screen, {0, 1}, &unicode_cell);
    require(unicode_cursor.col == 4 && unicode_cell.width == 2,
            "Unicode 16 emoji occupy two terminal cells");
    vterm_screen_reset(unicode_screen, 1);
    unicode_text = "日\u302AB";
    vterm_input_write(unicode_terminal, unicode_text, std::strlen(unicode_text));
    vterm_state_get_cursorpos(vterm_obtain_state(unicode_terminal), &unicode_cursor);
    require(unicode_cursor.col == 3,
            "wide-category combining marks do not consume another cell");
    vterm_free(unicode_terminal);
    auto terminal = vterm_new(2, 4);
    auto screen = vterm_obtain_screen(terminal);
    vterm_set_utf8(terminal, 1);
    vterm_screen_enable_reflow(screen, true);
    vterm_screen_reset(screen, 1);
    vterm_screen_mark_prompt(screen, 1);
    vterm_input_write(terminal, "P> ", 3);
    vterm_screen_mark_prompt(screen, 2);
    vterm_input_write(terminal, "XYZ", 3);
    VTermScreenCell marked{};
    vterm_screen_get_cell(screen, {0, 0}, &marked);
    require(marked.prompt == 1, "the prompt start is attached to its actual cell");
    vterm_set_size(terminal, 2, 8);
    vterm_screen_get_cell(screen, {0, 2}, &marked);
    require(marked.prompt == 4, "the input boundary survives reflow and subsequent input");
    vterm_input_write(terminal, "\rQ", 2);
    vterm_screen_get_cell(screen, {0, 0}, &marked);
    require(marked.prompt == 0, "overwriting a prompt start removes its notification");
    vterm_screen_reset(screen, 1);
    vterm_screen_get_cell(screen, {0, 3}, &marked);
    require(marked.prompt == 0, "erasing a prompt removes its notification");
    vterm_set_size(terminal, 2, 4);
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
