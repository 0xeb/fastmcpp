#include "fastmcpp/mcp/envelope.hpp"

#include "fastmcpp/protocol.hpp"
#include "fastmcpp/util/pagination.hpp"

#include <cstdint>
#include <vector>

namespace fastmcpp::mcp
{

namespace
{

constexpr const char* kB64Prefix = "=?base64?";
constexpr const char* kB64Suffix = "?=";

bool is_sentinel(const std::string& v)
{
    const std::string prefix = kB64Prefix;
    const std::string suffix = kB64Suffix;
    return v.size() >= prefix.size() + suffix.size() && v.compare(0, prefix.size(), prefix) == 0 &&
           v.compare(v.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_valid_utf8(const std::string& s)
{
    size_t i = 0;
    while (i < s.size())
    {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t len;
        uint32_t cp;
        if (c < 0x80)
        {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0)
        {
            len = 2;
            cp = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0)
        {
            len = 3;
            cp = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0)
        {
            len = 4;
            cp = c & 0x07;
        }
        else
            return false;
        if (i + len > s.size())
            return false;
        for (size_t k = 1; k < len; ++k)
        {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Overlong forms, surrogates and out-of-range code points are not UTF-8.
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

bool header_safe(const std::string& v)
{
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7E)
            return false;
    return true;
}

const char* name_key_for(const std::string& method)
{
    // PYTHON REFERENCE: mcp/shared/inbound.py:71-77 (NAME_BEARING_METHODS)
    if (method == "tools/call" || method == "prompts/get")
        return "name";
    if (method == "resources/read")
        return "uri";
    return nullptr;
}

const std::string* find_header(const HeaderMap& headers, const char* name)
{
    auto it = headers.find(name);
    return it == headers.end() ? nullptr : &it->second;
}

} // namespace

bool has_modern_envelope(const Json& params)
{
    // PYTHON REFERENCE: mcp/server/runner.py:505-517 (_has_modern_envelope)
    return params.is_object() && params.contains("_meta") && params["_meta"].is_object() &&
           params["_meta"].contains(kProtocolVersionMetaKey);
}

EnvelopeRejection unsupported_protocol_version(const std::string& requested)
{
    Json supported = Json::array();
    for (const auto& v : protocol::kModernVersions)
        supported.push_back(std::string(v));
    return {kUnsupportedProtocolVersion, "Unsupported protocol version",
            Json{{"supported", supported}, {"requested", requested}}};
}

std::variant<Envelope, EnvelopeRejection> classify_request(const Json& body,
                                                           const HeaderMap* headers)
{
    const Json* meta = nullptr;
    if (body.is_object() && body.contains("params") && body["params"].is_object() &&
        body["params"].contains("_meta"))
        meta = &body["params"]["_meta"];

    if (meta == nullptr || !meta->is_object())
        return EnvelopeRejection{
            kInvalidParams,
            std::string("params._meta must be an object carrying the required '") +
                kProtocolVersionMetaKey + "' and '" + kClientCapabilitiesMetaKey +
                "' envelope keys",
            nullptr};

    std::string missing;
    for (const char* key : {kProtocolVersionMetaKey, kClientCapabilitiesMetaKey})
    {
        if (!meta->contains(key))
        {
            if (!missing.empty())
                missing += ", ";
            missing += key;
        }
    }
    if (!missing.empty())
        return EnvelopeRejection{kInvalidParams,
                                 "params._meta is missing the required envelope key(s): " + missing,
                                 nullptr};

    const Json& version = (*meta)[kProtocolVersionMetaKey];

    if (headers != nullptr)
    {
        const std::string* version_header = find_header(*headers, kProtocolVersionHeader);
        if (version_header == nullptr || !version.is_string() ||
            *version_header != version.get<std::string>())
            return EnvelopeRejection{kHeaderMismatch,
                                     std::string(kProtocolVersionHeader) +
                                         " header does not match the request envelope's protocol "
                                         "version",
                                     nullptr};

        const std::string method = body.value("method", std::string());
        const std::string* method_header = find_header(*headers, kMethodHeader);
        if (method_header == nullptr || *method_header != method)
            return EnvelopeRejection{kHeaderMismatch,
                                     std::string(kMethodHeader) +
                                         " header does not match the request body's method",
                                     nullptr};

        if (const char* name_key = name_key_for(method))
        {
            const Json& params = body["params"];
            if (params.contains(name_key) && !params[name_key].is_null())
            {
                const std::string* name_header = find_header(*headers, kNameHeader);
                std::optional<std::string> decoded =
                    name_header ? decode_header_value(*name_header) : std::nullopt;
                if (!decoded || !params[name_key].is_string() ||
                    *decoded != params[name_key].get<std::string>())
                    return EnvelopeRejection{kHeaderMismatch,
                                             std::string(kNameHeader) +
                                                 " header does not match the request body's '" +
                                                 name_key + "' parameter",
                                             nullptr};
            }
        }
    }

    if (!version.is_string())
        return EnvelopeRejection{kInvalidParams,
                                 "the protocol-version envelope value must be a string", nullptr};

    const std::string requested = version.get<std::string>();
    if (!protocol::is_modern(requested))
        return unsupported_protocol_version(requested);

    Envelope env;
    env.protocol_version = requested;
    env.client_info = meta->contains(kClientInfoMetaKey) ? (*meta)[kClientInfoMetaKey] : Json();
    env.client_capabilities = (*meta)[kClientCapabilitiesMetaKey];
    if (meta->contains(kLogLevelMetaKey) && (*meta)[kLogLevelMetaKey].is_string())
        env.log_level = (*meta)[kLogLevelMetaKey].get<std::string>();
    return env;
}

std::optional<std::string> decode_header_value(const std::string& value)
{
    if (!is_sentinel(value))
        return value;
    const size_t prefix = std::char_traits<char>::length(kB64Prefix);
    const size_t suffix = std::char_traits<char>::length(kB64Suffix);
    const std::string payload = value.substr(prefix, value.size() - prefix - suffix);
    if (payload.size() % 4 != 0)
        return std::nullopt;
    const std::string decoded = util::pagination::base64_decode(payload);
    // Canonical round trip rejects bad characters, bad padding and non-zero
    // trailing bits, which is what Python's validate=True + re-encode checks.
    if (util::pagination::base64_encode(decoded) != payload)
        return std::nullopt;
    if (!is_valid_utf8(decoded))
        return std::nullopt;
    return decoded;
}

std::string encode_header_value(const std::string& value)
{
    const bool trimmed = value.empty() || (value.front() != ' ' && value.back() != ' ' &&
                                           value.front() != '\t' && value.back() != '\t');
    if (header_safe(value) && trimmed && !is_sentinel(value))
        return value;
    return std::string(kB64Prefix) + util::pagination::base64_encode(value) + kB64Suffix;
}

int http_status_for_error(int code, int fallback)
{
    switch (code)
    {
    case kParseError:
    case kInvalidRequest:
    case kInvalidParams:
    case kHeaderMismatch:
    case kMissingRequiredClientCapability:
    case kUnsupportedProtocolVersion:
        return 400;
    case kMethodNotFound:
        return 404;
    default:
        return fallback;
    }
}

} // namespace fastmcpp::mcp
