#pragma once
#include "fastmcpp/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fastmcpp::util::pagination
{

/// Decoded cursor state
struct CursorState
{
    int offset{0};
};

/// Base64 encode a string
inline std::string base64_encode(const std::string& input)
{
    static const char* b64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((input.size() + 2) / 3 * 4);
    for (size_t i = 0; i < input.size(); i += 3)
    {
        uint32_t n = static_cast<uint8_t>(input[i]) << 16;
        if (i + 1 < input.size())
            n |= static_cast<uint8_t>(input[i + 1]) << 8;
        if (i + 2 < input.size())
            n |= static_cast<uint8_t>(input[i + 2]);
        result.push_back(b64_chars[(n >> 18) & 0x3F]);
        result.push_back(b64_chars[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < input.size()) ? b64_chars[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < input.size()) ? b64_chars[n & 0x3F] : '=');
    }
    return result;
}

/// Base64 decode a string; returns empty string on invalid input
inline std::string base64_decode(const std::string& input)
{
    static const int b64_table[] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1,
        -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18,
        19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33,
        34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51};

    std::string result;
    result.reserve(input.size() * 3 / 4);
    for (size_t i = 0; i < input.size(); i += 4)
    {
        if (i + 3 >= input.size())
            break;
        auto val = [&](size_t idx) -> int
        {
            auto c = static_cast<unsigned char>(input[idx]);
            if (c == '=')
                return 0;
            if (c >= sizeof(b64_table) / sizeof(b64_table[0]))
                return -1;
            return b64_table[c];
        };
        int a = val(i), b = val(i + 1), c = val(i + 2), d = val(i + 3);
        if (a < 0 || b < 0 || c < 0 || d < 0)
            return {};
        uint32_t n = (a << 18) | (b << 12) | (c << 6) | d;
        result.push_back(static_cast<char>((n >> 16) & 0xFF));
        if (input[i + 2] != '=')
            result.push_back(static_cast<char>((n >> 8) & 0xFF));
        if (input[i + 3] != '=')
            result.push_back(static_cast<char>(n & 0xFF));
    }
    return result;
}

/// Encode an offset into a cursor string
inline std::string encode_cursor(int offset)
{
    Json j = {{"o", offset}};
    return base64_encode(j.dump());
}

/// Decode a cursor string into a CursorState; returns {0} on error
inline CursorState decode_cursor(const std::string& cursor)
{
    try
    {
        auto decoded = base64_decode(cursor);
        if (decoded.empty())
            return {0};
        auto j = Json::parse(decoded);
        return {j.value("o", 0)};
    }
    catch (...)
    {
        return {0};
    }
}

/// Paginated result with items and optional next cursor
template <typename T>
struct PaginatedResult
{
    std::vector<T> items;
    std::optional<std::string> next_cursor;
};

/// Paginate a sequence by cursor offset
template <typename T>
PaginatedResult<T> paginate_sequence(const std::vector<T>& items,
                                     const std::optional<std::string>& cursor, int page_size)
{
    if (page_size <= 0)
        return {items, std::nullopt};

    int offset = 0;
    if (cursor.has_value() && !cursor->empty())
        offset = decode_cursor(*cursor).offset;

    if (offset < 0)
        offset = 0;

    auto begin = items.begin();
    if (static_cast<size_t>(offset) >= items.size())
        return {{}, std::nullopt};

    std::advance(begin, offset);
    auto end = begin;
    auto remaining = static_cast<int>(std::distance(begin, items.end()));
    std::advance(end, std::min(page_size, remaining));

    std::vector<T> page(begin, end);
    std::optional<std::string> next;
    if (static_cast<size_t>(offset + page_size) < items.size())
        next = encode_cursor(offset + page_size);

    return {std::move(page), std::move(next)};
}

} // namespace fastmcpp::util::pagination
