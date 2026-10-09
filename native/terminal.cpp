#include "terminal.hpp"
#include "unicode-width.hpp"
#include <array>
#include <cstdlib>
#include <limits>

namespace neo {
namespace {
int palette(int n) {
    static constexpr int basic[] = {0x000000,
                                    0xcd0000,
                                    0x00cd00,
                                    0xcdcd00,
                                    0x0000ee,
                                    0xcd00cd,
                                    0x00cdcd,
                                    0xe5e5e5,
                                    0x7f7f7f,
                                    0xff0000,
                                    0x00ff00,
                                    0xffff00,
                                    0x5c5cff,
                                    0xff00ff,
                                    0x00ffff,
                                    0xffffff};
    n = std::clamp(n, 0, 255);
    if (n < 16) {
        return basic[n];
    }
    if (n >= 232) {
        int v = 8 + 10 * (n - 232);
        return v * 0x010101;
    }
    n -= 16;
    const auto component = [](int v) {
        return v ? 55 + v * 40 : 0;
    };
    return (component(n / 36) << 16) | (component(n / 6 % 6) << 8) | component(n % 6);
}

std::string encode(uint32_t c) {
    std::string s;
    if (c < 128) {
        s += static_cast<char>(c);
    } else if (c < 2048) {
        s += static_cast<char>(0xc0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 63));
    } else if (c < 65536) {
        s += static_cast<char>(0xe0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 63));
        s += static_cast<char>(0x80 | (c & 63));
    } else {
        s += static_cast<char>(0xf0 | (c >> 18));
        s += static_cast<char>(0x80 | ((c >> 12) & 63));
        s += static_cast<char>(0x80 | ((c >> 6) & 63));
        s += static_cast<char>(0x80 | (c & 63));
    }
    return s;
}

bool significant(const Cell& c) {
    return c.text != " " || c.bg != -1 || (c.attributes & 8) || c.prompt;
}
}

struct Terminal::State {
    struct Row {
        std::vector<Cell> cells;
        bool continuation = false;
        int used = 0;
    };

    struct Buffer {
        std::vector<Row> lines;
        int x = 0, y = 0, sx = 0, sy = 0;
        bool pending = false;
        Cell saved;
    };

    int cols, rows, top = 0, bottom;
    Buffer main, alternate;
    bool alt = false;
    std::function<void(const std::string&)> output;
    std::vector<bool> dirty, tabs;
    bool metadata = true, history_cleared = false;
    std::deque<Screen::HistoryRow> history;
    std::deque<std::string> clipboard, events;
    Cell pen;
    bool wrap = true, origin = false, insert = false, app = false, keypad = false, paste = false,
         visible = true, blink = true;
    int shape = 1, mouse = 0;
    bool sgr_mouse = false, utf_mouse = false;
    int held_button = 0, pending_prompt = 0;
    std::string title;
    enum class Parse { Ground, Escape, Csi, Osc, OscEscape, Ignore, IgnoreEscape, Charset };
    Parse parse = Parse::Ground;
    std::string control;
    bool overflow = false;
    uint32_t code = 0, minimum = 0;
    int remaining = 0;
    bool graphics = false;

    State(int c, int r, std::function<void(const std::string&)> o)
        : cols(c), rows(r), bottom(r - 1), output(std::move(o)) {
        validate(c, r);
        main.lines.assign(r, blank());
        alternate.lines = main.lines;
        dirty.assign(r, true);
        reset_tabs();
    }

    static void validate(int c, int r) {
        if (c < 2 || c > 300 || r < 2 || r > 200) {
            throw std::runtime_error("Invalid terminal size");
        }
    }

    Row blank() const {
        Row r;
        r.cells.assign(cols, Cell{});
        return r;
    }

    Buffer& b() {
        return alt ? alternate : main;
    }

    const Buffer& b() const {
        return alt ? alternate : main;
    }

    void reset_tabs() {
        tabs.assign(cols, false);
        for (int i = 8; i < cols; i += 8) {
            tabs[i] = true;
        }
    }

    void mark(int y) {
        dirty[y] = true;
        metadata = true;
    }

    void all() {
        dirty.assign(rows, true);
        metadata = true;
    }

    void recompute(Row& r) {
        r.used = 0;
        for (int x = 0; x < cols; ++x) {
            if (r.cells[x].width && significant(r.cells[x])) {
                r.used = std::min(cols, x + r.cells[x].width);
            }
        }
    }

    void normalize(Row& r) {
        for (int x = 0; x < cols; ++x) {
            if (r.cells[x].width == 2) {
                if (x + 1 == cols) {
                    r.cells[x] = Cell{};
                } else {
                    r.cells[x + 1] = Cell{};
                    r.cells[++x].width = 0;
                }
            } else if (!r.cells[x].width) {
                r.cells[x] = Cell{};
            }
        }
        recompute(r);
    }

    void erase_cell(Row& r, int x) {
        if (!r.cells[x].width && x > 0) {
            r.cells[x - 1] = Cell{};
        }
        if (r.cells[x].width == 2 && x + 1 < cols) {
            r.cells[x + 1] = Cell{};
        }
        Cell c;
        c.fg = pen.fg;
        c.bg = pen.bg;
        c.attributes = pen.attributes;
        r.cells[x] = std::move(c);
    }

    void erase(int y, int begin, int end) {
        auto& r = b().lines[y];
        for (int x = std::max(0, begin); x < std::min(cols, end); ++x) {
            erase_cell(r, x);
        }
        recompute(r);
        if (begin == 0 && end >= cols) {
            r.continuation = false;
        }
        mark(y);
    }

    void store(const Row& row) {
        Screen::HistoryRow h;
        h.continuation = row.continuation;
        for (int x = 0; x < row.used; ++x) {
            if (row.cells[x].width) {
                h.cells.push_back(row.cells[x]);
            }
        }
        if (history.size() == 2000) {
            history.pop_front();
        }
        history.push_back(std::move(h));
    }

    void scroll(int begin, int end, int count) {
        count = std::clamp(count, -(end - begin + 1), end - begin + 1);
        auto& lines = b().lines;
        if (count > 0) {
            for (int i = 0; i < count; ++i) {
                if (!alt && begin == 0) {
                    store(lines[begin]);
                }
                lines.erase(lines.begin() + begin);
                lines.insert(lines.begin() + end, blank());
            }
        } else {
            for (int i = 0; i > count; --i) {
                lines.erase(lines.begin() + end);
                lines.insert(lines.begin() + begin, blank());
            }
        }
        for (int y = begin; y <= end; ++y) {
            mark(y);
        }
    }

    void down(bool continuation = false) {
        auto& v = b();
        if (v.y == bottom) {
            scroll(top, bottom, 1);
        } else if (v.y < rows - 1) {
            ++v.y;
        }
        if (v.lines[v.y].continuation != continuation) {
            v.lines[v.y].continuation = continuation;
            mark(v.y);
        }
        v.pending = false;
        metadata = true;
    }

    void move(int x, int y) {
        auto& v = b();
        v.x = std::clamp(x, 0, cols - 1);
        v.y = std::clamp(y, origin ? top : 0, origin ? bottom : rows - 1);
        v.pending = false;
        metadata = true;
    }

    void save() {
        auto& v = b();
        v.sx = v.x;
        v.sy = v.y;
        v.saved = pen;
    }

    void restore() {
        auto& v = b();
        pen = v.saved;
        move(v.sx, v.sy);
    }

    void prompt(int role) {
        auto& v = b();
        int x = v.x, y = v.y;
        if (role == 2) {
            if (!v.pending) {
                if (x > 0) {
                    --x;
                } else if (y > 0) {
                    --y;
                    x = cols - 1;
                }
            }
            role = 4;
        } else {
            pending_prompt = role;
        }
        if (!v.lines[y].cells[x].width && x > 0) {
            --x;
        }
        v.lines[y].cells[x].prompt |= role;
        recompute(v.lines[y]);
        mark(y);
    }

    void put(uint32_t cp) {
        auto& v = b();
        int w = unicode_width(cp);
        if (w == 0) {
            int x = v.pending ? v.x : v.x - 1, y = v.y;
            if (x < 0 && y > 0) {
                --y;
                x = cols - 1;
            }
            if (x >= 0) {
                if (v.lines[y].cells[x].width == 0 && x > 0) {
                    --x;
                }
                auto& c = v.lines[y].cells[x];
                // The Emacs cell contract permits at most 16 Unicode scalars.
                // Text is produced by encode(), so each non-continuation byte starts one scalar.
                const auto scalars =
                    std::count_if(c.text.begin(), c.text.end(), [](unsigned char ch) {
                        return (ch & 0xc0) != 0x80;
                    });
                if (scalars < 16) {
                    c.text += encode(cp);
                    mark(y);
                }
            }
            return;
        }
        if (v.pending && wrap) {
            v.x = 0;
            down(true);
        }
        if (w == 2 && v.x == cols - 1) {
            if (wrap) {
                v.x = 0;
                down(true);
            } else {
                return;
            }
        }
        auto& r = v.lines[v.y];
        if (insert) {
            r.cells.insert(r.cells.begin() + v.x, w, Cell{});
            r.cells.resize(cols);
            normalize(r);
        }
        int role = std::exchange(pending_prompt, 0);
        erase_cell(r, v.x);
        if (w == 2) {
            erase_cell(r, v.x + 1);
        }
        Cell c = pen;
        c.text = encode(cp);
        c.width = w;
        c.prompt = role;
        r.cells[v.x] = std::move(c);
        if (w == 2) {
            r.cells[v.x + 1] = Cell{};
            r.cells[v.x + 1].width = 0;
        }
        r.used = std::max(r.used, v.x + w);
        mark(v.y);
        v.x += w;
        if (v.x >= cols) {
            v.x = cols - 1;
            v.pending = true;
        } else {
            v.pending = false;
        }
    }

    void c0(unsigned char c) {
        auto& v = b();
        switch (c) {
        case 8:
            move(v.x - 1, v.y);
            break;
        case 9: {
            int x = v.x + 1;
            while (x < cols - 1 && !tabs[x]) {
                ++x;
            }
            move(x, v.y);
            break;
        }
        case 10:
        case 11:
        case 12:
            down();
            break;
        case 13:
            move(0, v.y);
            break;
        case 14:
            graphics = true;
            break;
        case 15:
            graphics = false;
            break;
        default:
            break;
        }
    }

    void mode(int n, bool on, bool priv) {
        if (!priv) {
            if (n == 4) {
                insert = on;
            }
            return;
        }
        switch (n) {
        case 1:
            app = on;
            break;
        case 6:
            origin = on;
            move(0, on ? top : 0);
            break;
        case 7:
            wrap = on;
            break;
        case 12:
            blink = on;
            break;
        case 25:
            visible = on;
            break;
        case 1000:
        case 1002:
        case 1003:
            mouse = on ? (n == 1000 ? 1 : n == 1002 ? 2 : 3) : 0;
            break;
        case 1006:
            sgr_mouse = on;
            break;
        case 1005:
            utf_mouse = on;
            break;
        case 2004:
            paste = on;
            break;
        case 47:
        case 1047:
        case 1049:
            if (on != alt) {
                if (on && n == 1049) {
                    save();
                }
                alt = on;
                if (on && n != 47) {
                    alternate = Buffer{};
                    alternate.lines.assign(rows, blank());
                }
                if (!on && n == 1049) {
                    restore();
                }
                top = 0;
                bottom = rows - 1;
                all();
            }
            break;
        case 1048:
            if (on) {
                save();
            } else {
                restore();
            }
            break;
        default:
            break;
        }
        metadata = true;
    }

    void sgr(const std::vector<int>& p) {
        for (size_t i = 0; i < p.size(); ++i) {
            int n = p[i];
            switch (n) {
            case 0:
                pen = Cell{};
                break;
            case 1:
                pen.attributes |= 1;
                break;
            case 3:
                pen.attributes |= 4;
                break;
            case 4:
                pen.attributes |= 2;
                break;
            case 7:
                pen.attributes |= 8;
                break;
            case 9:
                pen.attributes |= 16;
                break;
            case 22:
                pen.attributes &= ~1;
                break;
            case 23:
                pen.attributes &= ~4;
                break;
            case 24:
                pen.attributes &= ~2;
                break;
            case 27:
                pen.attributes &= ~8;
                break;
            case 29:
                pen.attributes &= ~16;
                break;
            case 39:
                pen.fg = -1;
                break;
            case 49:
                pen.bg = -1;
                break;
            case 38:
            case 48: {
                int color = -1;
                if (i + 2 < p.size() && p[i + 1] == 5) {
                    color = palette(p[i + 2]);
                    i += 2;
                } else if (i + 4 < p.size() && p[i + 1] == 2) {
                    color = (std::clamp(p[i + 2], 0, 255) << 16) |
                            (std::clamp(p[i + 3], 0, 255) << 8) | std::clamp(p[i + 4], 0, 255);
                    i += 4;
                }
                if (color >= 0) {
                    (n == 38 ? pen.fg : pen.bg) = color;
                }
                break;
            }
            default:
                if (n >= 30 && n <= 37) {
                    pen.fg = palette(n - 30);
                } else if (n >= 40 && n <= 47) {
                    pen.bg = palette(n - 40);
                } else if (n >= 90 && n <= 97) {
                    pen.fg = palette(n - 82);
                } else if (n >= 100 && n <= 107) {
                    pen.bg = palette(n - 92);
                }
                break;
            }
        }
    }

    void csi(char final) {
        bool priv = !control.empty() && control[0] == '?';
        bool secondary = !control.empty() && control[0] == '>';
        std::vector<int> p(1, 0);
        bool intermediate = false;
        for (char c : control) {
            if (c >= '0' && c <= '9') {
                p.back() = std::min(100000, p.back() * 10 + c - '0');
            } else if (c == ';' || c == ':') {
                if (p.size() == 64) {
                    return;
                }
                p.push_back(0);
            } else if (c == ' ') {
                intermediate = true;
            }
        }
        const auto arg = [&](size_t i, int d = 1) {
            return i < p.size() && p[i] ? p[i] : d;
        };
        auto& v = b();
        int n = arg(0);
        switch (final) {
        case 'A':
            move(v.x, v.y - n);
            break;
        case 'B':
        case 'e':
            move(v.x, v.y + n);
            break;
        case 'C':
        case 'a':
            move(v.x + n, v.y);
            break;
        case 'D':
            move(v.x - n, v.y);
            break;
        case 'E':
            move(0, v.y + n);
            break;
        case 'F':
            move(0, v.y - n);
            break;
        case 'G':
        case '`':
            move(n - 1, v.y);
            break;
        case 'd':
            move(v.x, n - 1 + (origin ? top : 0));
            break;
        case 'H':
        case 'f':
            move(arg(1) - 1, n - 1 + (origin ? top : 0));
            break;
        case 'J':
            if (p[0] == 3) {
                history.clear();
                history_cleared = true;
                metadata = true;
            } else if (p[0] == 2) {
                for (int y = 0; y < rows; ++y) {
                    erase(y, 0, cols);
                }
            } else if (p[0] == 1) {
                for (int y = 0; y < v.y; ++y) {
                    erase(y, 0, cols);
                }
                erase(v.y, 0, v.x + 1);
            } else {
                erase(v.y, v.x, cols);
                for (int y = v.y + 1; y < rows; ++y) {
                    erase(y, 0, cols);
                }
            }
            break;
        case 'K':
            erase(v.y, p[0] == 0 ? v.x : 0, p[0] == 1 ? v.x + 1 : cols);
            break;
        case 'X':
            erase(v.y, v.x, v.x + std::min(n, cols));
            break;
        case '@':
        case 'P': {
            auto& r = v.lines[v.y];
            n = std::min(n, cols - v.x);
            if (final == '@') {
                r.cells.insert(r.cells.begin() + v.x, n, Cell{});
                r.cells.resize(cols);
            } else {
                r.cells.erase(r.cells.begin() + v.x, r.cells.begin() + v.x + n);
                r.cells.resize(cols);
            }
            normalize(r);
            mark(v.y);
            break;
        }
        case 'L':
        case 'M':
            if (v.y >= top && v.y <= bottom) {
                scroll(v.y, bottom, final == 'L' ? -n : n);
            }
            break;
        case 'S':
            scroll(top, bottom, n);
            break;
        case 'T':
            scroll(top, bottom, -n);
            break;
        case 'r': {
            int t = n - 1, d = arg(1, rows) - 1;
            if (t >= 0 && d < rows && t < d) {
                top = t;
                bottom = d;
                move(0, origin ? top : 0);
            }
            break;
        }
        case 'm':
            sgr(p);
            break;
        case 'h':
        case 'l':
            for (int a : p) {
                mode(a, final == 'h', priv);
            }
            break;
        case 's':
            save();
            break;
        case 'u':
            restore();
            break;
        case 'g':
            if (p[0] == 3) {
                tabs.assign(cols, false);
            } else if (p[0] == 0) {
                tabs[v.x] = false;
            }
            break;
        case 'I':
            for (int i = 0; i < std::min(n, cols); ++i) {
                c0(9);
            }
            break;
        case 'Z': {
            int x = v.x;
            for (int i = 0; i < std::min(n, cols); ++i) {
                x = std::max(0, x - 1);
                while (x > 0 && !tabs[x]) {
                    --x;
                }
            }
            move(x, v.y);
            break;
        }
        case 'n':
            if (p[0] == 5) {
                output("\x1b[0n");
            } else if (p[0] == 6) {
                output(std::string(priv ? "\x1b[?" : "\x1b[") +
                       std::to_string(v.y + 1 - (origin ? top : 0)) + ';' +
                       std::to_string(v.x + 1) + 'R');
            }
            break;
        case 'c':
            output(secondary ? "\x1b[>0;1;0c" : "\x1b[?1;2c");
            break;
        case 'q':
            if (intermediate) {
                shape = n <= 2 ? 1 : n <= 4 ? 2 : 3;
                blink = n % 2 != 0;
                metadata = true;
            }
            break;
        case 't':
            if (p[0] == 18) {
                output("\x1b[8;" + std::to_string(rows) + ';' + std::to_string(cols) + 't');
            }
            break;
        default:
            break;
        }
    }

    void osc() {
        auto split = control.find(';');
        if (split == std::string::npos) {
            return;
        }
        const auto id = control.substr(0, split), value = control.substr(split + 1);
        if (id == "0" || id == "2") {
            if (value.size() <= 16384) {
                try {
                    wide(value);
                    title = value;
                    metadata = true;
                } catch (const std::exception&) {
                }
            }
        } else if (id == "133") {
            if (value == "A") {
                prompt(1);
            } else if (value == "B") {
                prompt(2);
            }
        } else if (id == "7" || id == "51") {
            if (value.size() <= 16384) {
                if (events.size() == 32) {
                    events.pop_front();
                }
                events.push_back(id + ';' + value);
                metadata = true;
            }
        } else if (id == "52") {
            auto sem = value.find(';');
            if (sem == std::string::npos) {
                return;
            }
            auto data = value.substr(sem + 1);
            if (data == "?") {
                return;
            }
            std::string decoded;
            uint32_t bits = 0;
            int count = 0;
            bool valid = true;
            for (unsigned char c : data) {
                if (c == '=') {
                    break;
                }
                int d = c >= 'A' && c <= 'Z'   ? c - 'A'
                        : c >= 'a' && c <= 'z' ? c - 'a' + 26
                        : c >= '0' && c <= '9' ? c - '0' + 52
                        : c == '+'             ? 62
                        : c == '/'             ? 63
                                               : -1;
                if (d < 0) {
                    valid = false;
                    break;
                }
                bits = (bits << 6) | static_cast<uint32_t>(d);
                count += 6;
                if (count >= 8) {
                    count -= 8;
                    decoded += static_cast<char>((bits >> count) & 255);
                }
            }
            if (valid && decoded.size() <= 65536) {
                try {
                    wide(decoded);
                    if (clipboard.size() == 8) {
                        clipboard.pop_front();
                    }
                    clipboard.push_back(std::move(decoded));
                    metadata = true;
                } catch (const std::exception&) {
                }
            }
        }
    }

    void append(unsigned char c, size_t limit) {
        if (!overflow) {
            if (control.size() == limit) {
                overflow = true;
                control.clear();
            } else {
                control += static_cast<char>(c);
            }
        }
    }

    void byte(unsigned char c) {
        if (c == 24 || c == 26) {
            parse = Parse::Ground;
            control.clear();
            remaining = 0;
            return;
        }
        if (parse == Parse::Osc || parse == Parse::OscEscape) {
            if (c == 7 || (parse == Parse::OscEscape && c == '\\')) {
                if (!overflow) {
                    osc();
                }
                parse = Parse::Ground;
                control.clear();
                return;
            }
            if (parse == Parse::OscEscape) {
                append(27, 90000);
                parse = Parse::Osc;
            }
            if (c == 27) {
                parse = Parse::OscEscape;
            } else {
                append(c, 90000);
            }
            return;
        }
        if (parse == Parse::Ignore || parse == Parse::IgnoreEscape) {
            if (parse == Parse::IgnoreEscape && c == '\\') {
                parse = Parse::Ground;
            } else {
                parse = c == 27 ? Parse::IgnoreEscape : Parse::Ignore;
            }
            return;
        }
        if (c == 27) {
            if (remaining) {
                put(0xfffd);
                remaining = 0;
            }
            parse = Parse::Escape;
            return;
        }
        if (c < 32) {
            c0(c);
            return;
        }
        if (c == 127) {
            return;
        }
        if (parse == Parse::Charset) {
            graphics = c == '0';
            parse = Parse::Ground;
            return;
        }
        if (parse == Parse::Escape) {
            parse = Parse::Ground;
            auto& v = b();
            switch (c) {
            case '[':
                parse = Parse::Csi;
                control.clear();
                overflow = false;
                break;
            case ']':
                parse = Parse::Osc;
                control.clear();
                overflow = false;
                break;
            case 'P':
            case '^':
            case '_':
                parse = Parse::Ignore;
                break;
            case '(':
            case ')':
            case '*':
            case '+':
                parse = Parse::Charset;
                break;
            case '7':
                save();
                break;
            case '8':
                restore();
                break;
            case 'D':
                down();
                break;
            case 'E':
                move(0, v.y);
                down();
                break;
            case 'M':
                if (v.y == top) {
                    scroll(top, bottom, -1);
                } else {
                    move(v.x, v.y - 1);
                }
                break;
            case 'H':
                tabs[v.x] = true;
                break;
            case '=':
                keypad = true;
                break;
            case '>':
                keypad = false;
                break;
            case 'c':
                reset();
                break;
            default:
                break;
            }
            return;
        }
        if (parse == Parse::Csi) {
            if (c >= 0x40 && c <= 0x7e) {
                if (!overflow) {
                    csi(static_cast<char>(c));
                }
                parse = Parse::Ground;
                control.clear();
            } else {
                append(c, 1024);
            }
            return;
        }
        if (remaining) {
            if ((c & 0xc0) == 0x80) {
                code = (code << 6) | (c & 63);
                if (--remaining == 0) {
                    put(code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)
                            ? 0xfffd
                            : code);
                }
                return;
            }
            put(0xfffd);
            remaining = 0;
        }
        if (c < 128) {
            static constexpr uint32_t map[] = {
                0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0xb0,   0xb1,
                0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
                0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
                0x2502, 0x2264, 0x2265, 0x3c0,  0x2260, 0xa3,   0xb7};
            put(graphics && c >= 96 && c <= 126 ? map[c - 96] : c);
        } else if (c >= 0xc2 && c <= 0xdf) {
            code = c & 31;
            remaining = 1;
            minimum = 128;
        } else if (c >= 0xe0 && c <= 0xef) {
            code = c & 15;
            remaining = 2;
            minimum = 2048;
        } else if (c >= 0xf0 && c <= 0xf4) {
            code = c & 7;
            remaining = 3;
            minimum = 65536;
        } else {
            put(0xfffd);
        }
    }

    void reset() {
        main = Buffer{};
        alternate = Buffer{};
        main.lines.assign(rows, blank());
        alternate.lines = main.lines;
        alt = false;
        pen = Cell{};
        top = 0;
        bottom = rows - 1;
        wrap = true;
        origin = insert = app = keypad = paste = graphics = false;
        visible = blink = true;
        shape = 1;
        mouse = 0;
        reset_tabs();
        all();
    }

    void clear() {
        auto& v = b();
        for (auto& r : v.lines) {
            r = blank();
        }
        v.x = v.y = 0;
        v.pending = false;
        pending_prompt = 0;
        all();
    }

    void resize_buffer(Buffer& v, int oldcols, int newcols, int newrows, bool reflow) {
        if (!reflow) {
            v.lines.resize(newrows);
            for (auto& r : v.lines) {
                r.cells.resize(newcols);
                normalize(r);
            }
            v.x = std::min(v.x, newcols - 1);
            v.y = std::min(v.y, newrows - 1);
            v.pending = false;
            return;
        }
        std::vector<Row> result;
        std::vector<Cell> logical;
        int cursor_offset = -1, cursor_row = 0, cursor_x = 0;
        bool first_cont = false, have_line = false;
        const auto flush = [&]() {
            if (!have_line) {
                return;
            }
            Row row = blank();
            row.continuation = first_cont;
            int x = 0, offset = 0;
            for (const auto& c : logical) {
                if (x + c.width > newcols) {
                    row.used = x;
                    result.push_back(std::move(row));
                    row = blank();
                    row.continuation = true;
                    x = 0;
                }
                if (cursor_offset == offset) {
                    cursor_row = static_cast<int>(result.size());
                    cursor_x = x;
                }
                row.cells[x] = c;
                if (c.width == 2) {
                    row.cells[x + 1].width = 0;
                }
                x += c.width;
                offset += c.width;
            }
            row.used = x;
            if (cursor_offset >= offset) {
                cursor_row = static_cast<int>(result.size());
                cursor_x = x + cursor_offset - offset;
            }
            result.push_back(std::move(row));
            logical.clear();
            cursor_offset = -1;
        };
        int last = static_cast<int>(v.lines.size()) - 1;
        while (last > v.y && v.lines[last].used == 0 && !v.lines[last].continuation) {
            --last;
        }
        for (int y = 0; y <= last; ++y) {
            const auto& r = v.lines[y];
            if (y == 0 || !r.continuation) {
                flush();
                first_cont = r.continuation;
                have_line = true;
            }
            int logical_width = 0;
            for (const auto& c : logical) {
                logical_width += c.width;
            }
            if (y == v.y) {
                cursor_offset = logical_width + v.x + (v.pending ? 1 : 0);
            }
            int end = r.used;
            if (y == v.y) {
                end = std::max(end, v.x + (v.pending ? 1 : 0));
            }
            for (int x = 0; x < std::min(oldcols, end); ++x) {
                if (r.cells[x].width) {
                    logical.push_back(r.cells[x]);
                }
            }
            if (y == last) {
                flush();
            }
        }
        int drop = std::max(0, static_cast<int>(result.size()) - newrows);
        for (int i = 0; i < drop; ++i) {
            if (&v == &main) {
                store(result[i]);
            }
        }
        if (drop) {
            result.erase(result.begin(), result.begin() + drop);
        }
        result.resize(newrows);
        for (auto& r : result) {
            if (r.cells.empty()) {
                r = blank();
            }
        }
        v.lines = std::move(result);
        v.y = std::clamp(cursor_row - drop, 0, newrows - 1);
        v.x = std::clamp(cursor_x, 0, newcols - 1);
        v.pending = cursor_x >= newcols;
    }

    void resize(int c, int r) {
        validate(c, r);
        if (c == cols && r == rows) {
            return;
        }
        int old = cols;
        cols = c;
        resize_buffer(main, old, c, r, true);
        resize_buffer(alternate, old, c, r, false);
        rows = r;
        top = 0;
        bottom = r - 1;
        reset_tabs();
        all();
    }

    void command(const Command& v) {
        if (v.text.size() > max_frame) {
            throw std::runtime_error("Terminal input exceeds frame limit");
        }
        if (v.type == 'R') {
            resize(v.first, v.second);
            return;
        }
        if (v.type == 'L') {
            clear();
            return;
        }
        if (v.type == 'H') {
            history.clear();
            history_cleared = true;
            metadata = true;
            return;
        }
        if (v.type == 'T' || v.type == 'P') {
            output(v.type == 'P' && paste ? "\x1b[200~" + v.text + "\x1b[201~" : v.text);
            return;
        }
        if (v.type == 'U') {
            uint32_t c = static_cast<uint32_t>(v.first);
            const bool control_letter = c >= 'a' && c <= 'z';
            const bool ambiguous = c == 'i' || c == 'j' || c == 'm' || c == '[';
            const bool classical_punctuation = c >= '\\' && c <= '_';
            if (((v.second & 4) && (ambiguous || (!control_letter && !classical_punctuation))) ||
                (c == ' ' && (v.second & 1))) {
                output("\x1b[" + std::to_string(c) + ';' + std::to_string(v.second + 1) + 'u');
                return;
            }
            if (v.second & 4) {
                if (c >= 'a' && c <= 'z') {
                    c -= 32;
                }
                if (c >= 64 && c <= 95) {
                    c &= 31;
                } else if (c == ' ') {
                    c = 0;
                } else if (c == '?') {
                    c = 127;
                }
            }
            output(std::string(v.second & 2 ? "\x1b" : "") + encode(c));
            return;
        }
        if (v.type == 'K') {
            int k = v.first, m = v.second;
            std::string s;
            const int modifier = m + 1;
            if (k <= 4 && (m & 4) && !(k == 2 && (m & 1))) {
                const int c = k == 1 ? 13 : k == 2 ? 9 : k == 3 ? 127 : 27;
                output("\x1b[" + std::to_string(c) + ';' + std::to_string(modifier) + 'u');
                return;
            } else if (k == 2 && (m & 1)) {
                output(m == 1 ? "\x1b[Z" : "\x1b[1;" + std::to_string(modifier) + 'Z');
                return;
            } else if (k == 1) {
                s = "\r";
            } else if (k == 2) {
                s = m & 1 ? "\x1b[Z" : "\t";
            } else if (k == 3) {
                s = "\x7f";
            } else if (k == 4) {
                s = "\x1b";
            } else if ((k >= 5 && k <= 8) || k == 11 || k == 12) {
                char f = k == 5    ? 'A'
                         : k == 6  ? 'B'
                         : k == 7  ? 'D'
                         : k == 8  ? 'C'
                         : k == 11 ? 'H'
                                   : 'F';
                s = m ? "\x1b[1;" + std::to_string(modifier) + f
                      : std::string(app ? "\x1bO" : "\x1b[") + f;
            } else if (k >= 257 && k <= 260) {
                char f = static_cast<char>('P' + k - 257);
                s = m ? "\x1b[1;" + std::to_string(modifier) + f : std::string("\x1bO") + f;
            } else {
                int number = 0;
                if (k == 9) {
                    number = 2;
                } else if (k == 10) {
                    number = 3;
                } else if (k == 13) {
                    number = 5;
                } else if (k == 14) {
                    number = 6;
                } else if (k >= 261 && k <= 280) {
                    static constexpr int fn[] = {15, 17, 18, 19, 20, 21, 23, 24, 25, 26,
                                                 28, 29, 31, 32, 33, 34, 42, 43, 44, 45};
                    number = fn[k - 261];
                }
                if (number) {
                    s = "\x1b[" + std::to_string(number) +
                        (m ? ";" + std::to_string(modifier) : "") + "~";
                }
            }
            if (k <= 4 && (m & 2)) {
                s = "\x1b" + s;
            }
            output(s);
            return;
        }
        if (v.type == 'M' && mouse) {
            if (v.third > 0 && v.third <= 3) {
                held_button = v.third;
            }
            if (v.third == 0 && mouse == 1) {
                return;
            }
            if (v.third == 0 && mouse == 2 && !held_button) {
                return;
            }
            bool release = v.third < 0;
            int button = std::abs(v.third);
            int code_button = button >= 4   ? 64 + button - 4
                              : button      ? button - 1
                              : held_button ? held_button - 1
                                            : 3;
            if (v.third == 0) {
                code_button |= 32;
            }
            if (v.fourth & 1) {
                code_button |= 4;
            }
            if (v.fourth & 2) {
                code_button |= 8;
            }
            if (v.fourth & 4) {
                code_button |= 16;
            }
            if (sgr_mouse) {
                output("\x1b[<" + std::to_string(code_button) + ';' + std::to_string(v.second + 1) +
                       ';' + std::to_string(v.first + 1) + (release ? 'm' : 'M'));
            } else if (utf_mouse) {
                output("\x1b[M" + encode(static_cast<uint32_t>((release ? 3 : code_button) + 32)) +
                       encode(static_cast<uint32_t>(v.second + 33)) +
                       encode(static_cast<uint32_t>(v.first + 33)));
            } else if (v.second < 223 && v.first < 223) {
                std::string s = "\x1b[M";
                s += static_cast<char>((release ? 3 : code_button) + 32);
                s += static_cast<char>(v.second + 33);
                s += static_cast<char>(v.first + 33);
                output(s);
            }
            if (release) {
                held_button = 0;
            }
        }
    }

    Screen snapshot() {
        Screen s;
        const auto& v = b();
        s.cols = cols;
        s.rows = rows;
        s.x = v.x;
        s.y = v.y;
        s.visible = visible;
        s.cursor_shape = shape;
        s.cursor_blink = blink;
        s.alt = alt;
        s.title = title;
        s.mouse = mouse;
        s.incremental = true;
        s.history_cleared = std::exchange(history_cleared, false);
        s.lines.resize(rows);
        s.continuations.resize(rows);
        s.content_widths.resize(rows);
        for (int y = 0; y < rows; ++y) {
            s.continuations[y] = v.lines[y].continuation;
            s.content_widths[y] = v.lines[y].used;
            if (dirty[y]) {
                s.changed_rows.push_back(y);
                for (const auto& c : v.lines[y].cells) {
                    if (c.width) {
                        s.lines[y].push_back(c);
                    }
                }
                dirty[y] = false;
            }
        }
        for (int i = 0; i < 64 && !history.empty(); ++i) {
            std::string line;
            for (const auto& c : history.front().cells) {
                line += c.text;
            }
            s.history.push_back(std::move(line));
            s.history_rows.push_back(std::move(history.front()));
            history.pop_front();
        }
        s.clipboard.assign(clipboard.begin(), clipboard.end());
        clipboard.clear();
        s.shell_events.assign(events.begin(), events.end());
        events.clear();
        metadata = false;
        return s;
    }
};

Terminal::Terminal(int cols, int rows, std::function<void(const std::string&)> output)
    : state_(std::make_unique<State>(cols, rows, std::move(output))) {
}

Terminal::~Terminal() = default;

void Terminal::feed(std::string_view bytes) {
    for (unsigned char c : bytes) {
        state_->byte(c);
    }
}

void Terminal::resize(int cols, int rows) {
    state_->resize(cols, rows);
}

void Terminal::clear_screen() {
    state_->clear();
}

void Terminal::command(const Command& value) {
    state_->command(value);
}

void Terminal::shell_notification(const std::string& value) {
    (void)value; /* ConPTY prompt coordinates can precede streamed text; OSC 133 supplies ordered
                    marks. */
}

bool Terminal::has_updates() const {
    return state_->metadata || !state_->history.empty();
}

Screen Terminal::snapshot() {
    return state_->snapshot();
}
}
