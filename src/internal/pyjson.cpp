#include "pyjson.hpp"

#include "pyurl.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <system_error>

namespace fastmcpp::internal::pyjson
{

namespace
{

// ------------------------------------------------------------------ bytes -> code points

std::string hex2(unsigned char b)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%02x", b);
    return buf;
}

/// UnicodeDecodeError.__str__ for bytes [start, end) of @p data.
[[noreturn]] void codec_error(const std::string& codec, const std::string& data, std::size_t start,
                              std::size_t end, const std::string& reason)
{
    std::string msg = "'" + codec + "' codec can't decode ";
    if (end == start + 1)
        msg += "byte " + hex2(static_cast<unsigned char>(data[start])) + " in position " +
               std::to_string(start);
    else
        msg += "bytes in position " + std::to_string(start) + "-" + std::to_string(end - 1);
    throw UnicodeDecodeError(msg + ": " + reason);
}

bool is_cont(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

/// bytes.decode('utf-8', 'surrogatepass') of @p b from @p start; error
/// positions are relative to @p start (as utf-8-sig reports them).
std::u32string decode_utf8(const std::string& in, std::size_t start)
{
    const std::string b = in.substr(start);
    const auto* p = reinterpret_cast<const unsigned char*>(b.data());
    const std::size_t n = b.size();
    std::u32string out;
    out.reserve(n);
    std::size_t i = 0;
    while (i < n)
    {
        const unsigned char c = p[i];
        if (c < 0x80)
        {
            out.push_back(c);
            ++i;
            continue;
        }
        // Mirrors the reference decoder's error classification: 0 = end of
        // data, 1 = invalid start, k >= 2 = byte k-1 is a bad continuation.
        int err = -1;
        int len = 0;
        if (c < 0xC2 || c > 0xF4)
            err = 1;
        else if (c < 0xE0)
        {
            len = 2;
            if (n - i < 2)
                err = 0;
            else if (!is_cont(p[i + 1]))
                err = 2;
        }
        else if (c < 0xF0)
        {
            len = 3;
            if (n - i < 3)
            {
                if (n - i >= 2)
                {
                    const unsigned char c2 = p[i + 1];
                    if (!is_cont(c2) || (c2 < 0xA0 ? c == 0xE0 : c == 0xED))
                        err = 2;
                }
                if (err < 0)
                    err = 0;
            }
            else
            {
                const unsigned char c2 = p[i + 1];
                if (!is_cont(c2) || (c == 0xE0 && c2 < 0xA0) || (c == 0xED && c2 >= 0xA0))
                    err = 2;
                else if (!is_cont(p[i + 2]))
                    err = 3;
            }
        }
        else
        {
            len = 4;
            if (n - i < 4)
            {
                if (n - i >= 2)
                {
                    const unsigned char c2 = p[i + 1];
                    if (!is_cont(c2) || (c2 < 0x90 ? c == 0xF0 : c == 0xF4))
                        err = 2;
                    else if (n - i >= 3 && !is_cont(p[i + 2]))
                        err = 3;
                }
                if (err < 0)
                    err = 0;
            }
            else
            {
                const unsigned char c2 = p[i + 1];
                if (!is_cont(c2) || (c == 0xF0 && c2 < 0x90) || (c == 0xF4 && c2 >= 0x90))
                    err = 2;
                else if (!is_cont(p[i + 2]))
                    err = 3;
                else if (!is_cont(p[i + 3]))
                    err = 4;
            }
        }
        if (err < 0)
        {
            char32_t cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (int k = 1; k < len; ++k)
                cp = (cp << 6) | (p[i + k] & 0x3F);
            out.push_back(cp);
            i += len;
            continue;
        }
        // surrogatepass: an encoded surrogate (ED A0..BF xx) is accepted.
        if (n - i >= 3 && (c & 0xF0) == 0xE0 && is_cont(p[i + 1]) && is_cont(p[i + 2]))
        {
            const char32_t cp = ((c & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
            if (cp >= 0xD800 && cp <= 0xDFFF)
            {
                out.push_back(cp);
                i += 3;
                continue;
            }
        }
        if (err == 0)
            codec_error("utf-8", b, i, n, "unexpected end of data");
        if (err == 1)
            codec_error("utf-8", b, i, i + 1, "invalid start byte");
        codec_error("utf-8", b, i, i + static_cast<std::size_t>(err) - 1,
                    "invalid continuation byte");
    }
    return out;
}

/// utf-16 with surrogatepass: unpaired surrogates pass through. Error
/// positions count from the start of @p b, BOM included.
std::u32string decode_utf16(const std::string& b, std::size_t start, bool big_endian)
{
    const char* codec = big_endian ? "utf-16-be" : "utf-16-le";
    const auto* p = reinterpret_cast<const unsigned char*>(b.data());
    const std::size_t n = b.size();
    std::u32string out;
    auto unit = [&](std::size_t k) -> char32_t
    { return big_endian ? char32_t((p[k] << 8) | p[k + 1]) : char32_t((p[k + 1] << 8) | p[k]); };
    std::size_t k = start;
    for (; k + 1 < n; k += 2)
    {
        const char32_t u = unit(k);
        if (u >= 0xD800 && u <= 0xDBFF && k + 3 < n)
        {
            const char32_t u2 = unit(k + 2);
            if (u2 >= 0xDC00 && u2 <= 0xDFFF)
            {
                out.push_back(0x10000 + ((u - 0xD800) << 10) + (u2 - 0xDC00));
                k += 2;
                continue;
            }
        }
        out.push_back(u);
    }
    if (k < n)
        codec_error(codec, b, k, n, "truncated data");
    return out;
}

std::u32string decode_utf32(const std::string& b, std::size_t start, bool big_endian)
{
    const char* codec = big_endian ? "utf-32-be" : "utf-32-le";
    const auto* p = reinterpret_cast<const unsigned char*>(b.data());
    const std::size_t n = b.size();
    std::u32string out;
    std::size_t k = start;
    for (; k + 3 < n; k += 4)
    {
        const std::uint32_t v =
            big_endian ? (std::uint32_t(p[k]) << 24) | (std::uint32_t(p[k + 1]) << 16) |
                             (std::uint32_t(p[k + 2]) << 8) | p[k + 3]
                       : (std::uint32_t(p[k + 3]) << 24) | (std::uint32_t(p[k + 2]) << 16) |
                             (std::uint32_t(p[k + 1]) << 8) | p[k];
        if (v > 0x10FFFF)
            codec_error(codec, b, k, k + 4, "code point not in range(0x110000)");
        out.push_back(char32_t(v));
    }
    if (k < n)
        codec_error(codec, b, k, n, "truncated data");
    return out;
}

/// json.detect_encoding followed by bytes.decode(encoding, 'surrogatepass').
std::u32string decode_document(const std::string& b)
{
    auto starts = [&](const char* bom, std::size_t n)
    { return b.size() >= n && b.compare(0, n, bom, n) == 0; };
    if (starts("\xFF\xFE\x00\x00", 4))
        return decode_utf32(b, 4, false);
    if (starts("\x00\x00\xFE\xFF", 4))
        return decode_utf32(b, 4, true);
    if (starts("\xFF\xFE", 2))
        return decode_utf16(b, 2, false);
    if (starts("\xFE\xFF", 2))
        return decode_utf16(b, 2, true);
    if (starts("\xEF\xBB\xBF", 3))
        return decode_utf8(b, 3);
    if (b.size() >= 4)
    {
        if (b[0] == 0)
            return b[1] ? decode_utf16(b, 0, true) : decode_utf32(b, 0, true);
        if (b[1] == 0)
            return (b[2] || b[3]) ? decode_utf16(b, 0, false) : decode_utf32(b, 0, false);
    }
    else if (b.size() == 2)
    {
        if (b[0] == 0)
            return decode_utf16(b, 0, true);
        if (b[1] == 0)
            return decode_utf16(b, 0, false);
    }
    return decode_utf8(b, 0);
}

void append_utf8(std::string& out, char32_t cp)
{
    if (cp >= 0xD800 && cp <= 0xDFFF)
        cp = 0xFFFD;
    if (cp < 0x80)
        out.push_back(char(cp));
    else if (cp < 0x800)
    {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

// ------------------------------------------------------------------ scanner

/// Where the reference scanner raises StopIteration; decode() reports it as
/// "Expecting value" at this index.
struct StopIteration
{
    std::size_t idx;
};

constexpr std::size_t kMaxIntDigits = 4300;

class Scanner
{
  public:
    Scanner(const std::u32string& s, std::size_t max_depth)
        : s_(s), n_(s.size()), max_depth_(max_depth)
    {
    }

    OrderedJson decode()
    {
        std::size_t end = 0;
        OrderedJson obj;
        try
        {
            obj = scan(skip_ws(0), end);
        }
        catch (const StopIteration& e)
        {
            raise("Expecting value", e.idx);
        }
        end = skip_ws(end);
        if (end != n_)
            raise("Extra data", end);
        return obj;
    }

  private:
    struct Frame
    {
        bool is_object;
        OrderedJson value;
        std::string key;
    };

    std::size_t skip_ws(std::size_t idx) const
    {
        while (idx < n_ &&
               (s_[idx] == ' ' || s_[idx] == '\t' || s_[idx] == '\n' || s_[idx] == '\r'))
            ++idx;
        return idx;
    }

    [[noreturn]] void raise(const char* msg, std::size_t pos) const
    {
        std::size_t lineno = 1;
        std::size_t colno = pos + 1;
        for (std::size_t k = 0; k < pos && k < n_; ++k)
            if (s_[k] == '\n')
                ++lineno, colno = pos - k;
        throw JSONDecodeError(msg, lineno, colno, pos);
    }

    static int hex_digit(char32_t d)
    {
        if (d >= '0' && d <= '9')
            return int(d - '0');
        if (d >= 'a' && d <= 'f')
            return int(d - 'a' + 10);
        if (d >= 'A' && d <= 'F')
            return int(d - 'A' + 10);
        return -1;
    }

    std::string scanstring(std::size_t end, std::size_t& next_end) const
    {
        const std::size_t begin = end - 1;
        std::string out;
        while (true)
        {
            std::size_t next = end;
            char32_t c = 0;
            for (; next < n_; ++next)
            {
                c = s_[next];
                if (c == '"' || c == '\\')
                    break;
                if (c <= 0x1f)
                    raise("Invalid control character at", next);
            }
            if (next == n_)
                c = 0;
            if (c != '"' && c != '\\')
                raise("Unterminated string starting at", begin);
            for (std::size_t k = end; k < next; ++k)
                append_utf8(out, s_[k]);
            ++next;
            if (c == '"')
            {
                end = next;
                break;
            }
            if (next == n_)
                raise("Unterminated string starting at", begin);
            c = s_[next];
            if (c != 'u')
            {
                end = next + 1;
                switch (c)
                {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    c = '\b';
                    break;
                case 'f':
                    c = '\f';
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                default:
                    raise("Invalid \\escape", end - 2);
                }
            }
            else
            {
                c = 0;
                ++next;
                end = next + 4;
                if (end >= n_)
                    raise("Invalid \\uXXXX escape", next - 1);
                for (; next < end; ++next)
                {
                    const int d = hex_digit(s_[next]);
                    if (d < 0)
                        raise("Invalid \\uXXXX escape", end - 5);
                    c = (c << 4) | char32_t(d);
                }
                if (c >= 0xD800 && c <= 0xDBFF && end + 6 < n_ && s_[next++] == '\\' &&
                    s_[next++] == 'u')
                {
                    char32_t c2 = 0;
                    end += 6;
                    for (; next < end; ++next)
                    {
                        const int d = hex_digit(s_[next]);
                        if (d < 0)
                            raise("Invalid \\uXXXX escape", end - 5);
                        c2 = (c2 << 4) | char32_t(d);
                    }
                    if (c2 >= 0xDC00 && c2 <= 0xDFFF)
                        c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                    else
                        end -= 6;
                }
            }
            append_utf8(out, c);
        }
        next_end = end;
        return out;
    }

    bool matches(std::size_t idx, const char* word) const
    {
        for (std::size_t k = 0; word[k]; ++k)
            if (idx + k >= n_ || s_[idx + k] != char32_t(static_cast<unsigned char>(word[k])))
                return false;
        return true;
    }

    OrderedJson match_number(std::size_t start, std::size_t& next_idx) const
    {
        const std::size_t end_idx = n_ - 1;
        std::size_t idx = start;
        bool is_float = false;
        auto digit = [&](std::size_t k) { return s_[k] >= '0' && s_[k] <= '9'; };
        if (s_[idx] == '-')
        {
            ++idx;
            if (idx > end_idx)
                throw StopIteration{start};
        }
        if (s_[idx] >= '1' && s_[idx] <= '9')
        {
            ++idx;
            while (idx <= end_idx && digit(idx))
                ++idx;
        }
        else if (s_[idx] == '0')
            ++idx;
        else
            throw StopIteration{start};
        if (idx < end_idx && s_[idx] == '.' && digit(idx + 1))
        {
            is_float = true;
            idx += 2;
            while (idx <= end_idx && digit(idx))
                ++idx;
        }
        if (idx < end_idx && (s_[idx] == 'e' || s_[idx] == 'E'))
        {
            const std::size_t e_start = idx;
            ++idx;
            if (idx < end_idx && (s_[idx] == '-' || s_[idx] == '+'))
                ++idx;
            while (idx <= end_idx && digit(idx))
                ++idx;
            if (digit(idx - 1))
                is_float = true;
            else
                idx = e_start;
        }
        std::string text;
        text.reserve(idx - start);
        for (std::size_t k = start; k < idx; ++k)
            text.push_back(char(s_[k]));
        next_idx = idx;
        if (is_float)
            return OrderedJson(std::strtod(text.c_str(), nullptr));
        const std::size_t ndigits = text.size() - (text[0] == '-' ? 1 : 0);
        if (ndigits > kMaxIntDigits)
            throw std::invalid_argument(
                "Exceeds the limit (4300 digits) for integer string conversion: value has " +
                std::to_string(ndigits) +
                " digits; use sys.set_int_max_str_digits() to increase the limit");
        const char* first = text.data();
        const char* last = text.data() + text.size();
        std::int64_t i64 = 0;
        if (auto r = std::from_chars(first, last, i64); r.ec == std::errc() && r.ptr == last)
            return OrderedJson(i64);
        std::uint64_t u64 = 0;
        if (auto r = std::from_chars(first, last, u64); r.ec == std::errc() && r.ptr == last)
            return OrderedJson(u64);
        OrderedJson big = OrderedJson::binary(std::vector<std::uint8_t>(text.begin(), text.end()),
                                              kBigIntSubtype);
        return big;
    }

    /// Opens a container at @p idx (s_[idx] is '{' or '['), or returns a
    /// complete scalar.
    std::optional<OrderedJson> begin_value(std::vector<Frame>& stack, std::size_t& idx) const
    {
        if (idx >= n_)
            throw StopIteration{idx};
        const char32_t c = s_[idx];
        if (c == '{' || c == '[')
        {
            if (stack.size() + 1 > max_depth_)
                throw RecursionError(std::string("maximum recursion depth exceeded while decoding "
                                                 "a JSON ") +
                                     (c == '{' ? "object" : "array") + " from a unicode string");
            const bool obj = c == '{';
            stack.push_back(Frame{obj, obj ? OrderedJson::object() : OrderedJson::array(), {}});
            idx = skip_ws(idx + 1);
            if (idx < n_ && s_[idx] == (obj ? U'}' : U']'))
            {
                ++idx;
                OrderedJson done = std::move(stack.back().value);
                stack.pop_back();
                return done;
            }
            if (obj)
                read_key(stack.back(), idx);
            return std::nullopt;
        }
        std::size_t next = 0;
        switch (c)
        {
        case '"':
        {
            OrderedJson v(scanstring(idx + 1, next));
            idx = next;
            return v;
        }
        case 'n':
            if (matches(idx, "null"))
                return idx += 4, OrderedJson(nullptr);
            break;
        case 't':
            if (matches(idx, "true"))
                return idx += 4, OrderedJson(true);
            break;
        case 'f':
            if (matches(idx, "false"))
                return idx += 5, OrderedJson(false);
            break;
        case 'N':
            if (matches(idx, "NaN"))
                return idx += 3, OrderedJson(std::numeric_limits<double>::quiet_NaN());
            break;
        case 'I':
            if (matches(idx, "Infinity"))
                return idx += 8, OrderedJson(std::numeric_limits<double>::infinity());
            break;
        case '-':
            if (matches(idx, "-Infinity"))
                return idx += 9, OrderedJson(-std::numeric_limits<double>::infinity());
            break;
        default:
            break;
        }
        OrderedJson v = match_number(idx, next);
        idx = next;
        return v;
    }

    /// Reads `"key"` and the ':' delimiter, leaving @p idx at the value.
    void read_key(Frame& frame, std::size_t& idx) const
    {
        if (idx >= n_ || s_[idx] != '"')
            raise("Expecting property name enclosed in double quotes", idx);
        std::size_t after = 0;
        frame.key = scanstring(idx + 1, after);
        idx = skip_ws(after);
        if (idx >= n_ || s_[idx] != ':')
            raise("Expecting ':' delimiter", idx);
        idx = skip_ws(idx + 1);
    }

    OrderedJson scan(std::size_t idx, std::size_t& next_idx) const
    {
        std::vector<Frame> stack;
        while (true)
        {
            std::optional<OrderedJson> value = begin_value(stack, idx);
            if (!value)
                continue;
            // Attach the value and consume delimiters, closing containers
            // until one expects another value.
            while (true)
            {
                if (stack.empty())
                {
                    next_idx = idx;
                    return std::move(*value);
                }
                Frame& top = stack.back();
                if (top.is_object)
                    top.value[top.key] = std::move(*value);
                else
                    top.value.push_back(std::move(*value));
                idx = skip_ws(idx);
                const char32_t close = top.is_object ? U'}' : U']';
                if (idx < n_ && s_[idx] == close)
                {
                    ++idx;
                    value = std::move(top.value);
                    stack.pop_back();
                    continue;
                }
                if (idx >= n_ || s_[idx] != ',')
                    raise("Expecting ',' delimiter", idx);
                idx = skip_ws(idx + 1);
                if (top.is_object)
                    read_key(top, idx);
                break;
            }
        }
    }

    const std::u32string& s_;
    std::size_t n_;
    std::size_t max_depth_;
};

// ------------------------------------------------------------------ output helpers

bool is_big_int(const OrderedJson& v)
{
    return v.is_binary() && v.get_binary().has_subtype() &&
           v.get_binary().subtype() == kBigIntSubtype;
}

std::string big_int_digits(const OrderedJson& v)
{
    const auto& bin = v.get_binary();
    return std::string(bin.begin(), bin.end());
}

void dumps_string(const std::string& s, std::string& out)
{
    static const char* hex = "0123456789abcdef";
    auto unit = [&](std::uint32_t u)
    {
        out += "\\u";
        for (int shift = 12; shift >= 0; shift -= 4)
            out.push_back(hex[(u >> shift) & 0xF]);
    };
    out.push_back('"');
    std::vector<std::uint32_t> cps;
    {
        // Decode leniently: the strings come from loads() or from callers
        // holding valid UTF-8.
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        std::size_t i = 0;
        while (i < s.size())
        {
            const unsigned char c = p[i];
            int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (i + len > s.size())
                len = 1;
            std::uint32_t cp = len == 1 ? c : c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            for (int k = 1; k < len; ++k)
                cp = (cp << 6) | (p[i + k] & 0x3F);
            cps.push_back(cp);
            i += len;
        }
    }
    for (const auto cp : cps)
    {
        switch (cp)
        {
        case '"':
            out += "\\\"";
            continue;
        case '\\':
            out += "\\\\";
            continue;
        case '\n':
            out += "\\n";
            continue;
        case '\r':
            out += "\\r";
            continue;
        case '\t':
            out += "\\t";
            continue;
        case '\b':
            out += "\\b";
            continue;
        case '\f':
            out += "\\f";
            continue;
        default:
            break;
        }
        if (cp >= 0x20 && cp <= 0x7E)
            out.push_back(char(cp));
        else if (cp < 0x10000)
            unit(cp);
        else
        {
            const std::uint32_t v = cp - 0x10000;
            unit(0xD800 | (v >> 10));
            unit(0xDC00 | (v & 0x3FF));
        }
    }
    out.push_back('"');
}

std::string dumps_float(double v)
{
    if (std::isnan(v))
        return "NaN";
    if (std::isinf(v))
        return v < 0 ? "-Infinity" : "Infinity";
    return float_repr(v);
}

/// How a value is written: json.dumps (ensure_ascii) or repr().
struct Style
{
    bool repr = false;
    std::optional<int> indent;
    bool compact = false;
};

void write_scalar(const OrderedJson& v, const Style& st, std::string& out)
{
    switch (v.type())
    {
    case nlohmann::json::value_t::null:
        out += st.repr ? "None" : "null";
        return;
    case nlohmann::json::value_t::boolean:
        out += v.get<bool>() ? (st.repr ? "True" : "true") : (st.repr ? "False" : "false");
        return;
    case nlohmann::json::value_t::number_integer:
        out += std::to_string(v.get<std::int64_t>());
        return;
    case nlohmann::json::value_t::number_unsigned:
        out += std::to_string(v.get<std::uint64_t>());
        return;
    case nlohmann::json::value_t::number_float:
        out += st.repr ? float_repr(v.get<double>()) : dumps_float(v.get<double>());
        return;
    case nlohmann::json::value_t::string:
        if (st.repr)
            out += pyurl::py_repr(v.get_ref<const std::string&>());
        else
            dumps_string(v.get_ref<const std::string&>(), out);
        return;
    case nlohmann::json::value_t::binary:
        out += is_big_int(v) ? big_int_digits(v) : (st.repr ? "None" : "null");
        return;
    default:
        out += st.repr ? "None" : "null";
        return;
    }
}

/// Serializes without recursion so deeply nested documents are safe.
std::string write_value(const OrderedJson& root, const Style& st)
{
    struct Level
    {
        const OrderedJson* value;
        OrderedJson::const_iterator it;
        bool first;
    };
    std::string out;
    const auto newline = [&](std::size_t lvl)
    {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(*st.indent) * lvl, ' ');
    };
    const char* item_sep = st.indent || st.compact ? "," : ", ";
    std::vector<Level> stack;
    const OrderedJson* next = &root;
    while (true)
    {
        if (next)
        {
            const OrderedJson& v = *next;
            next = nullptr;
            if (v.is_object() || v.is_array())
            {
                if (v.empty())
                    out += v.is_object() ? "{}" : "[]";
                else
                {
                    out.push_back(v.is_object() ? '{' : '[');
                    stack.push_back(Level{&v, v.cbegin(), true});
                }
            }
            else
                write_scalar(v, st, out);
        }
        if (stack.empty())
            return out;
        Level& top = stack.back();
        if (top.it == top.value->cend())
        {
            const bool obj = top.value->is_object();
            stack.pop_back();
            if (st.indent)
                newline(stack.size());
            out.push_back(obj ? '}' : ']');
            continue;
        }
        if (!top.first)
            out += item_sep;
        top.first = false;
        if (st.indent)
            newline(stack.size());
        if (top.value->is_object())
        {
            if (st.repr)
                out += pyurl::py_repr(top.it.key());
            else
                dumps_string(top.it.key(), out);
            out += st.compact ? ":" : ": ";
        }
        next = &*top.it;
        ++top.it;
    }
}
const char* kErrorsUrl = "https://errors.pydantic.dev/2.14/v/";

/// Byte offset of the character boundary at or before @p pos.
std::size_t floor_boundary(const std::string& s, std::size_t pos)
{
    while (pos > 0 && pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
        --pos;
    return pos;
}

std::size_t ceil_boundary(const std::string& s, std::size_t pos)
{
    while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
        ++pos;
    return pos;
}

} // namespace

JSONDecodeError::JSONDecodeError(const std::string& msg, std::size_t lineno, std::size_t colno,
                                 std::size_t pos)
    : std::invalid_argument(msg + ": line " + std::to_string(lineno) + " column " +
                            std::to_string(colno) + " (char " + std::to_string(pos) + ")")
{
}

OrderedJson loads(const std::string& bytes, std::size_t max_depth)
{
    const std::u32string doc = decode_document(bytes);
    return Scanner(doc, max_depth).decode();
}

bool is_int(const OrderedJson& value)
{
    return value.is_number_integer() || is_big_int(value);
}

std::string dumps(const OrderedJson& value, std::optional<int> indent)
{
    return write_value(value, Style{false, indent});
}

std::string dumps_compact(const OrderedJson& value)
{
    return write_value(value, Style{false, std::nullopt, true});
}

std::string urlsafe_b64decode(const std::string& text)
{
    for (const char c : text)
        if (static_cast<unsigned char>(c) >= 0x80)
            throw std::invalid_argument("string argument should contain only ASCII characters");
    auto value_of = [](char c) -> int
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+' || c == '-')
            return 62;
        if (c == '/' || c == '_')
            return 63;
        return -1;
    };
    std::string out;
    int quad_pos = 0;
    unsigned leftchar = 0;
    int pads = 0;
    for (const char ch : text)
    {
        if (ch == '=')
        {
            if (quad_pos >= 2 && quad_pos + ++pads >= 4)
                return out;
            continue;
        }
        const int v = value_of(ch);
        if (v < 0)
            continue;
        pads = 0;
        switch (quad_pos)
        {
        case 0:
            quad_pos = 1;
            leftchar = unsigned(v);
            break;
        case 1:
            quad_pos = 2;
            out.push_back(char((leftchar << 2) | (unsigned(v) >> 4)));
            leftchar = unsigned(v) & 0x0F;
            break;
        case 2:
            quad_pos = 3;
            out.push_back(char((leftchar << 4) | (unsigned(v) >> 2)));
            leftchar = unsigned(v) & 0x03;
            break;
        default:
            quad_pos = 0;
            out.push_back(char((leftchar << 6) | unsigned(v)));
            leftchar = 0;
            break;
        }
    }
    if (quad_pos == 1)
        throw Base64Error("Invalid base64-encoded string: number of data characters (" +
                          std::to_string(out.size() / 3 * 4 + 1) +
                          ") cannot be 1 more than a multiple of 4");
    if (quad_pos != 0)
        throw Base64Error("Incorrect padding");
    return out;
}

std::string float_repr(double value)
{
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    if (value == 0)
        return std::signbit(value) ? "-0.0" : "0.0";

    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::scientific);
    const std::string sci(buf, r.ptr);
    const bool neg = sci[0] == '-';
    const auto epos = sci.find('e');
    std::string digits;
    for (std::size_t i = neg ? 1 : 0; i < epos; ++i)
        if (sci[i] != '.')
            digits.push_back(sci[i]);
    while (digits.size() > 1 && digits.back() == '0')
        digits.pop_back();
    const int decpt = std::stoi(sci.substr(epos + 1)) + 1;

    std::string out = neg ? "-" : "";
    if (decpt <= -4 || decpt > 16)
    {
        out += digits[0];
        if (digits.size() > 1)
            out += "." + digits.substr(1);
        const int e = decpt - 1;
        char eb[16];
        std::snprintf(eb, sizeof(eb), "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
        out += eb;
    }
    else if (decpt <= 0)
        out += "0." + std::string(std::size_t(-decpt), '0') + digits;
    else if (std::size_t(decpt) >= digits.size())
        out += digits + std::string(std::size_t(decpt) - digits.size(), '0') + ".0";
    else
        out += digits.substr(0, std::size_t(decpt)) + "." + digits.substr(std::size_t(decpt));
    return out;
}

std::string repr(const OrderedJson& value)
{
    return write_value(value, Style{true, std::nullopt});
}

std::string type_name(const OrderedJson& value)
{
    switch (value.type())
    {
    case nlohmann::json::value_t::boolean:
        return "bool";
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned:
        return "int";
    case nlohmann::json::value_t::number_float:
        return "float";
    case nlohmann::json::value_t::string:
        return "str";
    case nlohmann::json::value_t::array:
        return "list";
    case nlohmann::json::value_t::object:
        return "dict";
    case nlohmann::json::value_t::binary:
        return is_big_int(value) ? "int" : "bytes";
    default:
        return "NoneType";
    }
}

std::string str(const OrderedJson& value)
{
    if (value.is_string())
        return value.get<std::string>();
    return repr(value);
}

bool truthy(const OrderedJson& value)
{
    switch (value.type())
    {
    case nlohmann::json::value_t::null:
        return false;
    case nlohmann::json::value_t::boolean:
        return value.get<bool>();
    case nlohmann::json::value_t::number_integer:
        return value.get<std::int64_t>() != 0;
    case nlohmann::json::value_t::number_unsigned:
        return value.get<std::uint64_t>() != 0;
    case nlohmann::json::value_t::number_float:
        return value.get<double>() != 0.0;
    case nlohmann::json::value_t::string:
        return !value.get_ref<const std::string&>().empty();
    case nlohmann::json::value_t::array:
    case nlohmann::json::value_t::object:
        return !value.empty();
    default:
        return true;
    }
}

std::string truncate_middle(const std::string& text)
{
    constexpr std::size_t kMax = 50;
    if (text.size() <= kMax)
        return text;
    const std::size_t mid = (kMax + 1) / 2;
    return text.substr(0, floor_boundary(text, mid)) + "..." +
           text.substr(ceil_boundary(text, text.size() - (mid - 1)));
}

std::string validation_error_str(const std::string& title, const std::vector<LineError>& errors)
{
    std::string out = std::to_string(errors.size()) +
                      (errors.size() == 1 ? " validation error for " : " validation errors for ") +
                      title;
    for (const auto& e : errors)
    {
        out += "\n";
        if (!e.loc.empty())
            out += e.loc + "\n";
        out += "  " + e.msg + " [type=" + e.type +
               ", input_value=" + truncate_middle(repr(e.input)) +
               ", input_type=" + type_name(e.input) + "]";
        if (e.url)
            out += std::string("\n    For further information visit ") + kErrorsUrl + e.type;
    }
    return out;
}

} // namespace fastmcpp::internal::pyjson
