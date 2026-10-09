#include "terminal.hpp"
#include "screen-encoder.hpp"
#include <iostream>
#include <stdexcept>
using namespace neo;

static void require(bool ok, const char* message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

static std::string text(const Screen& s) {
    std::string out;
    for (const auto& r : s.lines) {
        for (const auto& c : r) {
            if (c.text != " ") {
                out += c.text;
            }
        }
    }
    return out;
}

int main() try {
    std::string output;
    Terminal t(10, 4, [&](const std::string& s) {
        output += s;
    });
    require(t.snapshot().changed_rows.size() == 4, "initial rows dirty");
    t.feed("\x1b[2;3H");
    auto s = t.snapshot();
    require(s.changed_rows.empty() && s.x == 2 && s.y == 1, "cursor update has no cell rows");
    t.clear_screen();
    t.snapshot();
    const std::string fragmented = "\x1b[1;38;2;1;2;3mA日e\u1ab0\x1b]0;title\x1b\\";
    for (char c : fragmented) {
        t.feed(std::string_view(&c, 1));
    }
    s = t.snapshot();
    require(text(s) == "A日e\u1ab0" && s.x == 4, "fragmented Unicode combining");
    require(s.lines[0][1].width == 2 && s.lines[0][0].fg == 0x010203 &&
                s.lines[0][0].attributes == 1,
            "RGB bold wide");
    require(s.title == "title", "split OSC terminator");
    for (const auto& base : {std::string("e"), std::string("日")}) {
        Terminal bounded(8, 4, [](const std::string&) {
        });
        std::string input = base, retained = base;
        for (int i = 0; i < 100; ++i) {
            input += "\u1ab0";
        }
        for (int i = 0; i < 15; ++i) {
            retained += "\u1ab0";
        }
        bounded.feed(input + "B");
        auto frame = bounded.snapshot();
        require(frame.lines[0][0].text == retained && frame.lines[0][1].text == "B",
                "cell stores at most16 Unicode scalars and excess combining marks do not consume "
                "ASCII");
        require(frame.x == (base == "e" ? 2 : 3) &&
                    frame.lines[0][0].width == (base == "e" ? 1 : 2),
                "combining cap preserves cursor and base width");
        ScreenEncoder encoder;
        const auto payload = encoder.encode(frame);
        require(payload.find(json_string(retained)) != std::string::npos &&
                    payload.size() < max_frame,
                "bounded combining cell passes ScreenEncoder frame contract");
    }
    t.feed("\x1b[6n");
    require(output == "\x1b[1;5R", "cursor query");
    output.clear();
    t.feed("\x1b[?1h\x1b[?2004h");
    t.command(Command{'K', {}, 5});
    t.command(Command{'P', "abc"});
    require(output == "\x1bOA\x1b[200~abc\x1b[201~", "application arrows and paste");
    output.clear();
    t.command(Command{'K', {}, 3, 4});
    t.command(Command{'K', {}, 2, 5});
    t.command(Command{'U', {}, '1', 4});
    t.command(Command{'U', {}, 'c', 4});
    require(output == "\x1b[127;5u\x1b[1;6Z\x1b[49;5u\x03",
            "modified special and Unicode keys preserve modifiers");
    output.clear();
    t.command(Command{'U', {}, 'i', 4});
    t.command(Command{'U', {}, 'j', 4});
    t.command(Command{'U', {}, 'm', 4});
    t.command(Command{'U', {}, '[', 4});
    t.command(Command{'U', {}, '\\', 4});
    t.command(Command{'U', {}, ' ', 1});
    t.command(Command{'U', {}, 0x65e5, 2});
    t.command(Command{'K', {}, 1, 4});
    require(output == "\x1b[105;5u\x1b[106;5u\x1b[109;5u\x1b[91;5u\x1c\x1b[32;2u\x1b日\x1b[13;5u",
            "ambiguous controls and UTF8 meta preserve key identity");
    t.feed("\x1b[?1049hALT\x1b[?1049l");
    s = t.snapshot();
    require(!s.alt && text(s) == "A日e\u1ab0", "alternate restoration");
    Terminal r(4, 12, [](const std::string&) {
    });
    r.feed("A日B");
    for (int width : {2, 3, 4, 7}) {
        r.resize(width, 12);
    }
    r.feed("Z");
    require(text(r.snapshot()) == "A日BZ", "reflow cursor and wide cells");
    Terminal h(4, 2, [](const std::string&) {
    });
    h.feed("ABCDEFGHIJKLMNOPQRST");
    s = h.snapshot();
    require(s.history_rows.size() == 3 && s.continuations[0], "wrapped history");
    h.resize(8, 2);
    s = h.snapshot();
    require(text(s) == "MNOPQRST" && s.continuations[0], "widen live scrollback continuation");
    Terminal p(8, 3, [](const std::string&) {
    });
    p.feed("\x1b]133;A\x07P> \x1b]133;B\x07X");
    s = p.snapshot();
    require(s.lines[0][0].prompt == 1 && s.lines[0][2].prompt == 4,
            "prompt boundaries survive writing");
    p.feed("\rQ");
    require(p.snapshot().lines[0][0].prompt == 0, "overwrite removes prompt");
    Terminal q(6, 3, [](const std::string&) {
    });
    q.feed("abc\x1b[31");
    q.clear_screen();
    q.feed("mX");
    s = q.snapshot();
    require(text(s) == "X" && s.lines[0][0].fg == 0xcd0000, "clear preserves parser fragment");
    q.feed("\x1b]0;" + std::string(100000, 'a') + "\x07Z");
    require(q.snapshot().title.empty(), "oversized OSC discarded");
    Terminal e(6, 3, [](const std::string&) {
    });
    e.feed("abcdef\r\x1b[2C\x1b[2P");
    require(text(e.snapshot()) == "abef", "delete characters");
    const std::string stream =
        "\x1b[38;5;196mA日é\r\n\x1b[2;4HZ\x1b]7;file:///C:/work\x1b\\\x1b[?25l";
    for (size_t split = 0; split <= stream.size(); ++split) {
        Terminal f(10, 4, [](const std::string&) {
        });
        f.feed(std::string_view(stream).substr(0, split));
        f.feed(std::string_view(stream).substr(split));
        auto frame = f.snapshot();
        require(text(frame) == "A日éZ" && frame.lines[0][0].fg == 0xff0000 && !frame.visible,
                "every VT/UTF8 split preserves state");
        require(frame.shell_events.size() == 1 && frame.shell_events[0] == "7;file:///C:/work",
                "OSC7 event");
    }
    Terminal modes(16, 4, [](const std::string&) {
    });
    modes.snapshot();
    modes.feed("\t\x1b"
               "7\x1b[4;16H\x1b"
               "8");
    s = modes.snapshot();
    require(s.x == 8 && s.y == 0 && s.changed_rows.empty(),
            "tab cursor save restore do not dirty rows");
    modes.feed("\x1b[2;3r\x1b[?6hX\x1b[3;1HY");
    s = modes.snapshot();
    require(s.y == 2 && s.lines[1][0].text == "X" && s.lines[2][0].text == "Y",
            "origin clamps within scroll margins");
    Terminal edit(6, 4, [](const std::string&) {
    });
    edit.feed("one\r\ntwo\r\nthree\x1b[2;1H\x1b[L");
    s = edit.snapshot();
    require(s.lines[1][0].text == " " && s.lines[2][0].text == "t",
            "insert line shifts only lower rows");
    edit.feed("\x1b[M");
    require(edit.snapshot().lines[1][0].text == "t", "delete line restores next row");
    Terminal edge(6, 4, [](const std::string&) {
    });
    edge.feed("ABCD\x1b[1;4H");
    edge.resize(8, 4);
    edge.resize(4, 4);
    edge.feed("Z");
    require(text(edge.snapshot()) == "ABCZ",
            "reflow preserves cursor before rightmost existing glyph");
    Terminal empty(10, 4, [](const std::string&) {
    });
    empty.feed("a\r\n\r\nb");
    empty.resize(20, 4);
    s = empty.snapshot();
    require(s.lines[0][0].text == "a" && s.lines[1][0].text == " " && s.lines[2][0].text == "b" &&
                s.y == 2,
            "reflow preserves empty hard lines");
    Terminal mouse(8, 4, [&](const std::string& bytes) {
        output += bytes;
    });
    output.clear();
    mouse.feed("\x1b[?1000h\x1b[?1006h");
    mouse.command(Command{'M', {}, 1, 2, 1, 4});
    mouse.command(Command{'M', {}, 1, 2, -1, 4});
    require(output == "\x1b[<16;3;2M\x1b[<16;3;2m", "SGR mouse modifier press release");
    mouse.feed("\x1b]52;c;aGVsbG8=\x07");
    s = mouse.snapshot();
    require(s.clipboard.size() == 1 && s.clipboard[0] == "hello", "OSC52 decoded request");
    Terminal recovery(8, 4, [](const std::string&) {
    });
    recovery.feed("\x1b[" + std::string(3000, '1') + "mQ\x1bPignored\x1b\\Z");
    require(text(recovery.snapshot()) == "QZ", "oversized CSI and unsupported DCS recover");
    Terminal many(4, 2, [](const std::string&) {
    });
    for (int i = 0; i < 100; ++i) {
        many.feed("x\r\n");
    }
    require(many.snapshot().history_rows.size() == 64 && many.has_updates(),
            "bounded history snapshot retains work");
    require(many.snapshot().history_rows.size() == 35 && !many.has_updates(),
            "history drain completes");
    std::cout << "TERMINAL_TESTS=PASS\n";
} catch (const std::exception& error) {

    std::cerr << "TERMINAL_TESTS=FAIL: " << error.what() << '\n';
    return 1;
}
