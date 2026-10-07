#pragma once
#include <windows.h>
#ifdef small
#undef small
#endif
#include <cstdint>
#include <string>
#include <stdexcept>
#include <charconv>

namespace neo {
constexpr uint32_t max_frame = 4 * 1024 * 1024;

inline std::string utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8,
                                          WC_ERR_INVALID_CHARS,
                                          value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (!count) {
        throw std::runtime_error("Invalid UTF-16");
    }
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        result.data(),
                        count,
                        nullptr,
                        nullptr);
    return result;
}

inline std::wstring wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count) {
        throw std::runtime_error("Invalid UTF-8");
    }
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        result.data(),
                        count);
    return result;
}

inline std::string json_string(const std::string& text) {
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (const unsigned char byte : text) {
        if (byte == '"' || byte == '\\') {
            result += '\\';
            result += byte;
        } else if (byte == '\n') {
            result += "\\n";
        } else if (byte == '\r') {
            result += "\\r";
        } else if (byte == '\t') {
            result += "\\t";
        } else if (byte < 32) {
            result += "\\u00";
            result += hex[byte >> 4];
            result += hex[byte & 15];
        } else {
            result += byte;
        }
    }
    return result + '"';
}

inline uint32_t frame_length(const char* bytes) {
    uint32_t length = 0;
    for (int index = 0; index < 4; ++index) {
        length |= uint32_t(static_cast<unsigned char>(bytes[index])) << (8 * index);
    }
    if (!length || length > max_frame) {
        throw std::runtime_error("Invalid frame length");
    }
    return length;
}

inline std::string encode_frame(const std::string& payload) {
    if (payload.empty() || payload.size() > max_frame) {
        throw std::runtime_error("Invalid frame length");
    }
    std::string result(4, '\0');
    for (int index = 0; index < 4; ++index) {
        result[index] = static_cast<char>(payload.size() >> (8 * index));
    }
    return result + payload;
}

inline int integer(const std::string& value) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("Invalid integer");
    }
    return result;
}

struct Command {
    char type;
    std::string text;
    int first = 0;
    int second = 0;
};

inline Command parse_command(const std::string& payload) {
    if (payload.empty()) {
        throw std::runtime_error("Missing command");
    }
    Command result{payload[0], payload.substr(1)};
    if (result.type == 'T' || result.type == 'P') {
        wide(result.text);
        return result;
    }
    if ((result.type == 'C' || result.type == 'L' || result.type == 'H') && result.text.empty()) {
        return result;
    }
    if (result.type != 'R' && result.type != 'K' && result.type != 'U') {
        throw std::runtime_error("Unknown command");
    }
    const auto comma = result.text.find(',');
    if (comma == std::string::npos) {
        throw std::runtime_error("Missing command field");
    }
    result.first = integer(result.text.substr(0, comma));
    result.second = integer(result.text.substr(comma + 1));
    if (result.type == 'R') {
        if (result.first < 2 || result.first > 300 || result.second < 2 || result.second > 200) {
            throw std::runtime_error("Terminal size outside 2..300 columns / 2..200 rows");
        }
    } else {
        if (result.second < 0 || result.second > 7) {
            throw std::runtime_error("Invalid modifiers");
        }
        if (result.type == 'K' && !(result.first >= 1 && result.first <= 14) &&
            !(result.first >= 257 && result.first <= 280)) {
            throw std::runtime_error("Invalid key");
        }
        if (result.type == 'U' && (result.first < 0 || result.first > 0x10ffff ||
                                   (result.first >= 0xd800 && result.first <= 0xdfff))) {
            throw std::runtime_error("Invalid Unicode scalar");
        }
    }
    return result;
}

inline std::wstring quote_argument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(backslashes * (character == L'"' ? 2 : 1), L'\\');
        backslashes = 0;
        if (character == L'"') {
            result += L'\\';
        }
        result += character;
    }
    result.append(backslashes * 2, L'\\');
    return result + L'"';
}
}
