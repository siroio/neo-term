#include <iostream>
#include <stdexcept>
#if __has_include("protocol.hpp") && __has_include("options.hpp")
#include "protocol.hpp"
#include "options.hpp"

void require(bool value, const char* specification) {
    if (!value) {
        throw std::runtime_error(specification);
    }
}

int main() {
    using namespace neo;
    require(candidates(Options{}) == std::vector<BackendKind>{BackendKind::bundled,
                                                              BackendKind::system,
                                                              BackendKind::classic},
            "auto prefers bundled, system, classic");
    Options restricted;
    restricted.no_conpty = true;
    require(candidates(restricted) == std::vector<BackendKind>{BackendKind::classic},
            "ConPTY prohibition excludes both runtimes");
    restricted.kind = BackendKind::system;
    bool rejected = false;
    try {
        candidates(restricted);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "an explicitly forbidden backend must fail");
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
}
#else
int main() {
    std::cerr << "FAIL: backend selection and communication contracts are absent\n";
    return 1;
}
#endif
