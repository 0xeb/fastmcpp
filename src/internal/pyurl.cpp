#include "pyurl.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace fastmcpp::internal::pyurl
{

namespace
{

std::string lower_ascii(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool contains(const std::vector<std::string>& list, const std::string& s)
{
    return std::find(list.begin(), list.end(), s) != list.end();
}

const std::vector<std::string>& uses_params()
{
    static const std::vector<std::string> v{"",      "ftp",   "hdl",  "prospero", "http",  "imap",
                                            "https", "shttp", "rtsp", "rtsps",    "rtspu", "sip",
                                            "sips",  "mms",   "sftp", "tel"};
    return v;
}

const std::vector<std::string>& uses_netloc()
{
    static const std::vector<std::string> v{
        "",     "ftp",   "http",    "gopher", "nntp",  "telnet",       "imap",
        "wais", "file",  "mms",     "https",  "shttp", "snews",        "prospero",
        "rtsp", "rtsps", "rtspu",   "rsync",  "svn",   "svn+ssh",      "sftp",
        "nfs",  "git",   "git+ssh", "ws",     "wss",   "itms-services"};
    return v;
}

void check_bracketed_netloc(const std::string& netloc)
{
    const auto at = netloc.rfind('@');
    const std::string hostport = at == std::string::npos ? netloc : netloc.substr(at + 1);
    const auto open = hostport.find('[');
    if (open == std::string::npos)
        return;
    if (open != 0)
        throw std::invalid_argument("Invalid IPv6 URL");
    const std::string bracketed = hostport.substr(open + 1);
    const auto close = bracketed.find(']');
    const std::string host = bracketed.substr(0, close);
    const std::string rest = close == std::string::npos ? "" : bracketed.substr(close + 1);
    if (!rest.empty() && rest[0] != ':')
        throw std::invalid_argument("Invalid IPv6 URL");
    if (!host.empty() && host[0] == 'v')
    {
        std::size_t i = 1;
        while (i < host.size() && hex_value(host[i]) >= 0)
            ++i;
        if (i == 1 || i >= host.size() || host[i] != '.' || i + 1 >= host.size())
            throw std::invalid_argument("IPvFuture address is invalid");
        return;
    }
    const auto ip = ip_address(host);
    if (!ip)
        throw std::invalid_argument(py_repr(host) +
                                    " does not appear to be an IPv4 or IPv6 address");
    if (ip->version == 4)
        throw std::invalid_argument("An IPv4 address cannot be in brackets");
}

/// The host and port text of a netloc (urllib's _hostinfo).
std::pair<std::string, std::optional<std::string>> hostinfo(const std::string& netloc)
{
    const auto at = netloc.rfind('@');
    const std::string rest = at == std::string::npos ? netloc : netloc.substr(at + 1);
    std::string host, port;
    const auto open = rest.find('[');
    if (open != std::string::npos)
    {
        const std::string bracketed = rest.substr(open + 1);
        const auto close = bracketed.find(']');
        host = bracketed.substr(0, close);
        const std::string after = close == std::string::npos ? "" : bracketed.substr(close + 1);
        const auto colon = after.find(':');
        port = colon == std::string::npos ? "" : after.substr(colon + 1);
    }
    else
    {
        const auto colon = rest.find(':');
        host = rest.substr(0, colon);
        port = colon == std::string::npos ? "" : rest.substr(colon + 1);
    }
    return {host, port.empty() ? std::nullopt : std::optional<std::string>(port)};
}

// ------------------------------------------------------------------ WHATWG host

const char* kHex = "0123456789ABCDEF";

std::string percent_encode(const std::string& s, const std::string& extra)
{
    std::string out;
    for (unsigned char c : s)
        if (c < 0x20 || c >= 0x7F || extra.find(static_cast<char>(c)) != std::string::npos)
        {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
        else
            out.push_back(static_cast<char>(c));
    return out;
}

const std::string kPathSet = " \"#<>?`{}";
const std::string kQuerySet = " \"#<>'";
const std::string kSpecialFreeQuerySet = " \"#<>";
const std::string kFragmentSet = " \"<>`";
const std::string kUserinfoSet = " \"#<>?`{}/:;=@[\\]^|";

std::string resolve_dot_segments(const std::string& path)
{
    std::vector<std::string> segments;
    std::size_t i = !path.empty() && path[0] == '/' ? 1 : 0;
    bool trailing = false;
    while (i <= path.size())
    {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos)
            j = path.size();
        const std::string seg = path.substr(i, j - i);
        const std::string ls = lower_ascii(seg);
        if (seg == "." || ls == "%2e")
            trailing = true;
        else if (seg == ".." || ls == ".%2e" || ls == "%2e." || ls == "%2e%2e")
        {
            if (!segments.empty())
                segments.pop_back();
            trailing = true;
        }
        else
        {
            segments.push_back(seg);
            trailing = false;
        }
        if (j >= path.size())
            break;
        i = j + 1;
    }
    std::string out;
    for (const auto& s : segments)
        out += "/" + s;
    if (trailing)
        out += "/";
    return out;
}

/// WHATWG IPv4 number parser; nullopt on failure.
std::optional<std::uint64_t> ipv4_number(std::string input)
{
    if (input.empty())
        return std::nullopt;
    int radix = 10;
    if (input.size() >= 2 && input[0] == '0' && (input[1] == 'x' || input[1] == 'X'))
    {
        input = input.substr(2);
        radix = 16;
    }
    else if (input.size() >= 2 && input[0] == '0')
    {
        input = input.substr(1);
        radix = 8;
    }
    if (input.empty())
        return 0;
    std::uint64_t value = 0;
    for (char c : input)
    {
        const int d = hex_value(c);
        if (d < 0 || d >= radix)
            return std::nullopt;
        if (value < (std::uint64_t(1) << 40))
            value = value * static_cast<unsigned>(radix) + static_cast<unsigned>(d);
    }
    return value;
}

std::vector<std::string> split_dots(const std::string& s)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        const auto dot = s.find('.', start);
        parts.push_back(
            s.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return parts;
}

bool ends_in_number(const std::string& domain)
{
    auto parts = split_dots(domain);
    if (parts.back().empty())
    {
        if (parts.size() == 1)
            return false;
        parts.pop_back();
    }
    const std::string& last = parts.back();
    if (!last.empty() && std::all_of(last.begin(), last.end(), is_digit))
        return true;
    return ipv4_number(last).has_value();
}

std::optional<std::uint32_t> parse_whatwg_ipv4(const std::string& domain)
{
    auto parts = split_dots(domain);
    if (parts.back().empty() && parts.size() > 1)
        parts.pop_back();
    if (parts.size() > 4)
        return std::nullopt;
    std::vector<std::uint64_t> numbers;
    for (const auto& p : parts)
    {
        const auto n = ipv4_number(p);
        if (!n)
            return std::nullopt;
        numbers.push_back(*n);
    }
    for (std::size_t i = 0; i + 1 < numbers.size(); ++i)
        if (numbers[i] > 255)
            return std::nullopt;
    std::uint64_t limit = 1;
    for (std::size_t i = 0; i < 5 - numbers.size(); ++i)
        limit *= 256;
    if (numbers.back() >= limit)
        return std::nullopt;
    std::uint64_t ipv4 = numbers.back();
    for (std::size_t i = 0; i + 1 < numbers.size(); ++i)
    {
        std::uint64_t mul = 1;
        for (std::size_t k = 0; k < 3 - i; ++k)
            mul *= 256;
        ipv4 += numbers[i] * mul;
    }
    return static_cast<std::uint32_t>(ipv4);
}

std::string serialize_ipv4(std::uint32_t v)
{
    return std::to_string(v >> 24) + "." + std::to_string((v >> 16) & 0xFF) + "." +
           std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
}

/// WHATWG IPv6 parser; nullopt on failure.
std::optional<std::array<std::uint16_t, 8>> parse_whatwg_ipv6(const std::string& in)
{
    std::array<std::uint16_t, 8> address{};
    int piece = 0;
    int compress = -1;
    std::size_t p = 0;
    const auto at = [&](std::size_t i) -> int { return i < in.size() ? in[i] : -1; };
    if (at(p) == ':')
    {
        if (at(p + 1) != ':')
            return std::nullopt;
        p += 2;
        ++piece;
        compress = piece;
    }
    while (at(p) != -1)
    {
        if (piece == 8)
            return std::nullopt;
        if (at(p) == ':')
        {
            if (compress != -1)
                return std::nullopt;
            ++p;
            ++piece;
            compress = piece;
            continue;
        }
        unsigned value = 0;
        int length = 0;
        while (length < 4 && at(p) != -1 && hex_value(static_cast<char>(at(p))) >= 0)
        {
            value = value * 16 + static_cast<unsigned>(hex_value(static_cast<char>(at(p))));
            ++p;
            ++length;
        }
        if (at(p) == '.')
        {
            if (length == 0)
                return std::nullopt;
            p -= static_cast<std::size_t>(length);
            if (piece > 6)
                return std::nullopt;
            int seen = 0;
            while (at(p) != -1)
            {
                int ipv4_piece = -1;
                if (seen > 0)
                {
                    if (at(p) == '.' && seen < 4)
                        ++p;
                    else
                        return std::nullopt;
                }
                if (at(p) == -1 || !is_digit(static_cast<char>(at(p))))
                    return std::nullopt;
                while (at(p) != -1 && is_digit(static_cast<char>(at(p))))
                {
                    const int number = at(p) - '0';
                    if (ipv4_piece == -1)
                        ipv4_piece = number;
                    else if (ipv4_piece == 0)
                        return std::nullopt;
                    else
                        ipv4_piece = ipv4_piece * 10 + number;
                    if (ipv4_piece > 255)
                        return std::nullopt;
                    ++p;
                }
                address[piece] = static_cast<std::uint16_t>(address[piece] * 0x100 + ipv4_piece);
                ++seen;
                if (seen == 2 || seen == 4)
                    ++piece;
            }
            if (seen != 4)
                return std::nullopt;
            break;
        }
        else if (at(p) == ':')
        {
            ++p;
            if (at(p) == -1)
                return std::nullopt;
        }
        else if (at(p) != -1)
            return std::nullopt;
        address[piece] = static_cast<std::uint16_t>(value);
        ++piece;
    }
    if (compress != -1)
    {
        int swaps = piece - compress;
        piece = 7;
        while (piece != 0 && swaps > 0)
        {
            std::swap(address[piece], address[compress + swaps - 1]);
            --piece;
            --swaps;
        }
    }
    else if (piece != 8)
        return std::nullopt;
    return address;
}

std::string serialize_ipv6(const std::array<std::uint16_t, 8>& a)
{
    int best = -1, best_len = 1;
    for (int i = 0; i < 8;)
    {
        if (a[i] != 0)
        {
            ++i;
            continue;
        }
        int j = i;
        while (j < 8 && a[j] == 0)
            ++j;
        if (j - i > best_len)
        {
            best = i;
            best_len = j - i;
        }
        i = j;
    }
    static const char* digits = "0123456789abcdef";
    std::string out;
    bool ignore0 = false;
    for (int i = 0; i < 8; ++i)
    {
        if (ignore0 && a[i] == 0)
            continue;
        ignore0 = false;
        if (best == i)
        {
            out += i == 0 ? "::" : ":";
            ignore0 = true;
            continue;
        }
        std::string h;
        unsigned v = a[i];
        do
        {
            h.insert(h.begin(), digits[v & 0xF]);
            v >>= 4;
        } while (v);
        out += h;
        if (i != 7)
            out += ":";
    }
    return out;
}

// ---- IDNA (a practical subset of UTS #46: case mapping for common scripts, Punycode)

bool decode_utf8(const std::string& s, std::vector<std::uint32_t>& out)
{
    for (std::size_t i = 0; i < s.size();)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp;
        std::size_t len;
        if (c < 0x80)
            cp = c, len = 1;
        else if ((c & 0xE0) == 0xC0)
            cp = c & 0x1F, len = 2;
        else if ((c & 0xF0) == 0xE0)
            cp = c & 0x0F, len = 3;
        else if ((c & 0xF8) == 0xF0)
            cp = c & 0x07, len = 4;
        else
            return false;
        if (i + len > s.size())
            return false;
        for (std::size_t k = 1; k < len; ++k)
        {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        out.push_back(cp);
        i += len;
    }
    return true;
}

std::uint32_t simple_lower(std::uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z')
        return cp + 0x20;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
        return cp + 0x20;
    if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177))
        return cp % 2 == 0 ? cp + 1 : cp;
    if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E))
        return cp % 2 == 1 ? cp + 1 : cp;
    if (cp == 0x178)
        return 0xFF;
    if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2)
        return cp + 0x20;
    if (cp >= 0x410 && cp <= 0x42F)
        return cp + 0x20;
    if (cp >= 0x400 && cp <= 0x40F)
        return cp + 0x50;
    return cp;
}

void encode_utf8(std::uint32_t cp, std::string& out)
{
    if (cp < 0x80)
        out.push_back(static_cast<char>(cp));
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// Code points of @p s; bytes that are not valid UTF-8 map to themselves.
std::vector<std::uint32_t> code_points(const std::string& s)
{
    std::vector<std::uint32_t> cps;
    if (decode_utf8(s, cps))
        return cps;
    cps.clear();
    for (unsigned char c : s)
        cps.push_back(c);
    return cps;
}

std::string unicode_lower(const std::string& s)
{
    std::vector<std::uint32_t> cps;
    if (!decode_utf8(s, cps))
        return lower_ascii(s);
    std::string out;
    for (auto cp : cps)
        encode_utf8(simple_lower(cp), out);
    return out;
}

std::string punycode(const std::vector<std::uint32_t>& input)
{
    constexpr std::uint32_t base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
    const auto digit = [](std::uint32_t d) -> char
    { return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26)); };
    const auto adapt = [&](std::uint32_t delta, std::uint32_t points, bool first)
    {
        delta = first ? delta / damp : delta / 2;
        delta += delta / points;
        std::uint32_t k = 0;
        while (delta > ((base - tmin) * tmax) / 2)
        {
            delta /= base - tmin;
            k += base;
        }
        return k + (base - tmin + 1) * delta / (delta + skew);
    };
    std::string out;
    for (auto cp : input)
        if (cp < 0x80)
            out.push_back(static_cast<char>(cp));
    const std::uint32_t basic = static_cast<std::uint32_t>(out.size());
    std::uint32_t handled = basic;
    if (basic > 0)
        out.push_back('-');
    std::uint32_t n = 0x80, delta = 0, bias = 72;
    while (handled < input.size())
    {
        std::uint32_t m = 0xFFFFFFFF;
        for (auto cp : input)
            if (cp >= n && cp < m)
                m = cp;
        delta += (m - n) * (handled + 1);
        n = m;
        for (auto cp : input)
        {
            if (cp < n)
                ++delta;
            if (cp == n)
            {
                std::uint32_t q = delta;
                for (std::uint32_t k = base;; k += base)
                {
                    const std::uint32_t t = k <= bias ? tmin : (k >= bias + tmax ? tmax : k - bias);
                    if (q < t)
                        break;
                    out.push_back(digit(t + (q - t) % (base - t)));
                    q = (q - t) / (base - t);
                }
                out.push_back(digit(q));
                bias = adapt(delta, handled + 1, handled == basic);
                delta = 0;
                ++handled;
            }
        }
        ++delta;
        ++n;
    }
    return out;
}

std::optional<std::vector<std::uint32_t>> punycode_decode(const std::string& input)
{
    constexpr std::uint32_t base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700;
    const auto adapt = [&](std::uint32_t delta, std::uint32_t points, bool first)
    {
        delta = first ? delta / damp : delta / 2;
        delta += delta / points;
        std::uint32_t k = 0;
        while (delta > ((base - tmin) * tmax) / 2)
        {
            delta /= base - tmin;
            k += base;
        }
        return k + (base - tmin + 1) * delta / (delta + skew);
    };
    std::vector<std::uint32_t> out;
    const auto dash = input.rfind('-');
    std::size_t in = 0;
    if (dash != std::string::npos)
    {
        for (std::size_t i = 0; i < dash; ++i)
        {
            if (static_cast<unsigned char>(input[i]) >= 0x80)
                return std::nullopt;
            out.push_back(static_cast<unsigned char>(input[i]));
        }
        in = dash + 1;
    }
    std::uint32_t n = 0x80, i = 0, bias = 72;
    while (in < input.size())
    {
        const std::uint32_t old_i = i;
        std::uint32_t w = 1;
        for (std::uint32_t k = base;; k += base)
        {
            if (in >= input.size())
                return std::nullopt;
            const char c = input[in++];
            std::uint32_t digit;
            if (c >= 'a' && c <= 'z')
                digit = static_cast<std::uint32_t>(c - 'a');
            else if (c >= 'A' && c <= 'Z')
                digit = static_cast<std::uint32_t>(c - 'A');
            else if (c >= '0' && c <= '9')
                digit = static_cast<std::uint32_t>(c - '0' + 26);
            else
                return std::nullopt;
            if (digit > (0xFFFFFFFFu - i) / w)
                return std::nullopt;
            i += digit * w;
            const std::uint32_t t = k <= bias ? tmin : (k >= bias + tmax ? tmax : k - bias);
            if (digit < t)
                break;
            if (w > 0xFFFFFFFFu / (base - t))
                return std::nullopt;
            w *= base - t;
        }
        const auto len = static_cast<std::uint32_t>(out.size() + 1);
        bias = adapt(i - old_i, len, old_i == 0);
        if (i / len > 0x10FFFF - n)
            return std::nullopt;
        n += i / len;
        i %= len;
        if (n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF))
            return std::nullopt;
        out.insert(out.begin() + i, n);
        ++i;
    }
    return out;
}

/// An "xn--" label is valid when it decodes to a non-ASCII label whose
/// encoding round-trips.
bool valid_ace_label(const std::string& label)
{
    const std::string body = label.substr(4);
    if (body.empty())
        return false;
    const auto decoded = punycode_decode(body);
    if (!decoded || decoded->empty())
        return false;
    bool non_ascii = false;
    for (auto cp : *decoded)
    {
        if (cp >= 0x80)
            non_ascii = true;
        if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
            return false;
    }
    return non_ascii && "xn--" + punycode(*decoded) == label;
}

bool is_forbidden_domain_byte(unsigned char c)
{
    static const std::string forbidden = " #/:<>?@[\\]^|%";
    return c <= 0x20 || c == 0x7F || forbidden.find(static_cast<char>(c)) != std::string::npos;
}

/// The ASCII form of a domain; nullopt when it is not a valid domain.
std::optional<std::string> domain_to_ascii(const std::string& domain)
{
    std::vector<std::uint32_t> cps;
    if (!decode_utf8(domain, cps))
        return std::nullopt;
    std::string out;
    std::vector<std::uint32_t> label;
    const auto flush = [&]() -> bool
    {
        bool ascii = true;
        for (auto& cp : label)
        {
            cp = simple_lower(cp);
            if (cp >= 0x80)
                ascii = false;
            else if (is_forbidden_domain_byte(static_cast<unsigned char>(cp)))
                return false;
        }
        if (ascii)
        {
            std::string text;
            for (auto cp : label)
                text.push_back(static_cast<char>(cp));
            if (text.size() >= 4 && text.compare(0, 4, "xn--") == 0 && !valid_ace_label(text))
                return false;
            out += text;
        }
        else
            out += "xn--" + punycode(label);
        label.clear();
        return true;
    };
    for (auto cp : cps)
    {
        // Ideographic and fullwidth full stops separate labels too.
        if (cp == '.' || cp == 0x3002 || cp == 0xFF0E || cp == 0xFF61)
        {
            if (!flush())
                return std::nullopt;
            out.push_back('.');
        }
        else
            label.push_back(cp);
    }
    if (!flush())
        return std::nullopt;
    return out;
}

/// The serialized host of a special-scheme URL, or the validation message.
std::pair<std::optional<std::string>, std::string> parse_host(const std::string& input)
{
    if (!input.empty() && input[0] == '[')
    {
        if (input.back() != ']')
            return {std::nullopt, "invalid IPv6 address"};
        const auto v6 = parse_whatwg_ipv6(input.substr(1, input.size() - 2));
        if (!v6)
            return {std::nullopt, "invalid IPv6 address"};
        return {"[" + serialize_ipv6(*v6) + "]", ""};
    }
    const auto ascii = domain_to_ascii(unquote(input));
    if (!ascii)
        return {std::nullopt, "invalid international domain name"};
    if (ascii->empty())
        return {std::nullopt, "empty host"};
    if (ends_in_number(*ascii))
    {
        const auto v4 = parse_whatwg_ipv4(*ascii);
        if (!v4)
            return {std::nullopt, "invalid IPv4 address"};
        return {serialize_ipv4(*v4), ""};
    }
    return {*ascii, ""};
}

bool is_special_scheme(const std::string& scheme, int* default_port)
{
    static const std::vector<std::pair<std::string, int>> kSpecial{
        {"http", 80}, {"https", 443}, {"ws", 80}, {"wss", 443}, {"ftp", 21}, {"file", -1}};
    for (const auto& [s, p] : kSpecial)
        if (s == scheme)
        {
            if (default_port)
                *default_port = p;
            return true;
        }
    return false;
}

// ---- ipaddress

struct Net
{
    IPAddress addr;
    int prefix;
};

bool in_net(const IPAddress& a, const Net& n)
{
    if (a.version != n.addr.version)
        return false;
    const int bits = n.prefix;
    for (int i = 0; i < bits / 8; ++i)
        if (a.bytes[i] != n.addr.bytes[i])
            return false;
    if (bits % 8)
    {
        const std::uint8_t mask = static_cast<std::uint8_t>(0xFF << (8 - bits % 8));
        if ((a.bytes[bits / 8] & mask) != (n.addr.bytes[bits / 8] & mask))
            return false;
    }
    return true;
}

Net net(const char* text, int prefix)
{
    return Net{*ip_address(text), prefix};
}

const std::vector<Net>& v4_private()
{
    static const std::vector<Net> v{
        net("0.0.0.0", 8),      net("10.0.0.0", 8),        net("127.0.0.0", 8),
        net("169.254.0.0", 16), net("172.16.0.0", 12),     net("192.0.0.0", 24),
        net("192.0.0.170", 31), net("192.0.2.0", 24),      net("192.168.0.0", 16),
        net("198.18.0.0", 15),  net("198.51.100.0", 24),   net("203.0.113.0", 24),
        net("240.0.0.0", 4),    net("255.255.255.255", 32)};
    return v;
}

const std::vector<Net>& v4_private_exceptions()
{
    static const std::vector<Net> v{net("192.0.0.9", 32), net("192.0.0.10", 32)};
    return v;
}

const std::vector<Net>& v6_private()
{
    static const std::vector<Net> v{
        net("::1", 128),   net("::", 128),    net("::ffff:0:0", 96), net("64:ff9b:1::", 48),
        net("100::", 64),  net("2001::", 23), net("2001:db8::", 32), net("2002::", 16),
        net("3fff::", 20), net("fc00::", 7),  net("fe80::", 10)};
    return v;
}

const std::vector<Net>& v6_private_exceptions()
{
    static const std::vector<Net> v{net("2001:1::1", 128), net("2001:1::2", 128),
                                    net("2001:3::", 32),   net("2001:4:112::", 48),
                                    net("2001:20::", 28),  net("2001:30::", 28)};
    return v;
}

std::optional<std::uint8_t> parse_v4_octet(const std::string& s)
{
    if (s.empty() || s.size() > 3 || !std::all_of(s.begin(), s.end(), is_digit))
        return std::nullopt;
    if (s != "0" && s[0] == '0')
        return std::nullopt;
    const int v = std::stoi(s);
    if (v > 255)
        return std::nullopt;
    return static_cast<std::uint8_t>(v);
}

std::optional<std::uint32_t> parse_v4(const std::string& s)
{
    const auto parts = split_dots(s);
    if (parts.size() != 4)
        return std::nullopt;
    std::uint32_t v = 0;
    for (const auto& p : parts)
    {
        const auto o = parse_v4_octet(p);
        if (!o)
            return std::nullopt;
        v = (v << 8) | *o;
    }
    return v;
}

std::optional<std::array<std::uint16_t, 8>> parse_v6(const std::string& s)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true)
    {
        const auto c = s.find(':', start);
        parts.push_back(s.substr(start, c == std::string::npos ? std::string::npos : c - start));
        if (c == std::string::npos)
            break;
        start = c + 1;
    }
    if (parts.size() < 3)
        return std::nullopt;
    if (parts.back().find('.') != std::string::npos)
    {
        const auto v4 = parse_v4(parts.back());
        if (!v4)
            return std::nullopt;
        parts.pop_back();
        static const char* digits = "0123456789abcdef";
        const auto hex = [&](unsigned v)
        {
            std::string h;
            do
            {
                h.insert(h.begin(), digits[v & 0xF]);
                v >>= 4;
            } while (v);
            return h;
        };
        parts.push_back(hex((*v4 >> 16) & 0xFFFF));
        parts.push_back(hex(*v4 & 0xFFFF));
    }
    if (parts.size() > 9)
        return std::nullopt;
    int skip = -1;
    for (std::size_t i = 1; i + 1 < parts.size(); ++i)
        if (parts[i].empty())
        {
            if (skip != -1)
                return std::nullopt;
            skip = static_cast<int>(i);
        }
    int hi, lo, skipped;
    if (skip != -1)
    {
        hi = skip;
        lo = static_cast<int>(parts.size()) - skip - 1;
        if (parts.front().empty() && --hi)
            return std::nullopt;
        if (parts.back().empty() && --lo)
            return std::nullopt;
        skipped = 8 - (hi + lo);
        if (skipped < 1)
            return std::nullopt;
    }
    else
    {
        if (parts.size() != 8 || parts.front().empty() || parts.back().empty())
            return std::nullopt;
        hi = 8;
        lo = 0;
        skipped = 0;
    }
    const auto hextet = [](const std::string& h) -> std::optional<std::uint16_t>
    {
        if (h.empty() || h.size() > 4)
            return std::nullopt;
        unsigned v = 0;
        for (char c : h)
        {
            const int d = hex_value(c);
            if (d < 0)
                return std::nullopt;
            v = v * 16 + static_cast<unsigned>(d);
        }
        return static_cast<std::uint16_t>(v);
    };
    std::array<std::uint16_t, 8> out{};
    int idx = 0;
    for (int i = 0; i < hi; ++i)
    {
        const auto h = hextet(parts[static_cast<std::size_t>(i)]);
        if (!h)
            return std::nullopt;
        out[idx++] = *h;
    }
    idx += skipped;
    for (int i = lo; i > 0; --i)
    {
        const auto h = hextet(parts[parts.size() - static_cast<std::size_t>(i)]);
        if (!h)
            return std::nullopt;
        out[idx++] = *h;
    }
    return out;
}

} // namespace

// ================================================================== urllib.parse

ParseResult urlsplit(const std::string& input)
{
    std::size_t b = 0;
    while (b < input.size() && static_cast<unsigned char>(input[b]) <= 0x20)
        ++b;
    std::string url;
    for (std::size_t i = b; i < input.size(); ++i)
        if (input[i] != '\t' && input[i] != '\r' && input[i] != '\n')
            url.push_back(input[i]);

    ParseResult r;
    const auto colon = url.find(':');
    if (colon != std::string::npos && colon > 0 &&
        std::isalpha(static_cast<unsigned char>(url[0])) &&
        std::all_of(url.begin(), url.begin() + static_cast<std::ptrdiff_t>(colon),
                    [](char c)
                    {
                        return std::isalnum(static_cast<unsigned char>(c)) || c == '+' ||
                               c == '-' || c == '.';
                    }))
    {
        r.scheme = lower_ascii(url.substr(0, colon));
        url = url.substr(colon + 1);
    }
    if (url.size() >= 2 && url[0] == '/' && url[1] == '/')
    {
        auto end = url.size();
        for (char d : {'/', '?', '#'})
        {
            const auto p = url.find(d, 2);
            if (p != std::string::npos && p < end)
                end = p;
        }
        r.netloc = url.substr(2, end - 2);
        url = url.substr(end);
        const bool open = r.netloc.find('[') != std::string::npos;
        const bool close = r.netloc.find(']') != std::string::npos;
        if (open != close)
            throw std::invalid_argument("Invalid IPv6 URL");
        if (open && close)
            check_bracketed_netloc(r.netloc);
    }
    if (const auto hash = url.find('#'); hash != std::string::npos)
    {
        r.fragment = url.substr(hash + 1);
        url = url.substr(0, hash);
    }
    if (const auto q = url.find('?'); q != std::string::npos)
    {
        r.query = url.substr(q + 1);
        url = url.substr(0, q);
    }
    r.path = url;
    return r;
}

ParseResult urlparse(const std::string& url)
{
    ParseResult r = urlsplit(url);
    if (contains(uses_params(), r.scheme) && r.path.find(';') != std::string::npos)
    {
        std::size_t i;
        if (r.path.find('/') != std::string::npos)
        {
            i = r.path.find(';', r.path.rfind('/'));
            if (i == std::string::npos)
                return r;
        }
        else
            i = r.path.find(';');
        r.params = r.path.substr(i + 1);
        r.path = r.path.substr(0, i);
    }
    return r;
}

std::string urlunsplit(const ParseResult& p)
{
    std::string url = p.path;
    if (!p.netloc.empty())
    {
        if (!url.empty() && url[0] != '/')
            url = "/" + url;
        url = "//" + p.netloc + url;
    }
    else if (url.size() >= 2 && url[0] == '/' && url[1] == '/')
        url = "//" + url;
    else if (!p.scheme.empty() && contains(uses_netloc(), p.scheme) &&
             (url.empty() || url[0] == '/'))
        url = "//" + url;
    if (!p.scheme.empty())
        url = p.scheme + ":" + url;
    if (!p.query.empty())
        url += "?" + p.query;
    if (!p.fragment.empty())
        url += "#" + p.fragment;
    return url;
}

std::string urlunparse(const ParseResult& p)
{
    ParseResult q = p;
    if (!p.params.empty())
        q.path = p.path + ";" + p.params;
    return urlunsplit(q);
}

std::optional<std::string> username(const ParseResult& p)
{
    const auto at = p.netloc.rfind('@');
    if (at == std::string::npos)
        return std::nullopt;
    const std::string info = p.netloc.substr(0, at);
    return info.substr(0, info.find(':'));
}

std::optional<std::string> password(const ParseResult& p)
{
    const auto at = p.netloc.rfind('@');
    if (at == std::string::npos)
        return std::nullopt;
    const std::string info = p.netloc.substr(0, at);
    const auto colon = info.find(':');
    if (colon == std::string::npos)
        return std::nullopt;
    return info.substr(colon + 1);
}

std::optional<std::string> hostname(const ParseResult& p)
{
    const std::string host = hostinfo(p.netloc).first;
    if (host.empty())
        return std::nullopt;
    const auto pct = host.find('%');
    if (pct == std::string::npos)
        return unicode_lower(host);
    return unicode_lower(host.substr(0, pct)) + host.substr(pct);
}

std::optional<int> port(const ParseResult& p)
{
    const auto text = hostinfo(p.netloc).second;
    if (!text)
        return std::nullopt;
    if (!std::all_of(text->begin(), text->end(), is_digit))
        throw std::invalid_argument("Port could not be cast to integer value as " + py_repr(*text));
    std::size_t i = 0;
    while (i + 1 < text->size() && (*text)[i] == '0')
        ++i;
    const std::string digits = text->substr(i);
    if (digits.size() > 5 || std::stol(digits) > 65535)
        throw std::invalid_argument("Port out of range 0-65535");
    return static_cast<int>(std::stol(digits));
}

std::string unquote(const std::string& s, bool plus)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
        if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0)
        {
            out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
            i += 2;
        }
        else if (plus && s[i] == '+')
            out.push_back(' ');
        else
            out.push_back(s[i]);
    return out;
}

std::string quote_plus(const std::string& s)
{
    std::string out;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            out.push_back(static_cast<char>(c));
        else if (c == ' ')
            out.push_back('+');
        else
        {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

std::string urlencode(const std::vector<std::pair<std::string, std::string>>& params)
{
    std::string out;
    for (const auto& [k, v] : params)
    {
        if (!out.empty())
            out.push_back('&');
        out += quote_plus(k) + "=" + quote_plus(v);
    }
    return out;
}

bool fnmatchcase(const std::string& name_text, const std::string& pat_text)
{
    const auto name = code_points(name_text);
    const auto pat = code_points(pat_text);
    // Matches one pattern element at pat[pi] against c; sets next to the
    // index after the element.
    const auto element = [&](std::size_t pi, std::uint32_t c, std::size_t& next) -> bool
    {
        if (pat[pi] == '?')
        {
            next = pi + 1;
            return true;
        }
        if (pat[pi] == '[')
        {
            std::size_t j = pi + 1;
            if (j < pat.size() && pat[j] == '!')
                ++j;
            if (j < pat.size() && pat[j] == ']')
                ++j;
            while (j < pat.size() && pat[j] != ']')
                ++j;
            if (j < pat.size())
            {
                std::size_t k = pi + 1;
                next = j + 1;
                bool negate = false;
                if (pat[k] == '!')
                {
                    negate = true;
                    ++k;
                }
                bool hit = false;
                for (; k < j; ++k)
                    if (k + 2 < j && pat[k + 1] == '-')
                    {
                        if (c >= pat[k] && c <= pat[k + 2])
                            hit = true;
                        k += 2;
                    }
                    else if (pat[k] == c)
                        hit = true;
                return hit != negate;
            }
        }
        next = pi + 1;
        return pat[pi] == c;
    };
    std::size_t ni = 0, pi = 0;
    std::size_t star_pi = std::string::npos, star_ni = 0;
    while (ni < name.size())
    {
        std::size_t next = 0;
        if (pi < pat.size() && pat[pi] == '*')
        {
            star_pi = pi++;
            star_ni = ni;
        }
        else if (pi < pat.size() && element(pi, name[ni], next))
        {
            pi = next;
            ++ni;
        }
        else if (star_pi != std::string::npos)
        {
            pi = star_pi + 1;
            ni = ++star_ni;
        }
        else
            return false;
    }
    while (pi < pat.size() && pat[pi] == '*')
        ++pi;
    return pi == pat.size();
}

namespace
{

/// str.isprintable() for the code points that matter in practice: controls,
/// separators other than the space, format characters and private use.
bool is_printable(std::uint32_t cp)
{
    if (cp < 0x20 || (cp >= 0x7F && cp <= 0xA0) || cp == 0xAD)
        return false;
    if (cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) ||
        (cp >= 0x205F && cp <= 0x206F) || cp == 0x3000 || cp == 0xFEFF ||
        (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0xE000 && cp <= 0xF8FF) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp >= 0xF0000)
        return false;
    return true;
}

} // namespace

std::string py_repr(const std::string& s)
{
    const bool dq = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
    const char q = dq ? '"' : '\'';
    static const char* d = "0123456789abcdef";
    std::string out(1, q);
    for (auto cp : code_points(s))
        if (cp == '\\')
            out += "\\\\";
        else if (cp == static_cast<unsigned char>(q))
            out += std::string("\\") + q;
        else if (cp == '\n')
            out += "\\n";
        else if (cp == '\r')
            out += "\\r";
        else if (cp == '\t')
            out += "\\t";
        else if (!is_printable(cp))
        {
            const int width = cp < 0x100 ? 2 : (cp < 0x10000 ? 4 : 8);
            out += width == 2 ? "\\x" : (width == 4 ? "\\u" : "\\U");
            for (int shift = (width - 1) * 4; shift >= 0; shift -= 4)
                out.push_back(d[(cp >> shift) & 0xF]);
        }
        else
            encode_utf8(cp, out);
    out.push_back(q);
    return out;
}

// ================================================================== URL validation

namespace
{

bool is_forbidden_host_byte(unsigned char c)
{
    static const std::string forbidden = std::string(1, '\0') + "\t\n\r #/:<>?@[\\]^|";
    return forbidden.find(static_cast<char>(c)) != std::string::npos;
}

/// The host of a URL with a non-special scheme (an opaque host).
std::pair<std::optional<std::string>, std::string> parse_opaque_host(const std::string& input)
{
    if (!input.empty() && input[0] == '[')
        return parse_host(input);
    for (unsigned char c : input)
        if (is_forbidden_host_byte(c))
            return {std::nullopt, "invalid domain character"};
    return {percent_encode(input, ""), ""};
}

/// pydantic keeps an empty path when the input had none.
bool input_path_is_empty(const std::string& url_str)
{
    const auto colon = url_str.find(':');
    std::size_t i = colon + 1;
    while (i < url_str.size() && url_str[i] == '/')
        ++i;
    for (; i < url_str.size(); ++i)
    {
        if (url_str[i] == '/')
            return false;
        if (url_str[i] == '?' || url_str[i] == '#')
            return true;
    }
    return true;
}

} // namespace

UrlResult validate_url(const std::string& input, bool preserve_empty_path, bool http_only)
{
    UrlResult r;
    const std::string prefix = "Input should be a valid URL, ";
    if (input.empty())
    {
        r.error = prefix + "input is empty";
        return r;
    }
    std::size_t b = 0, e = input.size();
    while (b < e && static_cast<unsigned char>(input[b]) <= 0x20)
        ++b;
    while (e > b && static_cast<unsigned char>(input[e - 1]) <= 0x20)
        --e;
    std::string url;
    for (std::size_t i = b; i < e; ++i)
        if (input[i] != '\t' && input[i] != '\n' && input[i] != '\r')
            url.push_back(input[i]);
    std::size_t k = 0;
    if (url.empty() || !std::isalpha(static_cast<unsigned char>(url[0])))
    {
        r.error = prefix + "relative URL without a base";
        return r;
    }
    while (k < url.size() && (std::isalnum(static_cast<unsigned char>(url[k])) || url[k] == '+' ||
                              url[k] == '-' || url[k] == '.'))
        ++k;
    if (k >= url.size() || url[k] != ':')
    {
        r.error = prefix + "relative URL without a base";
        return r;
    }
    const std::string scheme = lower_ascii(url.substr(0, k));
    std::string rest = url.substr(k + 1);
    const auto scheme_ok = [&]() -> bool
    {
        if (http_only && scheme != "http" && scheme != "https")
        {
            r.error = "URL scheme should be 'http' or 'https'";
            return false;
        }
        return true;
    };
    if (scheme == "file")
    {
        if (scheme_ok())
            r.url = scheme + ":" + percent_encode(rest, " ");
        return r;
    }
    int default_port = -1;
    const bool special = is_special_scheme(scheme, &default_port);
    const auto is_slash = [&](char c) { return c == '/' || (special && c == '\\'); };

    bool has_authority = false;
    std::string userinfo, host, port;
    if (special)
    {
        std::size_t p = 0;
        while (p < rest.size() && is_slash(rest[p]))
            ++p;
        rest = rest.substr(p);
        has_authority = true;
    }
    else if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/')
    {
        rest = rest.substr(2);
        has_authority = true;
    }
    if (has_authority)
    {
        const std::size_t auth_end = rest.find_first_of(special ? "/\\?#" : "/?#");
        std::string authority = rest.substr(0, auth_end);
        rest = auth_end == std::string::npos ? std::string() : rest.substr(auth_end);
        const auto at = authority.rfind('@');
        if (at != std::string::npos)
        {
            const std::string info = authority.substr(0, at);
            authority = authority.substr(at + 1);
            const auto colon = info.find(':');
            const std::string user = percent_encode(info.substr(0, colon), kUserinfoSet);
            const std::string pass = colon == std::string::npos
                                         ? std::string()
                                         : percent_encode(info.substr(colon + 1), kUserinfoSet);
            if (!user.empty() || !pass.empty())
                userinfo = user + (pass.empty() ? "" : ":" + pass) + "@";
        }
        std::size_t split = std::string::npos;
        bool in_brackets = false;
        for (std::size_t i = 0; i < authority.size(); ++i)
        {
            if (authority[i] == '[')
                in_brackets = true;
            else if (authority[i] == ']')
                in_brackets = false;
            else if (authority[i] == ':' && !in_brackets)
            {
                split = i;
                break;
            }
        }
        const std::string host_text = authority.substr(0, split);
        if (host_text.empty() && (special || split != std::string::npos || at != std::string::npos))
        {
            r.error = prefix + "empty host";
            return r;
        }
        if (!host_text.empty())
        {
            const auto [parsed, host_error] =
                special ? parse_host(host_text) : parse_opaque_host(host_text);
            if (!parsed)
            {
                r.error = prefix + host_error;
                return r;
            }
            host = *parsed;
        }
        if (split != std::string::npos)
        {
            port = authority.substr(split + 1);
            if (!std::all_of(port.begin(), port.end(), is_digit))
            {
                r.error = prefix + "invalid port number";
                return r;
            }
            if (!port.empty())
            {
                std::size_t z = 0;
                while (z + 1 < port.size() && port[z] == '0')
                    ++z;
                const std::string digits = port.substr(z);
                if (digits.size() > 5 || std::stol(digits) > 65535)
                {
                    r.error = prefix + "invalid port number";
                    return r;
                }
                const long n = std::stol(digits);
                port = n == default_port ? std::string() : std::to_string(n);
            }
        }
    }

    std::optional<std::string> query, fragment;
    if (const auto hash = rest.find('#'); hash != std::string::npos)
    {
        fragment = percent_encode(rest.substr(hash + 1), kFragmentSet);
        rest = rest.substr(0, hash);
    }
    if (const auto q = rest.find('?'); q != std::string::npos)
    {
        query = percent_encode(rest.substr(q + 1), special ? kQuerySet : kSpecialFreeQuerySet);
        rest = rest.substr(0, q);
    }
    std::string path;
    if (special)
    {
        std::replace(rest.begin(), rest.end(), '\\', '/');
        path = rest.empty() ? "/" : percent_encode(resolve_dot_segments(rest), kPathSet);
    }
    else if (has_authority || (!rest.empty() && rest[0] == '/'))
        path = rest.empty() ? std::string() : percent_encode(resolve_dot_segments(rest), kPathSet);
    else
        path = percent_encode(rest, "");

    if (!scheme_ok())
        return r;
    if (special && preserve_empty_path && path == "/" && input_path_is_empty(input))
        path.clear();
    std::string out = scheme + ":";
    if (has_authority)
        out += "//" + userinfo + host + (port.empty() ? "" : ":" + port);
    out += path;
    if (query)
        out += "?" + *query;
    if (fragment)
        out += "#" + *fragment;
    r.url = out;
    return r;
}

// ================================================================== ipaddress

IPAddress IPAddress::v4(std::uint32_t value)
{
    IPAddress a;
    a.version = 4;
    a.bytes[0] = static_cast<std::uint8_t>(value >> 24);
    a.bytes[1] = static_cast<std::uint8_t>(value >> 16);
    a.bytes[2] = static_cast<std::uint8_t>(value >> 8);
    a.bytes[3] = static_cast<std::uint8_t>(value);
    return a;
}

std::uint32_t IPAddress::v4_value() const
{
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
           (std::uint32_t(bytes[2]) << 8) | bytes[3];
}

std::optional<IPAddress> IPAddress::ipv4_mapped() const
{
    if (version != 6)
        return std::nullopt;
    for (int i = 0; i < 10; ++i)
        if (bytes[i] != 0)
            return std::nullopt;
    if (bytes[10] != 0xFF || bytes[11] != 0xFF)
        return std::nullopt;
    IPAddress a;
    a.version = 4;
    std::copy(bytes.begin() + 12, bytes.end(), a.bytes.begin());
    return a;
}

std::optional<IPAddress> IPAddress::sixtofour() const
{
    if (version != 6 || bytes[0] != 0x20 || bytes[1] != 0x02)
        return std::nullopt;
    IPAddress a;
    a.version = 4;
    std::copy(bytes.begin() + 2, bytes.begin() + 6, a.bytes.begin());
    return a;
}

std::optional<std::pair<IPAddress, IPAddress>> IPAddress::teredo() const
{
    if (version != 6 || bytes[0] != 0x20 || bytes[1] != 0x01 || bytes[2] != 0 || bytes[3] != 0)
        return std::nullopt;
    IPAddress server, client;
    server.version = client.version = 4;
    std::copy(bytes.begin() + 4, bytes.begin() + 8, server.bytes.begin());
    for (int i = 0; i < 4; ++i)
        client.bytes[i] = static_cast<std::uint8_t>(~bytes[12 + i]);
    return std::make_pair(server, client);
}

bool IPAddress::is_private() const
{
    if (version == 4)
    {
        bool in = false;
        for (const auto& n : v4_private())
            in = in || in_net(*this, n);
        for (const auto& n : v4_private_exceptions())
            in = in && !in_net(*this, n);
        return in;
    }
    if (auto m = ipv4_mapped())
        return m->is_private();
    bool in = false;
    for (const auto& n : v6_private())
        in = in || in_net(*this, n);
    for (const auto& n : v6_private_exceptions())
        in = in && !in_net(*this, n);
    return in;
}

bool IPAddress::is_global() const
{
    if (version == 4)
        return !in_net(*this, net("100.64.0.0", 10)) && !is_private();
    if (auto m = ipv4_mapped())
        return m->is_global();
    return !is_private();
}

bool IPAddress::is_loopback() const
{
    if (version == 4)
        return bytes[0] == 127;
    if (auto m = ipv4_mapped())
        return m->is_loopback();
    for (int i = 0; i < 15; ++i)
        if (bytes[i])
            return false;
    return bytes[15] == 1;
}

bool IPAddress::is_multicast() const
{
    if (version == 4)
        return (bytes[0] & 0xF0) == 0xE0;
    if (auto m = ipv4_mapped())
        return m->is_multicast();
    return bytes[0] == 0xFF;
}

bool IPAddress::is_unspecified() const
{
    const int n = version == 4 ? 4 : 16;
    for (int i = 0; i < n; ++i)
        if (bytes[i])
            return false;
    return true;
}

bool IPAddress::is_link_local() const
{
    if (version == 4)
        return bytes[0] == 169 && bytes[1] == 254;
    if (auto m = ipv4_mapped())
        return m->is_link_local();
    return bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80;
}

std::string IPAddress::str() const
{
    if (version == 4)
        return serialize_ipv4(v4_value());
    std::string out;
    if (auto m = ipv4_mapped())
        out = "::ffff:" + m->str();
    else
    {
        std::array<std::uint16_t, 8> h{};
        for (int i = 0; i < 8; ++i)
            h[i] = static_cast<std::uint16_t>((bytes[2 * i] << 8) | bytes[2 * i + 1]);
        out = serialize_ipv6(h);
    }
    if (!scope_id.empty())
        out += "%" + scope_id;
    return out;
}

std::optional<IPAddress> ip_address(const std::string& text)
{
    if (text.find('/') != std::string::npos)
        return std::nullopt;
    if (const auto v4 = parse_v4(text))
        return IPAddress::v4(*v4);
    std::string addr = text, scope;
    if (const auto pct = text.find('%'); pct != std::string::npos)
    {
        addr = text.substr(0, pct);
        scope = text.substr(pct + 1);
        if (scope.empty() || scope.find('%') != std::string::npos)
            return std::nullopt;
    }
    const auto v6 = parse_v6(addr);
    if (!v6)
        return std::nullopt;
    IPAddress a;
    a.version = 6;
    for (int i = 0; i < 8; ++i)
    {
        a.bytes[2 * i] = static_cast<std::uint8_t>((*v6)[i] >> 8);
        a.bytes[2 * i + 1] = static_cast<std::uint8_t>((*v6)[i]);
    }
    a.scope_id = scope;
    return a;
}

} // namespace fastmcpp::internal::pyurl
