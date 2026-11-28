#include "fastmcpp/server/oauth_server.hpp"

#include "../internal/pyjson.hpp"
#include "../internal/pyurl.hpp"
#include "auth_detail.hpp"
#include "fastmcpp/server/cimd.hpp"
#include "fastmcpp/server/redirect_validation.hpp"
#include "fastmcpp/util/crypto.hpp"
#include "fastmcpp/util/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>

namespace fastmcpp::server::auth
{

namespace
{

using OJ = nlohmann::ordered_json;
using namespace detail;

constexpr std::size_t kMaxRequestBodySize = 4 * 1024 * 1024;

// ------------------------------------------------------------------ text

std::string hex_lower(const std::uint8_t* data, std::size_t len)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i)
    {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

std::string token_hex(std::size_t nbytes)
{
    const auto b = util::crypto::random_bytes(nbytes);
    return hex_lower(b.data(), b.size());
}

std::string uuid4()
{
    auto b = util::crypto::random_bytes(16);
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40);
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
    const std::string h = hex_lower(b.data(), b.size());
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) +
           "-" + h.substr(20);
}

std::string join(const std::vector<std::string>& items, const std::string& sep)
{
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? sep : "") + items[i];
    return out;
}

/// str.split(" "): empty fields kept.
std::vector<std::string> split_space(const std::string& s)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true)
    {
        const auto sp = s.find(' ', start);
        out.push_back(s.substr(start, sp == std::string::npos ? std::string::npos : sp - start));
        if (sp == std::string::npos)
            break;
        start = sp + 1;
    }
    return out;
}

/// repr() of a str.
std::string py_repr(const std::string& s)
{
    const bool dq = s.find('\'') != std::string::npos && s.find('"') == std::string::npos;
    const char q = dq ? '"' : '\'';
    std::string out(1, q);
    for (char c : s)
        if (c == '\\')
            out += "\\\\";
        else if (c == q)
            out += std::string("\\") + q;
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else if (c == '\t')
            out += "\\t";
        else
            out.push_back(c);
    out.push_back(q);
    return out;
}

std::string py_repr_list(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i)
        out += (i ? ", " : "") + py_repr(items[i]);
    return out + "]";
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

/// urllib.parse.unquote (or unquote_plus with @p plus).
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

/// urllib.parse.quote_plus with no extra safe characters.
std::string quote_plus(const std::string& s)
{
    static const char* digits = "0123456789ABCDEF";
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
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0xF]);
        }
    }
    return out;
}

/// The Location quoting of a redirect response.
std::string rstrip_slash(std::string s)
{
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

/// The response header named @p name, compared without case.
std::unordered_map<std::string, std::string>::iterator
find_header(std::unordered_map<std::string, std::string>& headers, const std::string& name)
{
    for (auto it = headers.begin(); it != headers.end(); ++it)
        if (lower(it->first) == name)
            return it;
    return headers.end();
}

std::string quote_location(const std::string& s)
{
    static const std::string safe = ":/%#?=@[]!$&'()*+,;";
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
            safe.find(static_cast<char>(c)) != std::string::npos)
            out.push_back(static_cast<char>(c));
        else
        {
            out.push_back('%');
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0xF]);
        }
    }
    return out;
}

bool is_valid_utf8(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len;
        unsigned cp;
        if (c < 0x80)
        {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0)
            len = 2, cp = c & 0x1F;
        else if ((c & 0xF0) == 0xE0)
            len = 3, cp = c & 0x0F;
        else if ((c & 0xF8) == 0xF0)
            len = 4, cp = c & 0x07;
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
        i += len;
    }
    return true;
}

std::optional<std::string> header_value(const CustomRouteRequest& req, const std::string& name)
{
    const std::string want = lower(name);
    for (const auto& [k, v] : req.headers)
        if (lower(k) == want)
            return v;
    return std::nullopt;
}

double now() noexcept
{
    return now_seconds();
}

// ------------------------------------------------------------------ JSON (strict reader)

/// Parses JSON the way the reference server's validator does and reports
/// syntax errors with the same wording and position.
class StrictJsonReader
{
  public:
    explicit StrictJsonReader(const std::string& s) : s_(s) {}

    OJ parse()
    {
        skip_ws();
        OJ v = value();
        skip_ws();
        if (i_ < s_.size())
            fail("trailing characters", i_ + 1);
        return v;
    }

    struct Error
    {
        std::string message;
    };

  private:
    [[noreturn]] void fail(const std::string& what, std::size_t pos) const
    {
        std::size_t line = 1, last_nl = std::string::npos;
        for (std::size_t k = 0; k < pos && k < s_.size(); ++k)
            if (s_[k] == '\n')
                ++line, last_nl = k;
        const std::size_t col = last_nl == std::string::npos ? pos : pos - last_nl - 1;
        throw Error{"Invalid JSON: " + what + " at line " + std::to_string(line) + " column " +
                    std::to_string(col)};
    }
    [[noreturn]] void eof(const char* what) const
    {
        fail(std::string("EOF while parsing ") + what, s_.size());
    }

    void skip_ws()
    {
        while (i_ < s_.size() &&
               (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    void ident(const char* word)
    {
        const std::size_t n = std::char_traits<char>::length(word);
        for (std::size_t k = 1; k < n; ++k)
        {
            if (i_ + k >= s_.size())
                eof("a value");
            if (s_[i_ + k] != word[k])
                fail("expected ident", i_ + k + 1);
        }
        i_ += n;
    }

    OJ value()
    {
        if (i_ >= s_.size())
            eof("a value");
        const char c = s_[i_];
        switch (c)
        {
        case '{':
            return object();
        case '[':
            return list();
        case '"':
            return string();
        case 't':
            ident("true");
            return true;
        case 'f':
            ident("false");
            return false;
        case 'n':
            ident("null");
            return nullptr;
        case 'N':
            ident("NaN");
            return std::numeric_limits<double>::quiet_NaN();
        case 'I':
            ident("Infinity");
            return std::numeric_limits<double>::infinity();
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
                return number();
            fail("expected value", i_ + 1);
        }
    }

    OJ number()
    {
        const std::size_t start = i_;
        const auto digit = [this](std::size_t k)
        { return k < s_.size() && s_[k] >= '0' && s_[k] <= '9'; };
        bool is_float = false;
        if (s_[i_] == '-')
        {
            ++i_;
            if (i_ >= s_.size())
                eof("a value");
            if (s_[i_] == 'I')
            {
                ident("Infinity");
                return -std::numeric_limits<double>::infinity();
            }
            if (!digit(i_))
                fail("invalid number", i_ + 1);
        }
        if (s_[i_] == '0')
        {
            ++i_;
            if (digit(i_))
                fail("invalid number", i_ + 1);
        }
        else
            while (digit(i_))
                ++i_;
        if (i_ < s_.size() && s_[i_] == '.')
        {
            is_float = true;
            ++i_;
            if (i_ >= s_.size())
                eof("a value");
            if (!digit(i_))
                fail("invalid number", i_ + 1);
            while (digit(i_))
                ++i_;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E'))
        {
            is_float = true;
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-'))
                ++i_;
            if (i_ >= s_.size())
                eof("a value");
            if (!digit(i_))
                fail("invalid number", i_ + 1);
            while (digit(i_))
                ++i_;
        }
        const std::string text = s_.substr(start, i_ - start);
        if (is_float)
            return std::strtod(text.c_str(), nullptr);
        return OJ::parse(text);
    }

    unsigned hex4()
    {
        unsigned v = 0;
        for (int k = 0; k < 4; ++k)
        {
            if (i_ >= s_.size())
                eof("a string");
            const int h = hex_value(s_[i_]);
            if (h < 0)
                fail("invalid escape", i_ + 1);
            v = v * 16 + static_cast<unsigned>(h);
            ++i_;
        }
        return v;
    }

    static void put_utf8(std::string& out, unsigned cp)
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

    std::string raw_string()
    {
        ++i_; // opening quote
        std::string out;
        while (true)
        {
            if (i_ >= s_.size())
                eof("a string");
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (c == '"')
            {
                ++i_;
                return out;
            }
            if (c < 0x20)
                fail("control character (\\u0000-\\u001F) found while parsing a string", i_ + 1);
            if (c != '\\')
            {
                out.push_back(static_cast<char>(c));
                ++i_;
                continue;
            }
            ++i_;
            if (i_ >= s_.size())
                eof("a string");
            const char e = s_[i_];
            switch (e)
            {
            case '"':
            case '\\':
            case '/':
                out.push_back(e);
                ++i_;
                break;
            case 'b':
                out.push_back('\b');
                ++i_;
                break;
            case 'f':
                out.push_back('\f');
                ++i_;
                break;
            case 'n':
                out.push_back('\n');
                ++i_;
                break;
            case 'r':
                out.push_back('\r');
                ++i_;
                break;
            case 't':
                out.push_back('\t');
                ++i_;
                break;
            case 'u':
            {
                ++i_;
                unsigned cp = hex4();
                if (cp >= 0xDC00 && cp <= 0xDFFF)
                    fail("lone leading surrogate in hex escape", i_);
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    if (i_ + 1 >= s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u')
                    {
                        if (i_ >= s_.size())
                            eof("a string");
                        fail("unexpected end of hex escape", i_ + 1);
                    }
                    i_ += 2;
                    const unsigned lo = hex4();
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        fail("lone leading surrogate in hex escape", i_);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                put_utf8(out, cp);
                break;
            }
            default:
                fail("invalid escape", i_ + 1);
            }
        }
    }

    OJ string()
    {
        return raw_string();
    }

    OJ object()
    {
        ++i_;
        OJ out = OJ::object();
        skip_ws();
        if (i_ >= s_.size())
            eof("an object");
        if (s_[i_] == '}')
        {
            ++i_;
            return out;
        }
        while (true)
        {
            if (s_[i_] != '"')
                fail("key must be a string", i_ + 1);
            std::string key = raw_string();
            skip_ws();
            if (i_ >= s_.size())
                eof("an object");
            if (s_[i_] != ':')
                fail("expected `:`", i_ + 1);
            ++i_;
            skip_ws();
            out[key] = value();
            skip_ws();
            if (i_ >= s_.size())
                eof("an object");
            if (s_[i_] == '}')
            {
                ++i_;
                return out;
            }
            if (s_[i_] != ',')
                fail("expected `,` or `}`", i_ + 1);
            ++i_;
            skip_ws();
            if (i_ >= s_.size())
                eof("an object");
            if (s_[i_] == '}')
                fail("trailing comma", i_ + 1);
        }
    }

    OJ list()
    {
        ++i_;
        OJ out = OJ::array();
        skip_ws();
        if (i_ >= s_.size())
            eof("a list");
        if (s_[i_] == ']')
        {
            ++i_;
            return out;
        }
        while (true)
        {
            out.push_back(value());
            skip_ws();
            if (i_ >= s_.size())
                eof("a list");
            if (s_[i_] == ']')
            {
                ++i_;
                return out;
            }
            if (s_[i_] != ',')
                fail("expected `,` or `]`", i_ + 1);
            ++i_;
            skip_ws();
            if (i_ >= s_.size())
                eof("a list");
            if (s_[i_] == ']')
                fail("trailing comma", i_ + 1);
        }
    }

    const std::string& s_;
    std::size_t i_ = 0;
};

// ------------------------------------------------------------------ URLs

using UrlResult = internal::pyurl::UrlResult;

/// Validate and normalize a URL the way the reference validator's URL types
/// do. @p http_only rejects schemes other than http and https.
UrlResult validate_url(const Json& input, bool preserve_empty_path, bool http_only)
{
    if (!input.is_string())
    {
        UrlResult r;
        r.error = "URL input should be a string or URL";
        return r;
    }
    return internal::pyurl::validate_url(input.get<std::string>(), preserve_empty_path, http_only);
}

std::string any_http_url(const std::string& url)
{
    const auto r = validate_url(Json(url), false, true);
    if (!r.url)
        throw std::invalid_argument(r.error + ": '" + url + "'");
    return *r.url;
}

bool is_loopback_issuer_host(const std::string& url)
{
    const auto sep = url.find("://");
    std::string rest = url.substr(sep + 3);
    const auto end = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, end);
    if (const auto at = authority.rfind('@'); at != std::string::npos)
        authority = authority.substr(at + 1);
    std::string host = authority;
    if (!host.empty() && host[0] == '[')
        host = host.substr(0, host.find(']') + 1);
    else if (const auto colon = host.rfind(':'); colon != std::string::npos)
        host = host.substr(0, colon);
    return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

// ------------------------------------------------------------------ validation errors

/// Field errors in reference order, rendered "loc: msg" one per line.
struct FieldErrors
{
    std::vector<std::string> lines;
    void add(const std::string& loc, const std::string& msg)
    {
        lines.push_back(loc + ": " + msg);
    }
    bool empty() const
    {
        return lines.empty();
    }
    std::string str() const
    {
        return join(lines, "\n");
    }
};

// ------------------------------------------------------------------ responses

CustomRouteResponse json_response(int status, const OJ& body,
                                  std::unordered_map<std::string, std::string> headers = {})
{
    CustomRouteResponse r;
    r.status = status;
    r.body = body.dump();
    r.content_type = "application/json";
    r.headers = std::move(headers);
    return r;
}

const std::unordered_map<std::string, std::string> kNoStore{{"Cache-Control", "no-store"},
                                                            {"Pragma", "no-cache"}};

OJ error_body(const std::string& error, const std::optional<std::string>& description)
{
    OJ j;
    j["error"] = error;
    if (description)
        j["error_description"] = *description;
    return j;
}

CustomRouteResponse token_error(const std::string& error,
                                const std::optional<std::string>& description, int status = 400)
{
    return json_response(status, error_body(error, description), kNoStore);
}

OJ token_json(const OAuthToken& t)
{
    OJ j;
    j["access_token"] = t.access_token;
    std::string type = t.token_type;
    if (!type.empty())
    {
        type = lower(type);
        type[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(type[0])));
    }
    j["token_type"] = type;
    if (t.expires_in)
        j["expires_in"] = *t.expires_in;
    if (t.scope)
        j["scope"] = *t.scope;
    if (t.refresh_token)
        j["refresh_token"] = *t.refresh_token;
    return j;
}

CustomRouteResponse redirect_response(const std::string& url)
{
    CustomRouteResponse r;
    r.status = 302;
    r.content_type = "";
    r.headers["location"] = quote_location(url);
    r.headers["Cache-Control"] = "no-store";
    return r;
}

std::function<CustomRouteResponse(const CustomRouteRequest&)>
body_limited(std::function<CustomRouteResponse(const CustomRouteRequest&)> handler)
{
    return [handler = std::move(handler)](const CustomRouteRequest& req)
    {
        std::size_t declared = 0;
        if (const auto cl = header_value(req, "content-length"))
            try
            {
                declared = static_cast<std::size_t>(std::stoull(*cl));
            }
            catch (...)
            {
            }
        if (req.body.size() > kMaxRequestBodySize || declared > kMaxRequestBodySize)
        {
            CustomRouteResponse r;
            r.status = 413;
            r.body = "Request body too large";
            r.content_type = "text/plain; charset=utf-8";
            return r;
        }
        return handler(req);
    };
}

// ------------------------------------------------------------------ client metadata

const char* kAuthMethodLiteral =
    "Input should be 'none', 'client_secret_post', 'client_secret_basic' or 'private_key_jwt'";

void optional_string(const OJ& obj, const char* key, std::optional<std::string>& out,
                     FieldErrors& errs)
{
    if (!obj.contains(key) || obj[key].is_null())
        return;
    if (!obj[key].is_string())
        errs.add(key, "Input should be a valid string");
    else
        out = obj[key].get<std::string>();
}

void optional_http_url(const OJ& obj, const char* key, std::optional<std::string>& out,
                       FieldErrors& errs)
{
    if (!obj.contains(key) || obj[key].is_null() ||
        (obj[key].is_string() && obj[key].get<std::string>().empty()))
        return;
    const auto r = validate_url(obj[key], true, true);
    if (r.url)
        out = *r.url;
    else
        errs.add(key, r.error);
}

std::optional<std::vector<std::string>> string_list(const OJ& value, const std::string& loc,
                                                    FieldErrors& errs)
{
    if (!value.is_array())
    {
        errs.add(loc, "Input should be a valid array");
        return std::nullopt;
    }
    std::vector<std::string> out;
    bool ok = true;
    for (std::size_t i = 0; i < value.size(); ++i)
        if (!value[i].is_string())
        {
            errs.add(loc + "." + std::to_string(i), "Input should be a valid string");
            ok = false;
        }
        else
            out.push_back(value[i].get<std::string>());
    if (!ok)
        return std::nullopt;
    return out;
}

/// The RFC 7591 registration request in @p body, or the validation errors.
std::variant<OAuthClientInformation, std::string> parse_client_metadata(const std::string& body)
{
    OJ obj;
    try
    {
        obj = StrictJsonReader(body).parse();
    }
    catch (const StrictJsonReader::Error& e)
    {
        return ": " + e.message;
    }
    if (!obj.is_object())
        return std::string(": Input should be an object");

    FieldErrors errs;
    OAuthClientInformation m;
    if (obj.contains("response_types"))
        if (auto v = string_list(obj["response_types"], "response_types", errs))
            m.response_types = *v;
    optional_string(obj, "scope", m.scope, errs);
    optional_string(obj, "client_name", m.client_name, errs);
    optional_http_url(obj, "client_uri", m.client_uri, errs);
    optional_http_url(obj, "logo_uri", m.logo_uri, errs);
    if (obj.contains("contacts") && !obj["contacts"].is_null())
        m.contacts = string_list(obj["contacts"], "contacts", errs);
    optional_http_url(obj, "tos_uri", m.tos_uri, errs);
    optional_http_url(obj, "policy_uri", m.policy_uri, errs);
    optional_http_url(obj, "jwks_uri", m.jwks_uri, errs);
    if (obj.contains("jwks"))
        m.jwks = Json::parse(obj["jwks"].dump());
    optional_string(obj, "software_id", m.software_id, errs);
    optional_string(obj, "software_version", m.software_version, errs);

    if (!obj.contains("redirect_uris"))
        errs.add("redirect_uris", "Field required");
    else if (!obj["redirect_uris"].is_null())
    {
        const auto& v = obj["redirect_uris"];
        if (!v.is_array())
            errs.add("redirect_uris", "Input should be a valid array");
        else
        {
            std::vector<std::string> uris;
            bool ok = true;
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                const auto r = validate_url(v[i], true, false);
                if (!r.url)
                {
                    errs.add("redirect_uris." + std::to_string(i), r.error);
                    ok = false;
                }
                else
                    uris.push_back(*r.url);
            }
            if (ok && uris.empty())
                errs.add("redirect_uris",
                         "List should have at least 1 item after validation, not 0");
            else if (ok)
                m.redirect_uris = uris;
        }
    }
    if (obj.contains("token_endpoint_auth_method") && !obj["token_endpoint_auth_method"].is_null())
    {
        const auto& v = obj["token_endpoint_auth_method"];
        static const std::set<std::string> kMethods{"none", "client_secret_post",
                                                    "client_secret_basic", "private_key_jwt"};
        if (!v.is_string() || !kMethods.count(v.get<std::string>()))
            errs.add("token_endpoint_auth_method", kAuthMethodLiteral);
        else
            m.token_endpoint_auth_method = v.get<std::string>();
    }
    if (obj.contains("grant_types"))
    {
        const auto& v = obj["grant_types"];
        if (!v.is_array())
            errs.add("grant_types", "Input should be a valid array");
        else
        {
            std::vector<std::string> grants;
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                if (v[i].is_string())
                {
                    grants.push_back(v[i].get<std::string>());
                    continue;
                }
                const std::string loc = "grant_types." + std::to_string(i);
                errs.add(loc + ".literal['authorization_code','refresh_token','" +
                             JWT_BEARER_GRANT_TYPE + "']",
                         std::string("Input should be 'authorization_code', 'refresh_token' or '") +
                             JWT_BEARER_GRANT_TYPE + "'");
                errs.add(loc + ".str", "Input should be a valid string");
            }
            m.grant_types = grants;
        }
    }
    m.application_type = "native";
    if (obj.contains("application_type"))
    {
        const auto& v = obj["application_type"];
        if (!v.is_string() || (v.get<std::string>() != "web" && v.get<std::string>() != "native"))
            errs.add("application_type", "Input should be 'web' or 'native'");
        else
            m.application_type = v.get<std::string>();
    }
    if (!errs.empty())
        return errs.str();
    return m;
}

// ------------------------------------------------------------------ CORS

CustomRouteResponse cors_preflight(const CustomRouteRequest& req,
                                   const std::vector<std::string>& methods)
{
    static const std::vector<std::string> kAllowHeaders{
        "Accept", "Accept-Language", "Content-Language", "Content-Type", "mcp-protocol-version"};
    CustomRouteResponse res;
    res.content_type = "text/plain; charset=utf-8";
    res.headers["vary"] = "Origin, Access-Control-Request-Method, Access-Control-Request-Headers, "
                          "Access-Control-Request-Private-Network";
    res.headers["access-control-allow-origin"] = "*";
    res.headers["access-control-allow-methods"] = join(methods, ", ");
    res.headers["access-control-max-age"] = "600";
    res.headers["access-control-allow-headers"] = join(kAllowHeaders, ", ");
    std::vector<std::string> failures;
    const auto method = header_value(req, "access-control-request-method").value_or("");
    if (std::find(methods.begin(), methods.end(), method) == methods.end())
        failures.push_back("method");
    if (const auto requested = header_value(req, "access-control-request-headers"))
    {
        std::size_t start = 0;
        while (true)
        {
            const auto comma = requested->find(',', start);
            const std::string h = strip(lower(requested->substr(
                start, comma == std::string::npos ? std::string::npos : comma - start)));
            bool allowed = false;
            for (const auto& a : kAllowHeaders)
                if (lower(a) == h)
                    allowed = true;
            if (!allowed)
            {
                failures.push_back("headers");
                break;
            }
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
    }
    if (header_value(req, "access-control-request-private-network"))
        failures.push_back("private-network");
    res.status = failures.empty() ? 200 : 400;
    res.body = failures.empty() ? "OK" : "Disallowed CORS " + join(failures, ", ");
    return res;
}

} // namespace

std::function<CustomRouteResponse(const CustomRouteRequest&)>
cors_handler(std::function<CustomRouteResponse(const CustomRouteRequest&)> handler,
             std::vector<std::string> methods)
{
    return
        [handler = std::move(handler), methods = std::move(methods)](const CustomRouteRequest& req)
    {
        const auto origin = header_value(req, "origin");
        if (origin && req.method == "OPTIONS" && header_value(req, "access-control-request-method"))
            return cors_preflight(req, methods);
        CustomRouteResponse res = handler(req);
        if (origin)
            res.headers["access-control-allow-origin"] = "*";
        std::string vary;
        for (auto it = res.headers.begin(); it != res.headers.end();)
            if (lower(it->first) == "vary")
            {
                vary = it->second;
                it = res.headers.erase(it);
            }
            else
                ++it;
        res.headers["vary"] = vary.empty() ? "Origin" : vary + ", Origin";
        return res;
    };
}

// ================================================================== errors

AuthorizeError::AuthorizeError(std::string error, std::optional<std::string> description)
    : std::runtime_error(description ? *description : error), error_(std::move(error)),
      description_(std::move(description))
{
}

TokenError::TokenError(std::string error, std::optional<std::string> description)
    : std::runtime_error(description ? *description : error), error_(std::move(error)),
      description_(std::move(description))
{
}

RegistrationError::RegistrationError(std::string error, std::optional<std::string> description)
    : std::runtime_error(description ? *description : error), error_(std::move(error)),
      description_(std::move(description))
{
}

// ================================================================== client information

OJ OAuthClientInformation::to_json() const
{
    OJ j;
    j["response_types"] = response_types;
    if (scope)
        j["scope"] = *scope;
    if (client_name)
        j["client_name"] = *client_name;
    if (client_uri)
        j["client_uri"] = *client_uri;
    if (logo_uri)
        j["logo_uri"] = *logo_uri;
    if (contacts)
        j["contacts"] = *contacts;
    if (tos_uri)
        j["tos_uri"] = *tos_uri;
    if (policy_uri)
        j["policy_uri"] = *policy_uri;
    if (jwks_uri)
        j["jwks_uri"] = *jwks_uri;
    if (!jwks.is_null())
        j["jwks"] = OJ::parse(jwks.dump());
    if (software_id)
        j["software_id"] = *software_id;
    if (software_version)
        j["software_version"] = *software_version;
    if (redirect_uris)
        j["redirect_uris"] = *redirect_uris;
    if (token_endpoint_auth_method)
        j["token_endpoint_auth_method"] = *token_endpoint_auth_method;
    j["grant_types"] = grant_types;
    if (application_type)
        j["application_type"] = *application_type;
    j["client_id"] = client_id;
    if (client_secret)
        j["client_secret"] = *client_secret;
    if (client_id_issued_at)
        j["client_id_issued_at"] = *client_id_issued_at;
    if (client_secret_expires_at)
        j["client_secret_expires_at"] = *client_secret_expires_at;
    if (issuer)
        j["issuer"] = *issuer;
    return j;
}

OAuthClientInformation OAuthClientInformation::from_json(const Json& in)
{
    if (!in.is_object())
        throw std::invalid_argument("client information must be an object");
    Json j = Json::object();
    for (auto it = in.begin(); it != in.end(); ++it)
        if (!it->is_null() && !(it->is_string() && it->get<std::string>().empty()))
            j[it.key()] = *it;
    if (!j.contains("client_id") || !j["client_id"].is_string())
        throw std::invalid_argument("client_id: Field required");
    OAuthClientInformation c;
    const auto str = [&j](const char* k) -> std::optional<std::string>
    {
        if (j.contains(k) && j[k].is_string())
            return j[k].get<std::string>();
        return std::nullopt;
    };
    const auto list = [&j](const char* k) -> std::optional<std::vector<std::string>>
    {
        if (!j.contains(k) || !j[k].is_array())
            return std::nullopt;
        std::vector<std::string> out;
        for (const auto& v : j[k])
            if (v.is_string())
                out.push_back(v.get<std::string>());
        return out;
    };
    const auto integer = [&j](const char* k) -> std::optional<std::int64_t>
    {
        if (j.contains(k) && j[k].is_number())
            return j[k].get<std::int64_t>();
        return std::nullopt;
    };
    if (auto v = list("response_types"))
        c.response_types = *v;
    c.scope = str("scope");
    c.client_name = str("client_name");
    c.client_uri = str("client_uri");
    c.logo_uri = str("logo_uri");
    c.contacts = list("contacts");
    c.tos_uri = str("tos_uri");
    c.policy_uri = str("policy_uri");
    c.jwks_uri = str("jwks_uri");
    if (j.contains("jwks"))
        c.jwks = j["jwks"];
    c.software_id = str("software_id");
    c.software_version = str("software_version");
    c.redirect_uris = list("redirect_uris");
    c.token_endpoint_auth_method = str("token_endpoint_auth_method");
    if (auto v = list("grant_types"))
        c.grant_types = *v;
    c.application_type = str("application_type");
    c.client_id = j["client_id"].get<std::string>();
    c.client_secret = str("client_secret");
    c.client_id_issued_at = integer("client_id_issued_at");
    c.client_secret_expires_at = integer("client_secret_expires_at");
    c.issuer = str("issuer");
    return c;
}

std::optional<std::vector<std::string>>
OAuthClientInformation::validate_scope(const std::optional<std::string>& requested_scope) const
{
    if (!requested_scope)
        return std::nullopt;
    const auto requested = split_space(*requested_scope);
    const auto allowed = scope ? split_space(*scope) : std::vector<std::string>{};
    for (const auto& s : requested)
        if (std::find(allowed.begin(), allowed.end(), s) == allowed.end())
            throw InvalidScopeError("Client was not registered with scope " + s);
    return requested;
}

namespace
{

std::string redirect_uri_path(const std::string& path)
{
    return path.empty() ? "/" : path;
}

/// Whether @p requested differs from the registered loopback URI only in
/// the port (RFC 8252 §7.3).
bool matches_registered_loopback_redirect_uri(const std::string& requested_uri,
                                              const std::string& registered_uri)
{
    namespace pu = internal::pyurl;
    pu::ParseResult requested, registered;
    try
    {
        requested = pu::urlparse(requested_uri);
        registered = pu::urlparse(registered_uri);
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    if (pu::username(requested) || pu::password(requested) || pu::username(registered) ||
        pu::password(registered))
        return false;
    const auto requested_host = pu::hostname(requested);
    const auto registered_host = pu::hostname(registered);
    if (!is_loopback_host(registered_host) || requested_host != registered_host)
        return false;
    return lower(requested.scheme) == lower(registered.scheme) &&
           redirect_uri_path(requested.path) == redirect_uri_path(registered.path) &&
           requested.params == registered.params && requested.query == registered.query &&
           requested.fragment == registered.fragment;
}

bool matches_registered_redirect_uri(const std::string& redirect_uri,
                                     const std::optional<std::vector<std::string>>& registered)
{
    if (!registered || registered->empty())
        return false;
    for (const auto& uri : *registered)
        if (redirect_uri == uri || matches_registered_loopback_redirect_uri(redirect_uri, uri))
            return true;
    return false;
}

void enforce_application_type(const OAuthClientInformation& client, const std::string& uri)
{
    if (!is_redirect_uri_allowed_for_application_type(uri, client.application_type))
        throw InvalidRedirectUriError(
            "Redirect URI '" + uri + "' is not allowed for application_type '" +
            (client.application_type ? *client.application_type : std::string("None")) + "'.");
}

std::string proxy_validate_redirect_uri(const OAuthClientInformation& c,
                                        const std::optional<std::string>& redirect_uri)
{
    const auto not_allowed = [](const std::string& uri)
    {
        return InvalidRedirectUriError("Redirect URI '" + uri +
                                       "' does not match allowed patterns.");
    };
    if (!redirect_uri && c.cimd_document)
    {
        const auto& uris = c.cimd_document->redirect_uris;
        if (uris.size() != 1)
            throw InvalidRedirectUriError(
                "redirect_uri must be specified when CIMD lists multiple redirect_uris.");
        const std::string& candidate = uris.front();
        if (candidate.find('*') != std::string::npos)
            throw InvalidRedirectUriError(
                "redirect_uri must be specified when CIMD redirect_uris uses wildcards.");
        const auto r = internal::pyurl::validate_url(candidate, false, false);
        if (!r.url)
            throw InvalidRedirectUriError(
                "Invalid CIMD redirect_uri: " +
                internal::pyjson::validation_error_str(
                    "function-wrap[wrap_val()]",
                    {{"", r.error.rfind("URL scheme", 0) == 0 ? "url_scheme" : "url_parsing",
                      r.error, internal::pyjson::OrderedJson(candidate)}}));
        if (!validate_redirect_uri(*r.url, c.allowed_redirect_uri_patterns))
            throw not_allowed(*r.url);
        enforce_application_type(c, *r.url);
        return *r.url;
    }
    if (redirect_uri)
    {
        const std::string& uri = *redirect_uri;
        if (!validate_redirect_uri(uri, std::nullopt))
            throw InvalidRedirectUriError("Redirect URI '" + uri + "' uses an unsafe scheme.");
        enforce_application_type(c, uri);
        if (c.cimd_document && !c.cimd_document->redirect_uris.empty())
        {
            bool match = false;
            for (const auto& pattern : c.cimd_document->redirect_uris)
                if (matches_allowed_pattern(uri, pattern))
                {
                    match = true;
                    break;
                }
            if (!match)
                throw InvalidRedirectUriError("Redirect URI '" + uri +
                                              "' does not match CIMD redirect_uris.");
            if (c.allowed_redirect_uri_patterns &&
                !validate_redirect_uri(uri, c.allowed_redirect_uri_patterns))
                throw not_allowed(uri);
            return uri;
        }
        if (!c.allowed_redirect_uri_patterns)
        {
            if (c.allow_unregistered_redirect_uris ||
                matches_registered_redirect_uri(uri, c.redirect_uris))
                return uri;
            throw InvalidRedirectUriError("Redirect URI '" + uri + "' not registered for client");
        }
        if (validate_redirect_uri(uri, c.allowed_redirect_uri_patterns))
            return uri;
        throw not_allowed(uri);
    }
    OAuthClientInformation base = c;
    base.proxy_client = false;
    const std::string resolved = base.validate_redirect_uri(std::nullopt);
    if (!validate_redirect_uri(resolved, c.allowed_redirect_uri_patterns))
        throw not_allowed(resolved);
    enforce_application_type(c, resolved);
    return resolved;
}

} // namespace

std::string
OAuthClientInformation::validate_redirect_uri(const std::optional<std::string>& redirect_uri) const
{
    if (proxy_client)
        return proxy_validate_redirect_uri(*this, redirect_uri);
    if (redirect_uri)
    {
        if (!redirect_uris || std::find(redirect_uris->begin(), redirect_uris->end(),
                                        *redirect_uri) == redirect_uris->end())
            throw InvalidRedirectUriError("Redirect URI '" + *redirect_uri +
                                          "' not registered for client");
        return *redirect_uri;
    }
    if (redirect_uris && redirect_uris->size() == 1)
        return redirect_uris->front();
    throw InvalidRedirectUriError(
        "redirect_uri must be specified unless the client has exactly one registered URI");
}

// ================================================================== forms

namespace
{
void parse_urlencoded(const std::string& body, FormData& out)
{
    std::size_t start = 0;
    while (start <= body.size())
    {
        auto amp = body.find('&', start);
        if (amp == std::string::npos)
            amp = body.size();
        const std::string field = body.substr(start, amp - start);
        if (!field.empty())
        {
            const auto eq = field.find('=');
            if (eq == std::string::npos)
                out.add(unquote(field, true), "");
            else
                out.add(unquote(field.substr(0, eq), true), unquote(field.substr(eq + 1), true));
        }
        start = amp + 1;
    }
}

std::optional<std::string> header_param(const std::string& header, const std::string& name)
{
    std::size_t pos = 0;
    while ((pos = header.find(';', pos)) != std::string::npos)
    {
        ++pos;
        const auto eq = header.find('=', pos);
        if (eq == std::string::npos)
            break;
        const std::string key = lower(strip(header.substr(pos, eq - pos)));
        auto end = header.find(';', eq);
        std::string value = strip(
            header.substr(eq + 1, end == std::string::npos ? std::string::npos : end - eq - 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        if (key == name)
            return value;
    }
    return std::nullopt;
}

void parse_multipart(const std::string& body, const std::string& boundary, FormData& out)
{
    const std::string delim = "--" + boundary;
    std::size_t pos = body.find(delim);
    while (pos != std::string::npos)
    {
        pos += delim.size();
        if (body.compare(pos, 2, "--") == 0)
            break;
        if (body.compare(pos, 2, "\r\n") == 0)
            pos += 2;
        const auto header_end = body.find("\r\n\r\n", pos);
        if (header_end == std::string::npos)
            break;
        const std::string headers = body.substr(pos, header_end - pos);
        const auto next = body.find("\r\n" + delim, header_end + 4);
        if (next == std::string::npos)
            break;
        const std::string content = body.substr(header_end + 4, next - header_end - 4);
        std::optional<std::string> name;
        bool is_file = false;
        std::size_t line_start = 0;
        while (line_start < headers.size())
        {
            auto line_end = headers.find("\r\n", line_start);
            if (line_end == std::string::npos)
                line_end = headers.size();
            const std::string line = headers.substr(line_start, line_end - line_start);
            const auto colon = line.find(':');
            if (colon != std::string::npos &&
                lower(strip(line.substr(0, colon))) == "content-disposition")
            {
                const std::string value = line.substr(colon + 1);
                name = header_param(value, "name");
                is_file = header_param(value, "filename").has_value();
            }
            line_start = line_end + 2;
        }
        if (name && !is_file)
            out.add(*name, content);
        pos = next + 2;
    }
}
} // namespace

FormData FormData::parse(const CustomRouteRequest& req)
{
    FormData out;
    const std::string ct = lower(header_value(req, "content-type").value_or(""));
    const std::string main = strip(ct.substr(0, ct.find(';')));
    if (main == "application/x-www-form-urlencoded")
        parse_urlencoded(req.body, out);
    else if (main == "multipart/form-data")
    {
        if (const auto boundary =
                header_param(header_value(req, "content-type").value_or(""), "boundary"))
            parse_multipart(req.body, *boundary, out);
    }
    return out;
}

FormData FormData::from_query(const CustomRouteRequest& req)
{
    FormData out;
    const auto q = req.target.find('?');
    if (q != std::string::npos)
    {
        std::string query = req.target.substr(q + 1);
        if (const auto hash = query.find('#'); hash != std::string::npos)
            query = query.substr(0, hash);
        parse_urlencoded(query, out);
    }
    else
        for (const auto& [k, v] : req.query_params)
            out.add(k, v);
    return out;
}

std::optional<std::string> FormData::get(const std::string& key) const
{
    for (auto it = items_.rbegin(); it != items_.rend(); ++it)
        if (it->first == key)
            return it->second;
    return std::nullopt;
}

bool FormData::contains(const std::string& key) const
{
    return get(key).has_value();
}

void FormData::add(std::string key, std::string value)
{
    items_.emplace_back(std::move(key), std::move(value));
}

std::string construct_redirect_uri(
    const std::string& redirect_uri_base,
    const std::vector<std::pair<std::string, std::optional<std::string>>>& params)
{
    std::string base = redirect_uri_base, fragment, query;
    bool has_fragment = false;
    if (const auto hash = base.find('#'); hash != std::string::npos)
    {
        fragment = base.substr(hash + 1);
        has_fragment = true;
        base = base.substr(0, hash);
    }
    if (const auto q = base.find('?'); q != std::string::npos)
    {
        query = base.substr(q + 1);
        base = base.substr(0, q);
    }
    // Existing parameters, grouped by name in first-seen order, blanks dropped.
    std::vector<std::pair<std::string, std::vector<std::string>>> grouped;
    std::size_t start = 0;
    while (start <= query.size() && !query.empty())
    {
        auto amp = query.find('&', start);
        if (amp == std::string::npos)
            amp = query.size();
        const std::string field = query.substr(start, amp - start);
        const auto eq = field.find('=');
        if (eq != std::string::npos && eq + 1 < field.size())
        {
            const std::string k = unquote(field.substr(0, eq), true);
            const std::string v = unquote(field.substr(eq + 1), true);
            auto it = std::find_if(grouped.begin(), grouped.end(),
                                   [&k](const auto& g) { return g.first == k; });
            if (it == grouped.end())
                grouped.push_back({k, {v}});
            else
                it->second.push_back(v);
        }
        start = amp + 1;
    }
    std::vector<std::string> parts;
    for (const auto& [k, vs] : grouped)
        for (const auto& v : vs)
            parts.push_back(quote_plus(k) + "=" + quote_plus(v));
    for (const auto& [k, v] : params)
        if (v)
            parts.push_back(quote_plus(k) + "=" + quote_plus(*v));
    std::string out = base;
    if (!parts.empty())
        out += "?" + join(parts, "&");
    if (has_fragment && !fragment.empty())
        out += "#" + fragment;
    return out;
}

void validate_issuer_url(const std::string& url)
{
    const std::string scheme = lower(url.substr(0, url.find(':')));
    if (scheme != "https" && !is_loopback_issuer_host(url))
        throw std::invalid_argument("Issuer URL must be HTTPS");
    if (url.find('#') != std::string::npos)
        throw std::invalid_argument("Issuer URL must not have a fragment");
    if (url.find('?') != std::string::npos)
        throw std::invalid_argument("Issuer URL must not have a query string");
}

// ================================================================== client authentication

OAuthClientInformation ClientAuthenticator::authenticate_request(const CustomRouteRequest& req,
                                                                 FormData& form)
{
    const auto client_id = form.get("client_id");
    if (!client_id || client_id->empty())
        throw AuthenticationError("Missing client_id");
    auto client = provider_.get_client(*client_id);
    if (!client)
        throw AuthenticationError("Invalid client_id");

    std::optional<std::string> request_secret;
    const std::string auth_header = header_value(req, "authorization").value_or("");
    const std::string method = client->token_endpoint_auth_method.value_or("");
    if (method == "client_secret_basic")
    {
        if (auth_header.rfind("Basic ", 0) != 0)
            throw AuthenticationError(
                "Missing or invalid Basic authentication in Authorization header");
        std::string encoded;
        for (char c : auth_header.substr(6))
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=')
                encoded.push_back(c);
        const auto invalid = []()
        { return AuthenticationError("Invalid Basic authentication header"); };
        if (encoded.size() % 4 != 0)
            throw invalid();
        std::string unpadded = encoded;
        while (!unpadded.empty() && unpadded.back() == '=')
            unpadded.pop_back();
        if (unpadded.find('=') != std::string::npos)
            throw invalid();
        std::replace(unpadded.begin(), unpadded.end(), '+', '-');
        std::replace(unpadded.begin(), unpadded.end(), '/', '_');
        const auto decoded_bytes = util::crypto::base64url_decode(unpadded);
        if (!decoded_bytes)
            throw invalid();
        const std::string decoded(decoded_bytes->begin(), decoded_bytes->end());
        if (!is_valid_utf8(decoded))
            throw invalid();
        const auto colon = decoded.find(':');
        if (colon == std::string::npos)
            throw invalid();
        const std::string basic_id = unquote(decoded.substr(0, colon), false);
        request_secret = unquote(decoded.substr(colon + 1), false);
        if (basic_id != *client_id)
            throw AuthenticationError("Client ID mismatch in Basic auth");
    }
    else if (method == "client_secret_post")
        request_secret = form.get("client_secret");
    else if (method != "none")
        throw AuthenticationError("Unsupported auth method: " +
                                  (client->token_endpoint_auth_method
                                       ? *client->token_endpoint_auth_method
                                       : std::string("None")));

    const bool has_secret = client->client_secret && !client->client_secret->empty();
    if (method != "none" && !has_secret)
        throw AuthenticationError(
            "Client is registered for secret-based authentication but has no stored secret");
    if (has_secret)
    {
        if (!request_secret || request_secret->empty())
            throw AuthenticationError("Client secret is required");
        const auto& stored = *client->client_secret;
        if (stored.size() != request_secret->size() ||
            !util::crypto::constant_time_equal(
                reinterpret_cast<const std::uint8_t*>(stored.data()),
                reinterpret_cast<const std::uint8_t*>(request_secret->data()), stored.size()))
            throw AuthenticationError("Invalid client_secret");
        if (client->client_secret_expires_at && *client->client_secret_expires_at &&
            *client->client_secret_expires_at < static_cast<std::int64_t>(now()))
            throw AuthenticationError("Client secret has expired");
    }
    return *client;
}

// ================================================================== OAuthProvider

OAuthProvider::OAuthProvider(OAuthProviderOptions options)
    : AuthProvider(options.base_url, std::move(options.required_scopes), options.resource_base_url),
      client_registration_options_(std::move(options.client_registration_options)),
      revocation_options_(std::move(options.revocation_options))
{
    if (!base_url_)
        throw std::invalid_argument("OAuthProvider requires a base_url");
    issuer_url_ = options.issuer_url ? any_http_url(*options.issuer_url) : *base_url_;
    if (options.service_documentation_url)
        service_documentation_url_ = any_http_url(*options.service_documentation_url);
}

OAuthToken OAuthProvider::exchange_identity_assertion(const OAuthClientInformation&,
                                                      const IdentityAssertionParams&)
{
    throw TokenError("unsupported_grant_type",
                     "The JWT bearer grant is not supported by this authorization server");
}

std::optional<AccessToken> OAuthProvider::verify_token(const std::string& token)
{
    return load_access_token(token);
}

std::vector<std::string> OAuthProvider::scopes_supported() const
{
    if (client_registration_options_ && client_registration_options_->valid_scopes &&
        !client_registration_options_->valid_scopes->empty())
        return *client_registration_options_->valid_scopes;
    return required_scopes_;
}

OJ build_metadata(const std::string& issuer_url,
                  const std::optional<std::string>& service_documentation_url,
                  const ClientRegistrationOptions& client_registration_options,
                  const RevocationOptions& revocation_options, bool supports_identity_assertion)
{
    std::string base = issuer_url;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    OJ m;
    m["issuer"] = issuer_url;
    m["authorization_endpoint"] = any_http_url(base + "/authorize");
    m["token_endpoint"] = any_http_url(base + "/token");
    if (client_registration_options.enabled)
        m["registration_endpoint"] = any_http_url(base + "/register");
    if (client_registration_options.valid_scopes)
        m["scopes_supported"] = *client_registration_options.valid_scopes;
    m["response_types_supported"] = OJ::array({"code"});
    std::vector<std::string> grants{"authorization_code", "refresh_token"};
    if (supports_identity_assertion)
        grants.push_back(JWT_BEARER_GRANT_TYPE);
    m["grant_types_supported"] = grants;
    m["token_endpoint_auth_methods_supported"] =
        OJ::array({"client_secret_post", "client_secret_basic"});
    if (service_documentation_url)
        m["service_documentation"] = *service_documentation_url;
    if (revocation_options.enabled)
    {
        m["revocation_endpoint"] = any_http_url(base + "/revoke");
        m["revocation_endpoint_auth_methods_supported"] =
            OJ::array({"client_secret_post", "client_secret_basic"});
    }
    m["code_challenge_methods_supported"] = OJ::array({"S256"});
    if (supports_identity_assertion)
        m["authorization_grant_profiles_supported"] = OJ::array({ID_JAG_GRANT_PROFILE});
    return m;
}

// ------------------------------------------------------------------ enhanced /authorize

std::string create_unregistered_client_html(const std::string& client_id,
                                            const std::string& registration_endpoint,
                                            const std::string& discovery_endpoint,
                                            const std::optional<std::string>& server_name,
                                            const std::optional<std::string>& server_icon_url,
                                            const std::string& title)
{
    namespace ui = fastmcpp::util::ui;
    const std::string error_box = R"HTML(
        <div class="info-box error">
            <p>The client ID <code>)HTML" +
                                  ui::html_escape(client_id) +
                                  R"HTML(</code> was not found in the server's client registry.</p>
        </div>
    )HTML";
    const std::string warning_box = R"HTML(
        <div class="info-box warning">
            <p>Your MCP client opened this page to complete OAuth authorization,
            but the server did not recognize its client ID. To fix this:</p>
            <ul>
                <li>Close this browser window</li>
                <li>Clear authentication tokens in your MCP client (or restart it)</li>
                <li>Try connecting again - your client should automatically re-register</li>
            </ul>
        </div>
    )HTML";
    const std::string help_link = R"HTML(
        <div class="help-link-container">
            <span class="help-link">
                Why am I seeing this?
                <span class="tooltip">
                    OAuth 2.0 requires clients to register before authorization.
                    This server returned a 400 error because the provided client
                    ID was not found.
                    <br><br>
                    In browser-delegated OAuth flows, your application cannot
                    detect this error automatically; it's waiting for a
                    callback that will never arrive. You must manually clear
                    auth tokens and reconnect.
                </span>
            </span>
        </div>
    )HTML";
    const std::string endpoints_box =
        ui::create_detail_box({{"Registration endpoint", registration_endpoint},
                               {"Authorization server metadata", discovery_endpoint}});
    const std::string endpoints_section = R"HTML(
        <details>
            <summary>Server endpoints</summary>
            )HTML" + endpoints_box + R"HTML(
        </details>
    )HTML";
    const std::string content =
        "\n        <div class=\"container\">\n            " +
        ui::create_logo(server_icon_url,
                        server_name && !server_name->empty() ? *server_name : "FastMCP") +
        "\n            <h1>" + title + "</h1>\n            " + error_box + "\n            " +
        warning_box + "\n            " + endpoints_section + "\n        </div>\n        " +
        help_link + "\n    ";
    const std::string additional_styles = std::string(ui::INFO_BOX_STYLES) + ui::TOOLTIP_STYLES +
                                          ui::DETAILS_STYLES + ui::DETAIL_BOX_STYLES + R"CSS(
        /* Error variant for info-box */
        .info-box.error {
            background: #fef2f2;
            border-color: #f87171;
        }
        .info-box.error strong {
            color: #991b1b;
        }
        /* Warning variant for info-box (yellow) */
        .info-box.warning {
            background: #fffbeb;
            border-color: #fbbf24;
        }
        .info-box.warning strong {
            color: #92400e;
        }
        .info-box code {
            background: rgba(0, 0, 0, 0.05);
            padding: 2px 6px;
            border-radius: 3px;
            font-family: 'SF Mono', Monaco, 'Cascadia Code', monospace;
            font-size: 0.9em;
        }
        .info-box ul {
            margin: 10px 0;
            padding-left: 20px;
        }
        .info-box li {
            margin: 6px 0;
        }
        )CSS";
    return ui::create_page(content, title, additional_styles);
}

EnhancedAuthorizationHandler::EnhancedAuthorizationHandler(
    OAuthProvider& provider, std::string base_url, std::optional<std::string> issuer_url,
    std::optional<std::string> server_name, std::optional<std::string> server_icon_url)
    : AuthorizationHandler(provider), issuer_(issuer_url ? *issuer_url : base_url),
      base_url_(rstrip_slash(base_url)), server_name_(std::move(server_name)),
      server_icon_url_(std::move(server_icon_url))
{
}

CustomRouteResponse EnhancedAuthorizationHandler::handle(const CustomRouteRequest& req) const
{
    CustomRouteResponse response = AuthorizationHandler::handle(req);

    if (response.status >= 300 && response.status < 400)
        if (auto it = find_header(response.headers, "location"); it != response.headers.end())
        {
            bool carries_result = false;
            try
            {
                FormData query;
                parse_urlencoded(internal::pyurl::urlparse(it->second).query, query);
                carries_result = query.contains("error") || query.contains("code");
            }
            catch (const std::invalid_argument&)
            {
            }
            if (carries_result)
                it->second = build_client_redirect(it->second, {}, issuer_);
        }

    if (response.status == 400)
    {
        std::optional<std::string> client_id;
        try
        {
            const FormData params =
                req.method == "GET" ? FormData::from_query(req) : FormData::parse(req);
            client_id = params.get("client_id");
        }
        catch (const std::exception&)
        {
        }
        if (client_id && !client_id->empty())
        {
            try
            {
                const Json body = Json::parse(response.body);
                const std::string description =
                    body.contains("error_description") && body["error_description"].is_string()
                        ? lower(body["error_description"].get<std::string>())
                        : std::string();
                if (body.value("error", "") == "invalid_request" &&
                    description.find("not found") != std::string::npos)
                {
                    std::optional<std::string> state;
                    if (body.contains("state") && body["state"].is_string())
                        state = body["state"].get<std::string>();
                    return unregistered_client_response(req, *client_id, state);
                }
            }
            catch (const std::exception&)
            {
            }
        }
    }
    return response;
}

CustomRouteResponse EnhancedAuthorizationHandler::unregistered_client_response(
    const CustomRouteRequest& req, const std::string& client_id,
    const std::optional<std::string>& state) const
{
    const std::string registration_endpoint = base_url_ + "/register";
    const std::string discovery_endpoint = base_url_ + "/.well-known/oauth-authorization-server";
    const std::string accept = header_value(req, "accept").value_or("");
    CustomRouteResponse response;
    if (accept.find("text/html") != std::string::npos)
    {
        response.status = 400;
        response.content_type = "text/html; charset=utf-8";
        response.body = create_unregistered_client_html(
            client_id, registration_endpoint, discovery_endpoint, server_name_, server_icon_url_);
        for (const auto& [k, v] : fastmcpp::util::ui::secure_html_headers())
            response.headers[k] = v;
    }
    else
    {
        OJ body;
        body["error"] = "invalid_request";
        body["error_description"] =
            "Client ID '" + client_id +
            "' is not registered with this server. MCP clients should automatically re-register "
            "by sending a POST request to the registration_endpoint and retry authorization. If "
            "this persists, clear cached authentication tokens and reconnect.";
        if (state)
            body["state"] = *state;
        body["registration_endpoint"] = registration_endpoint;
        body["authorization_server_metadata"] = discovery_endpoint;
        response = json_response(400, body, {{"Cache-Control", "no-store"}});
    }
    response.headers["Link"] =
        "<" + registration_endpoint + ">; rel=\"http://oauth.net/core/2.1/#registration\"";
    return response;
}

std::vector<CustomRoute> create_auth_routes(OAuthProvider& provider,
                                            const AuthRoutesOptions& options)
{
    validate_issuer_url(options.base_url);
    auto metadata = build_metadata(options.base_url, options.service_documentation_url,
                                   options.client_registration_options, options.revocation_options,
                                   options.identity_assertion_enabled);
    if (options.issuer_url)
        metadata["issuer"] = *options.issuer_url;
    const std::string metadata_body = metadata.dump();
    auto authenticator = options.client_authenticator
                             ? options.client_authenticator
                             : std::make_shared<ClientAuthenticator>(provider);
    auto authorization = options.authorization_handler
                             ? options.authorization_handler
                             : std::make_shared<AuthorizationHandler>(provider);
    auto token =
        std::make_shared<TokenHandler>(provider, authenticator, options.identity_assertion_enabled);

    std::vector<CustomRoute> routes;
    const auto add = [&routes](const std::vector<std::string>& methods, const std::string& path,
                               std::function<CustomRouteResponse(const CustomRouteRequest&)> h)
    {
        for (const auto& m : methods)
            routes.push_back(CustomRoute{m, path, h});
    };

    add({"GET", "OPTIONS"}, "/.well-known/oauth-authorization-server",
        cors_handler(
            [metadata_body](const CustomRouteRequest&)
            {
                CustomRouteResponse r;
                r.body = metadata_body;
                r.content_type = "application/json";
                r.headers["Cache-Control"] = "public, max-age=3600";
                return r;
            },
            {"GET", "OPTIONS"}));
    add({"GET", "POST"}, "/authorize",
        body_limited([authorization](const CustomRouteRequest& req)
                     { return authorization->handle(req); }));
    add({"POST", "OPTIONS"}, "/token",
        cors_handler(options.limit_token_body
                         ? body_limited([token](const CustomRouteRequest& req)
                                        { return token->handle(req); })
                         : std::function<CustomRouteResponse(const CustomRouteRequest&)>(
                               [token](const CustomRouteRequest& req)
                               { return token->handle(req); }),
                     {"POST", "OPTIONS"}));
    if (options.client_registration_options.enabled)
    {
        auto reg =
            std::make_shared<RegistrationHandler>(provider, options.client_registration_options);
        add({"POST", "OPTIONS"}, "/register",
            cors_handler(
                body_limited([reg](const CustomRouteRequest& req) { return reg->handle(req); }),
                {"POST", "OPTIONS"}));
    }
    if (options.revocation_options.enabled)
    {
        auto rev = std::make_shared<RevocationHandler>(provider, authenticator);
        add({"POST", "OPTIONS"}, "/revoke",
            cors_handler(
                body_limited([rev](const CustomRouteRequest& req) { return rev->handle(req); }),
                {"POST", "OPTIONS"}));
    }
    return routes;
}

std::vector<CustomRoute> OAuthProvider::get_routes(const std::optional<std::string>& mcp_path)
{
    set_mcp_path(mcp_path);
    AuthRoutesOptions o;
    o.base_url = *base_url_;
    o.issuer_url = issuer_url_;
    o.service_documentation_url = service_documentation_url_;
    o.client_registration_options =
        client_registration_options_.value_or(ClientRegistrationOptions{});
    o.revocation_options = revocation_options_.value_or(RevocationOptions{});
    o.limit_token_body = false;
    auto routes = create_auth_routes(*this, o);
    if (const auto resource = resource_url(mcp_path))
    {
        auto prm = create_protected_resource_routes(*resource, {issuer_url_}, scopes_supported());
        routes.insert(routes.end(), prm.begin(), prm.end());
    }
    auto base = AuthProvider::get_routes(mcp_path);
    routes.insert(routes.end(), base.begin(), base.end());
    return routes;
}

std::vector<CustomRoute>
OAuthProvider::get_well_known_routes(const std::optional<std::string>& mcp_path)
{
    auto routes = AuthProvider::get_well_known_routes(mcp_path);
    std::string issuer_path;
    {
        const auto sep = issuer_url_.find("://");
        const auto slash = issuer_url_.find('/', sep == std::string::npos ? 0 : sep + 3);
        if (slash != std::string::npos)
            issuer_path = issuer_url_.substr(slash);
        while (!issuer_path.empty() && issuer_path.back() == '/')
            issuer_path.pop_back();
    }
    std::vector<CustomRoute> out;
    // Routes come in method pairs; emit the aliases after each method's route.
    for (const auto& route : routes)
    {
        if (route.path != "/.well-known/oauth-authorization-server")
        {
            out.push_back(route);
            continue;
        }
        if (!issuer_path.empty())
        {
            out.push_back(CustomRoute{route.method,
                                      "/.well-known/oauth-authorization-server" + issuer_path,
                                      route.handler});
            out.push_back(CustomRoute{
                route.method, "/.well-known/openid-configuration" + issuer_path, route.handler});
        }
        else
            out.push_back(route);
        out.push_back(
            CustomRoute{route.method, "/.well-known/openid-configuration", route.handler});
    }
    return out;
}

// ================================================================== /authorize

CustomRouteResponse AuthorizationHandler::handle(const CustomRouteRequest& req) const
{
    std::optional<std::string> state;
    std::optional<std::string> redirect_uri;
    std::optional<OAuthClientInformation> client;
    std::optional<FormData> params;

    const auto error_response = [&](const std::string& error,
                                    const std::optional<std::string>& description,
                                    bool attempt_load_client = true) -> CustomRouteResponse
    {
        if (!client && attempt_load_client && params)
            if (const auto id = params->get("client_id"); id && !id->empty())
                client = provider_.get_client(*id);
        if (!redirect_uri && client)
        {
            try
            {
                std::optional<std::string> raw;
                bool valid = true;
                if (params && params->contains("redirect_uri"))
                {
                    const auto r = validate_url(Json(*params->get("redirect_uri")), false, false);
                    if (r.url)
                        raw = r.url;
                    else
                        valid = false;
                }
                if (valid)
                    redirect_uri = client->validate_redirect_uri(raw);
            }
            catch (const InvalidRedirectUriError&)
            {
            }
        }
        if (!state && params)
            state = params->get("state");
        if (redirect_uri && client)
            return redirect_response(construct_redirect_uri(
                *redirect_uri,
                {{"error", error}, {"error_description", description}, {"state", state}}));
        OJ body;
        body["error"] = error;
        if (description)
            body["error_description"] = *description;
        if (state)
            body["state"] = *state;
        return json_response(400, body, {{"Cache-Control", "no-store"}});
    };

    try
    {
        params = req.method == "GET" ? FormData::from_query(req) : FormData::parse(req);
        state = params->get("state");

        // Validate the request fields in declaration order.
        FieldErrors errs;
        bool unsupported_response_type = false;
        const auto client_id = params->get("client_id");
        if (!client_id)
            errs.add("client_id", "Field required");
        std::optional<std::string> requested_redirect;
        if (const auto r = params->get("redirect_uri"))
        {
            const auto v = validate_url(Json(*r), false, false);
            if (v.url)
                requested_redirect = v.url;
            else
                errs.add("redirect_uri", v.error);
        }
        const auto response_type = params->get("response_type");
        if (!response_type)
            errs.add("response_type", "Field required");
        else if (*response_type != "code")
        {
            errs.add("response_type", "Input should be 'code'");
            unsupported_response_type = true;
        }
        const auto code_challenge = params->get("code_challenge");
        if (!code_challenge)
            errs.add("code_challenge", "Field required");
        if (const auto m = params->get("code_challenge_method"); m && *m != "S256")
            errs.add("code_challenge_method", "Input should be 'S256'");
        if (!errs.empty())
            return error_response(unsupported_response_type ? "unsupported_response_type"
                                                            : "invalid_request",
                                  errs.str());

        client = provider_.get_client(*client_id);
        if (!client)
            return error_response("invalid_request", "Client ID '" + *client_id + "' not found",
                                  false);
        try
        {
            redirect_uri = client->validate_redirect_uri(requested_redirect);
        }
        catch (const InvalidRedirectUriError& e)
        {
            return error_response("invalid_request", std::string(e.what()));
        }
        std::optional<std::vector<std::string>> scopes;
        try
        {
            scopes = client->validate_scope(params->get("scope"));
        }
        catch (const InvalidScopeError& e)
        {
            return error_response("invalid_scope", std::string(e.what()));
        }
        AuthorizationParams ap;
        ap.state = state;
        ap.scopes = scopes;
        ap.code_challenge = *code_challenge;
        ap.redirect_uri = *redirect_uri;
        ap.redirect_uri_provided_explicitly = requested_redirect.has_value();
        ap.resource = params->get("resource");
        try
        {
            return redirect_response(provider_.authorize(*client, ap));
        }
        catch (const AuthorizeError& e)
        {
            return error_response(e.error(), e.description());
        }
    }
    catch (const std::exception&)
    {
        return error_response("server_error", std::string("An unexpected error occurred"));
    }
}

// ================================================================== /token

TokenHandler::TokenHandler(OAuthProvider& provider,
                           std::shared_ptr<ClientAuthenticator> authenticator,
                           bool identity_assertion_enabled)
    : provider_(provider), authenticator_(std::move(authenticator)),
      identity_assertion_enabled_(identity_assertion_enabled)
{
}

std::optional<CustomRouteResponse> TokenHandler::maybe_handle_id_jag(const CustomRouteRequest& req,
                                                                     FormData& form) const
{
    if (form.get("grant_type") != std::optional<std::string>(JWT_BEARER_GRANT_TYPE))
        return std::nullopt;
    OAuthClientInformation client;
    try
    {
        client = authenticator_->authenticate_request(req, form);
    }
    catch (const AuthenticationError& e)
    {
        return token_error("invalid_client", std::string(e.what()), 401);
    }
    if (std::find(client.grant_types.begin(), client.grant_types.end(), JWT_BEARER_GRANT_TYPE) ==
        client.grant_types.end())
        return token_error("unsupported_grant_type",
                           "Unsupported grant type (supported grant types are " +
                               py_repr_list(client.grant_types) + ")");
    const auto assertion = form.get("assertion");
    if (!assertion || assertion->empty())
        return token_error("invalid_request", std::string("Missing assertion"));
    IdentityAssertionParams params;
    params.assertion = *assertion;
    if (const auto scope = form.get("scope"); scope && !scope->empty())
        params.scopes = split_space(*scope);
    params.resource = form.get("resource");
    try
    {
        return json_response(200, token_json(provider_.exchange_identity_assertion(client, params)),
                             kNoStore);
    }
    catch (const TokenError& e)
    {
        return token_error(e.error(), e.description(), e.error() == "invalid_grant" ? 401 : 400);
    }
}

CustomRouteResponse TokenHandler::handle(const CustomRouteRequest& req) const
{
    FormData form = FormData::parse(req);
    if (identity_assertion_enabled_)
        if (auto r = maybe_handle_id_jag(req, form))
            return *r;

    CustomRouteResponse response = [&]() -> CustomRouteResponse
    {
        OAuthClientInformation client;
        try
        {
            client = authenticator_->authenticate_request(req, form);
        }
        catch (const AuthenticationError& e)
        {
            return token_error("invalid_client", std::string(e.what()), 401);
        }

        static const std::vector<std::string> kGrants{"authorization_code", "refresh_token",
                                                      JWT_BEARER_GRANT_TYPE};
        const auto grant_type = form.get("grant_type");
        if (!grant_type)
            return token_error(
                "invalid_request",
                std::string(": Unable to extract tag using discriminator 'grant_type'"));
        if (std::find(kGrants.begin(), kGrants.end(), *grant_type) == kGrants.end())
            return token_error("invalid_request",
                               ": Input tag '" + *grant_type +
                                   "' found using 'grant_type' does not match any of the expected "
                                   "tags: 'authorization_code', 'refresh_token', '" +
                                   JWT_BEARER_GRANT_TYPE + "'");
        FieldErrors errs;
        const std::string prefix = *grant_type + ".";
        const auto required = [&](const char* field)
        {
            const auto v = form.get(field);
            if (!v)
                errs.add(prefix + field, "Field required");
            return v;
        };
        std::optional<std::string> code, refresh, assertion, verifier, token_redirect;
        if (*grant_type == "authorization_code")
        {
            code = required("code");
            if (const auto r = form.get("redirect_uri"))
            {
                const auto v = validate_url(Json(*r), false, false);
                if (v.url)
                    token_redirect = v.url;
                else
                    errs.add(prefix + "redirect_uri", v.error);
            }
            required("client_id");
            verifier = required("code_verifier");
        }
        else if (*grant_type == "refresh_token")
        {
            refresh = required("refresh_token");
            required("client_id");
        }
        else
        {
            assertion = required("assertion");
            required("client_id");
        }
        if (!errs.empty())
            return token_error("invalid_request", errs.str());
        const std::string request_client_id = *form.get("client_id");

        if (std::find(client.grant_types.begin(), client.grant_types.end(), *grant_type) ==
            client.grant_types.end())
            return token_error("unsupported_grant_type",
                               "Unsupported grant type (supported grant types are " +
                                   py_repr_list(client.grant_types) + ")");

        OAuthToken tokens;
        if (*grant_type == "authorization_code")
        {
            auto auth_code = provider_.load_authorization_code(client, *code);
            if (!auth_code || auth_code->client_id != request_client_id)
                return token_error("invalid_grant",
                                   std::string("authorization code does not exist"));
            if (auth_code->expires_at < now())
                return token_error("invalid_grant", std::string("authorization code has expired"));
            const std::optional<std::string> auth_redirect =
                auth_code->redirect_uri_provided_explicitly
                    ? std::optional<std::string>(auth_code->redirect_uri)
                    : std::nullopt;
            if (token_redirect != auth_redirect)
                return token_error(
                    "invalid_request",
                    std::string("redirect_uri did not match the one used when creating auth code"));
            const auto digest = util::crypto::sha256(*verifier);
            if (util::crypto::base64url_encode(digest.data(), digest.size()) !=
                auth_code->code_challenge)
                return token_error("invalid_grant", std::string("incorrect code_verifier"));
            try
            {
                tokens = provider_.exchange_authorization_code(client, *auth_code);
            }
            catch (const TokenError& e)
            {
                return token_error(e.error(), e.description());
            }
        }
        else if (*grant_type == "refresh_token")
        {
            auto rt = provider_.load_refresh_token(client, *refresh);
            if (!rt || rt->client_id != request_client_id)
                return token_error("invalid_grant", std::string("refresh token does not exist"));
            if (rt->expires_at && *rt->expires_at && static_cast<double>(*rt->expires_at) < now())
                return token_error("invalid_grant", std::string("refresh token has expired"));
            const auto scope = form.get("scope");
            const std::vector<std::string> scopes =
                scope && !scope->empty() ? split_space(*scope) : rt->scopes;
            for (const auto& s : scopes)
                if (std::find(rt->scopes.begin(), rt->scopes.end(), s) == rt->scopes.end())
                    return token_error("invalid_scope", "cannot request scope `" + s +
                                                            "` not provided by refresh token");
            try
            {
                tokens = provider_.exchange_refresh_token(client, *rt, scopes);
            }
            catch (const TokenError& e)
            {
                return token_error(e.error(), e.description());
            }
        }
        else
        {
            if (!identity_assertion_enabled_)
                return token_error(
                    "unsupported_grant_type",
                    std::string(
                        "The JWT bearer grant is not supported by this authorization server"));
            if (!client.client_secret || client.client_secret->empty())
                return token_error(
                    "unauthorized_client",
                    std::string("The JWT bearer grant requires a confidential client"));
            IdentityAssertionParams params;
            params.assertion = *assertion;
            if (const auto scope = form.get("scope"); scope && !scope->empty())
                params.scopes = split_space(*scope);
            params.resource = form.get("resource");
            try
            {
                tokens = provider_.exchange_identity_assertion(client, params);
            }
            catch (const TokenError& e)
            {
                return token_error(e.error(), e.description());
            }
        }
        return json_response(200, token_json(tokens), kNoStore);
    }();

    // RFC 6749 §5.2 wants invalid_client for authentication failures, and MCP
    // wants 401 for invalid or expired grants.
    if (response.status == 401 || response.status == 400)
    {
        try
        {
            const auto body = Json::parse(response.body);
            const std::string error = body.value("error", "");
            std::optional<std::string> description;
            if (body.contains("error_description") && body["error_description"].is_string())
                description = body["error_description"].get<std::string>();
            if (response.status == 401 && error == "unauthorized_client")
                return token_error("invalid_client", description, 401);
            if (response.status == 400 && error == "invalid_grant")
                return token_error("invalid_grant", description, 401);
        }
        catch (const std::exception&)
        {
        }
    }
    return response;
}

// ================================================================== /register

CustomRouteResponse RegistrationHandler::handle(const CustomRouteRequest& req) const
{
    const auto bad = [](const std::string& description)
    {
        OJ j;
        j["error"] = "invalid_client_metadata";
        j["error_description"] = description;
        return json_response(400, j);
    };
    auto parsed = parse_client_metadata(req.body);
    if (auto* err = std::get_if<std::string>(&parsed))
        return bad(*err);
    auto info = std::get<OAuthClientInformation>(std::move(parsed));

    info.client_id = uuid4();
    if (!info.token_endpoint_auth_method)
        info.token_endpoint_auth_method = "client_secret_post";
    if (*info.token_endpoint_auth_method == "private_key_jwt")
        return bad("token_endpoint_auth_method 'private_key_jwt' is not supported");
    if (*info.token_endpoint_auth_method != "none")
        info.client_secret = token_hex(32);

    if (!info.scope && options_.default_scopes)
        info.scope = join(*options_.default_scopes, " ");
    else if (info.scope && options_.valid_scopes)
    {
        std::vector<std::string> invalid;
        for (const auto& s : split_whitespace(*info.scope))
            if (std::find(options_.valid_scopes->begin(), options_.valid_scopes->end(), s) ==
                    options_.valid_scopes->end() &&
                std::find(invalid.begin(), invalid.end(), s) == invalid.end())
                invalid.push_back(s);
        if (!invalid.empty())
            return bad("Requested scopes are not valid: " + join(invalid, ", "));
    }
    const auto has = [](const std::vector<std::string>& v, const std::string& s)
    { return std::find(v.begin(), v.end(), s) != v.end(); };
    if (!has(info.grant_types, "authorization_code"))
        return bad("grant_types must include 'authorization_code'");
    if (has(info.grant_types, JWT_BEARER_GRANT_TYPE))
        return bad(std::string("grant_types must not include '") + JWT_BEARER_GRANT_TYPE +
                   "'; the identity-assertion grant requires a pre-registered client");
    if (!has(info.response_types, "code"))
        return bad("response_types must include 'code' for authorization_code grant");

    const auto issued_at = static_cast<std::int64_t>(now());
    info.client_id_issued_at = issued_at;
    if (info.client_secret)
        info.client_secret_expires_at = options_.client_secret_expiry_seconds
                                            ? issued_at + *options_.client_secret_expiry_seconds
                                            : 0;
    // The stored record reads placeholder members as omitted.
    for (auto* field : {&info.scope, &info.client_name, &info.software_id, &info.software_version})
        if (*field && (*field)->empty())
            field->reset();
    if (info.jwks.is_string() && info.jwks.get<std::string>().empty())
        info.jwks = nullptr;
    try
    {
        provider_.register_client(info);
        return json_response(201, info.to_json());
    }
    catch (const RegistrationError& e)
    {
        OJ j;
        j["error"] = e.error();
        if (e.description())
            j["error_description"] = *e.description();
        return json_response(400, j);
    }
}

// ================================================================== /revoke

CustomRouteResponse RevocationHandler::handle(const CustomRouteRequest& req) const
{
    FormData form = FormData::parse(req);
    OAuthClientInformation client;
    try
    {
        client = authenticator_->authenticate_request(req, form);
    }
    catch (const AuthenticationError& e)
    {
        return json_response(401, error_body("unauthorized_client", std::string(e.what())));
    }
    FieldErrors errs;
    const auto token = form.get("token");
    if (!token)
        errs.add("token", "Field required");
    const auto hint = form.get("token_type_hint");
    if (hint && *hint != "access_token" && *hint != "refresh_token")
        errs.add("token_type_hint", "Input should be 'access_token' or 'refresh_token'");
    if (!form.get("client_id"))
        errs.add("client_id", "Field required");
    if (!form.get("client_secret"))
        errs.add("client_secret", "Field required");
    if (!errs.empty())
        return json_response(400, error_body("invalid_request", errs.str()));

    std::optional<std::variant<AccessToken, RefreshToken>> found;
    const auto try_access = [&]()
    {
        if (auto t = provider_.load_access_token(*token))
            found = *t;
    };
    const auto try_refresh = [&]()
    {
        if (auto t = provider_.load_refresh_token(client, *token))
            found = *t;
    };
    if (hint == std::optional<std::string>("refresh_token"))
    {
        try_refresh();
        if (!found)
            try_access();
    }
    else
    {
        try_access();
        if (!found)
            try_refresh();
    }
    if (found)
    {
        const std::string owner = std::visit([](const auto& t) { return t.client_id; }, *found);
        if (owner == client.client_id)
            provider_.revoke_token(*found);
    }
    CustomRouteResponse r;
    r.status = 200;
    r.content_type = "";
    r.headers = kNoStore;
    return r;
}

// ================================================================== in memory

InMemoryOAuthProvider::InMemoryOAuthProvider(OAuthProviderOptions options)
    : OAuthProvider(
          [&options]()
          {
              if (!options.base_url)
                  options.base_url = "http://fastmcp.example.com";
              return std::move(options);
          }())
{
}

std::optional<OAuthClientInformation>
InMemoryOAuthProvider::get_client(const std::string& client_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = clients_.find(client_id);
    if (it == clients_.end())
        return std::nullopt;
    return it->second;
}

void InMemoryOAuthProvider::register_client(const OAuthClientInformation& client_info)
{
    if (client_info.scope && client_registration_options_ &&
        client_registration_options_->valid_scopes)
    {
        std::vector<std::string> invalid;
        for (const auto& s : split_whitespace(*client_info.scope))
            if (std::find(client_registration_options_->valid_scopes->begin(),
                          client_registration_options_->valid_scopes->end(),
                          s) == client_registration_options_->valid_scopes->end())
                invalid.push_back(s);
        if (!invalid.empty())
            throw std::invalid_argument("Requested scopes are not valid: " + join(invalid, ", "));
    }
    if (client_info.client_id.empty())
        throw std::invalid_argument("client_id is required for client registration");
    std::lock_guard<std::mutex> lock(mutex_);
    clients_[client_info.client_id] = client_info;
}

std::string InMemoryOAuthProvider::authorize(const OAuthClientInformation& client,
                                             const AuthorizationParams& params)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clients_.count(client.client_id))
        throw AuthorizeError("unauthorized_client",
                             "Client '" + client.client_id + "' not registered.");
    const std::string code = "test_auth_code_" + token_hex(16);
    std::vector<std::string> scopes = params.scopes.value_or(std::vector<std::string>{});
    if (client.scope && !client.scope->empty())
    {
        const auto allowed = split_whitespace(*client.scope);
        scopes.erase(std::remove_if(
                         scopes.begin(), scopes.end(), [&allowed](const std::string& s)
                         { return std::find(allowed.begin(), allowed.end(), s) == allowed.end(); }),
                     scopes.end());
    }
    AuthorizationCode ac;
    ac.code = code;
    ac.client_id = client.client_id;
    ac.redirect_uri = params.redirect_uri;
    ac.redirect_uri_provided_explicitly = params.redirect_uri_provided_explicitly;
    ac.scopes = scopes;
    ac.expires_at = now() + DEFAULT_AUTH_CODE_EXPIRY_SECONDS;
    ac.code_challenge = params.code_challenge;
    auth_codes_[code] = ac;
    return construct_redirect_uri(params.redirect_uri, {{"code", code}, {"state", params.state}});
}

std::optional<AuthorizationCode>
InMemoryOAuthProvider::load_authorization_code(const OAuthClientInformation& client,
                                               const std::string& code)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = auth_codes_.find(code);
    if (it == auth_codes_.end() || it->second.client_id != client.client_id)
        return std::nullopt;
    if (it->second.expires_at < now())
    {
        auth_codes_.erase(it);
        return std::nullopt;
    }
    return it->second;
}

OAuthToken InMemoryOAuthProvider::exchange_authorization_code(const OAuthClientInformation& client,
                                                              const AuthorizationCode& code)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!auth_codes_.count(code.code))
        throw TokenError("invalid_grant", "Authorization code not found or already used.");
    auth_codes_.erase(code.code);
    const std::string access = "test_access_token_" + token_hex(32);
    const std::string refresh = "test_refresh_token_" + token_hex(32);
    AccessToken at;
    at.token = access;
    at.client_id = client.client_id;
    at.scopes = code.scopes;
    at.expires_at = static_cast<std::int64_t>(now() + DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS);
    access_tokens_[access] = at;
    RefreshToken rt;
    rt.token = refresh;
    rt.client_id = client.client_id;
    rt.scopes = code.scopes;
    refresh_tokens_[refresh] = rt;
    access_to_refresh_[access] = refresh;
    refresh_to_access_[refresh] = access;
    OAuthToken out;
    out.access_token = access;
    out.expires_in = DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS;
    out.refresh_token = refresh;
    out.scope = join(code.scopes, " ");
    return out;
}

std::optional<RefreshToken>
InMemoryOAuthProvider::load_refresh_token(const OAuthClientInformation& client,
                                          const std::string& refresh_token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = refresh_tokens_.find(refresh_token);
    if (it == refresh_tokens_.end() || it->second.client_id != client.client_id)
        return std::nullopt;
    if (it->second.expires_at && static_cast<double>(*it->second.expires_at) < now())
    {
        revoke_internal(std::nullopt, refresh_token);
        return std::nullopt;
    }
    return it->second;
}

OAuthToken InMemoryOAuthProvider::exchange_refresh_token(const OAuthClientInformation& client,
                                                         const RefreshToken& refresh_token,
                                                         const std::vector<std::string>& scopes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& s : scopes)
        if (std::find(refresh_token.scopes.begin(), refresh_token.scopes.end(), s) ==
            refresh_token.scopes.end())
            throw TokenError("invalid_scope",
                             "Requested scopes exceed those authorized by the refresh token.");
    revoke_internal(std::nullopt, refresh_token.token);
    const std::string access = "test_access_token_" + token_hex(32);
    const std::string refresh = "test_refresh_token_" + token_hex(32);
    AccessToken at;
    at.token = access;
    at.client_id = client.client_id;
    at.scopes = scopes;
    at.expires_at = static_cast<std::int64_t>(now() + DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS);
    access_tokens_[access] = at;
    RefreshToken rt;
    rt.token = refresh;
    rt.client_id = client.client_id;
    rt.scopes = scopes;
    refresh_tokens_[refresh] = rt;
    access_to_refresh_[access] = refresh;
    refresh_to_access_[refresh] = access;
    OAuthToken out;
    out.access_token = access;
    out.expires_in = DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS;
    out.refresh_token = refresh;
    out.scope = join(scopes, " ");
    return out;
}

std::optional<AccessToken> InMemoryOAuthProvider::load_access_token(const std::string& token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = access_tokens_.find(token);
    if (it == access_tokens_.end())
        return std::nullopt;
    if (it->second.expires_at && static_cast<double>(*it->second.expires_at) < now())
    {
        revoke_internal(token, std::nullopt);
        return std::nullopt;
    }
    return it->second;
}

void InMemoryOAuthProvider::revoke_internal(const std::optional<std::string>& access_token,
                                            const std::optional<std::string>& refresh_token)
{
    if (access_token)
    {
        access_tokens_.erase(*access_token);
        if (const auto it = access_to_refresh_.find(*access_token); it != access_to_refresh_.end())
        {
            refresh_tokens_.erase(it->second);
            refresh_to_access_.erase(it->second);
            access_to_refresh_.erase(it);
        }
    }
    if (refresh_token)
    {
        refresh_tokens_.erase(*refresh_token);
        if (const auto it = refresh_to_access_.find(*refresh_token); it != refresh_to_access_.end())
        {
            access_tokens_.erase(it->second);
            access_to_refresh_.erase(it->second);
            refresh_to_access_.erase(it);
        }
    }
}

void InMemoryOAuthProvider::revoke_token(const std::variant<AccessToken, RefreshToken>& token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto* a = std::get_if<AccessToken>(&token))
        revoke_internal(a->token, std::nullopt);
    else
        revoke_internal(std::nullopt, std::get<RefreshToken>(token).token);
}

std::map<std::string, OAuthClientInformation> InMemoryOAuthProvider::clients() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_;
}

std::map<std::string, AuthorizationCode> InMemoryOAuthProvider::auth_codes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return auth_codes_;
}

std::map<std::string, AccessToken> InMemoryOAuthProvider::access_tokens() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return access_tokens_;
}

std::map<std::string, RefreshToken> InMemoryOAuthProvider::refresh_tokens() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return refresh_tokens_;
}

} // namespace fastmcpp::server::auth
