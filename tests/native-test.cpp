#include <iostream>
#include <stdexcept>
#if __has_include("protocol.hpp") && __has_include("options.hpp")
#include "protocol.hpp"
#include "options.hpp"
#include "screen-encoder.hpp"

void require(bool value, const char* specification) {
    if (!value) {
        throw std::runtime_error(specification);
    }
}

int main() try {
    using namespace neo;
    ScreenEncoder encoder;
    Screen screen;
    screen.cols = 2;
    screen.rows = 2;
    screen.incremental = true;
    screen.changed_rows = {0, 1};
    screen.lines = {{Cell{"a"}}, {Cell{"b"}}};
    screen.continuations = {false, false};
    screen.content_widths = {1, 1};
    const auto initial = encoder.encode(screen);
    require(initial.find("\"rows\":[[0,[[\"a\"") != std::string::npos &&
                initial.find("[1,[[\"b\"") != std::string::npos,
            "initial dirty snapshot sends every supplied row");
    screen.changed_rows = {1};
    screen.lines[0] = std::vector<Cell>(4096, Cell{std::string(1024, 'z')});
    screen.lines[1][0].text = "c";
    const auto dirty = encoder.encode(screen);
    require(dirty.find("\"rows\":[[1,[[\"c\"") != std::string::npos &&
                dirty.size() < 1024 && dirty.find("zzzz") == std::string::npos,
            "unchanged populated rows are never serialized in incremental snapshots");
    screen.changed_rows.clear();
    screen.lines[1].clear();
    screen.x = 1;
    const auto cursor_only = encoder.encode(screen);
    require(cursor_only.find("\"rows\":[]") != std::string::npos &&
                cursor_only.find("\"x\":1") != std::string::npos,
            "cursor updates do not read or emit unchanged cells");
    require(encoder.encode(screen).empty(), "unchanged incremental state emits no frame");
    screen.incremental = false;
    screen.lines = {{Cell{"a"}}, {Cell{"c"}}};
    require(encoder.encode(screen).empty(),
            "incremental snapshots preserve cached contents of unchanged rows");
    screen.incremental = true;
    screen.cols = 3;
    screen.changed_rows = {0, 1};
    const auto resized = encoder.encode(screen);
    require(resized.find("\"cols\":3") != std::string::npos &&
                resized.find("\"rows\":[[0,") != std::string::npos &&
                resized.find("[1,[[\"c\"") != std::string::npos,
            "resize invalidates row cache even when cells remain the same");
    for (const auto invalid_index : {-1, 2}) {
        screen.changed_rows = {invalid_index};
        bool invalid_rejected = false;
        try {
            encoder.encode(screen);
        } catch (const std::exception&) {
            invalid_rejected = true;
        }
        require(invalid_rejected, "dirty indices outside the screen are rejected");
    }
    screen.changed_rows = {1};
    screen.lines.resize(1);
    bool missing_rejected = false;
    try {
        encoder.encode(screen);
    } catch (const std::exception&) {
        missing_rejected = true;
    }
    require(missing_rejected, "dirty rows without supplied cell vectors are rejected");
    screen.lines.resize(2);
    screen.lines[0] = {Cell{"a"}};
    screen.lines[1] = {Cell{"c"}};
    screen.changed_rows = {1, 1};
    require(encoder.encode(screen).empty(), "identical duplicate dirty rows emit no frame");
    screen.changed_rows.clear();
    screen.continuations[1] = true;
    screen.content_widths[1] = 2;
    const auto metadata = encoder.encode(screen);
    require(metadata.find("\"rows\":[]") != std::string::npos &&
                metadata.find("\"continuations\":[false,true]") != std::string::npos &&
                metadata.find("\"content_widths\":[1,2]") != std::string::npos,
            "row metadata can change independently of cell contents");
    screen.history = {"scrollback"};
    screen.history_rows = {{{Cell{"old"}}, true}};
    screen.history_cleared = true;
    screen.clipboard = {"copied"};
    screen.shell_events = {"prompt"};
    const auto events = encoder.encode(screen);
    require(events.find("\"rows\":[]") != std::string::npos &&
                events.find("\"history\":[\"scrollback\"]") != std::string::npos &&
                events.find("\"history_rows\":[[[[\"old\",1,-1,-1,0,0]],true]]") != std::string::npos &&
                events.find("\"history_cleared\":true") != std::string::npos &&
                events.find("\"clipboard\":[\"copied\"]") != std::string::npos &&
                events.find("\"shell_events\":[\"prompt\"]") != std::string::npos,
            "incremental frames preserve history, clipboard and shell events without dirty cells");
    ScreenEncoder full_encoder;
    Screen full;
    full.cols = 1;
    full.rows = 1;
    full.lines = {{Cell{"legacy"}}};
    require(full_encoder.encode(full).find("\"rows\":[[0,[[\"legacy\"") != std::string::npos,
            "legacy snapshots serialize all rows without dirty indices");
    require(full_encoder.encode(full).empty(), "unchanged legacy snapshots emit no frame");
    full.lines[0][0].text = "updated";
    require(full_encoder.encode(full).find("\"rows\":[[0,[[\"updated\"") != std::string::npos,
            "legacy snapshots continue detecting changed cells");
    require(candidates(Options{}) == std::vector<BackendKind>{BackendKind::bundled,
                                                              BackendKind::system},
            "auto only considers bundled and system ConPTY");
    Options restricted;
    restricted.no_conpty = true;
    bool rejected = false;
    try {
        candidates(restricted);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "disabling ConPTY cannot select an EXE backend");
    require(quote_argument(L"a b\\") == L"\"a b\\\\\"",
            "trailing backslashes survive Windows argument quoting");
    require(quote_argument(L"a\"b") == L"\"a\\\"b\"",
            "literal quotes survive Windows argument quoting");
    require(utf8(wide("日本語😀")) == "日本語😀", "Unicode text round trips");
    const auto frame = encode_frame("hello");
    require(frame.size() == 9 && frame[0] == 5, "frame length counts bytes");
    require(frame_length(frame.data()) == 5, "frame length is little endian");
    const char too_large[4] = {0, 0, 0x40, 1};
    rejected = false;
    try {
        frame_length(too_large);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "oversized frames are rejected before allocation");
    require(parse_command("R80,24").first == 80, "resize preserves requested columns");
    require(parse_command("L").type == 'L', "screen clear has no shell text");
    require(parse_command("H").type == 'H', "history clear is independent from screen clear");
    require(parse_command("M2,4,-1,7").third == -1,
            "mouse release preserves position, button and modifiers");
    for (const auto& invalid :
         {"M200,0,1,0", "M0,300,1,0", "M0,0,6,0", "M0,0,1,8", "M0,0,1", "M0,0,1,0,0"}) {
        rejected = false;
        try {
            parse_command(invalid);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "mouse fields must be complete and within terminal bounds");
    }
    rejected = false;
    try {
        parse_command("Lcls");
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "clear commands reject unexpected payloads");
    rejected = false;
    try {
        parse_command("R0,24");
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "zero terminal dimensions are rejected");
    rejected = false;
    try {
        parse_command("K5,0junk");
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "numeric fields cannot contain trailing data");
    require(json_string("\n\"\\") == "\"\\n\\\"\\\\\"", "JSON escapes preserve control text");
    std::cout << "NATIVE_TESTS=PASS\n";
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
#else
int main() {
    std::cerr << "FAIL: backend selection and communication contracts are absent\n";
    return 1;
}
#endif
