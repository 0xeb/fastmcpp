#include "fastmcpp/server/oauth_proxy.hpp"

#include "../internal/pyjson.hpp"
#include "../internal/pyurl.hpp"
#include "auth_detail.hpp"
#include "fastmcpp/client/http_rpc_transport.hpp"
#include "fastmcpp/client/stores.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/server/redirect_validation.hpp"
#include "fastmcpp/settings.hpp"
#include "fastmcpp/util/cookies.hpp"
#include "fastmcpp/util/crypto.hpp"
#include "fastmcpp/util/ui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>

namespace fastmcpp::server::auth
{
namespace
{

namespace crypto = fastmcpp::util::crypto;
namespace ui = fastmcpp::util::ui;
namespace ck = fastmcpp::util::cookies;
namespace pyjson = fastmcpp::internal::pyjson;
namespace pu = fastmcpp::internal::pyurl;
using OJ = nlohmann::ordered_json;

constexpr const char* kUpstreamTokens = "mcp-upstream-tokens";
constexpr const char* kClients = "mcp-oauth-proxy-clients";
constexpr const char* kTransactions = "mcp-oauth-transactions";
constexpr const char* kConsentCsrf = "mcp-consent-csrf-tokens";
constexpr const char* kCodes = "mcp-authorization-codes";
constexpr const char* kJtiMappings = "mcp-jti-mappings";
constexpr const char* kRefreshTokens = "mcp-refresh-tokens";
constexpr const char* kRotatedRefresh = "mcp-rotated-refresh-tokens";
constexpr const char* kEndedGrants = "mcp-ended-grants";

constexpr std::size_t kRefreshLockCacheSize = 10000;
constexpr double kRefreshReuseGraceSeconds = 60.0;
constexpr double kStateCullIntervalSeconds = 3600.0;
constexpr long long kTransactionTtl = 15 * 60;
constexpr const char* kIdJagGrantMarker = "id_jag";
constexpr std::size_t kMaxRememberedClients = 25;
constexpr std::size_t kMaxCsrfTokens = 10;
constexpr long long kRememberMaxAge = 365LL * 24 * 3600;
constexpr const char* kConsentBindingCookieBase = "MCP_CONSENT_BINDING";
constexpr const char* kConsentStateCookieBase = "MCP_CONSENT_STATE";
constexpr const char* kApprovedClients = "MCP_APPROVED_CLIENTS";
constexpr const char* kDeniedClients = "MCP_DENIED_CLIENTS";

std::string hex(const std::uint8_t* data, std::size_t n)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i)
    {
        out.push_back(digits[data[i] >> 4]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

std::string sha256_hex(const std::string& s)
{
    const auto d = crypto::sha256(s);
    return hex(d.data(), d.size());
}

/// secrets.token_urlsafe(n)
std::string token_urlsafe(std::size_t n)
{
    return crypto::base64url_encode(crypto::random_bytes(n));
}

std::string b64encode(const std::string& s)
{
    return crypto::base64_encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

/// base64.b64decode without validation: characters outside the alphabet are
/// discarded; incorrect padding fails.
std::optional<std::string> b64decode(const std::string& s)
{
    std::string clean;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=')
            clean.push_back(c);
    std::string data;
    std::size_t pad = 0;
    for (char c : clean)
    {
        if (c == '=')
            ++pad;
        else
        {
            if (pad)
                return std::nullopt;
            data.push_back(c == '+' ? '-' : c == '/' ? '_' : c);
        }
    }
    if (data.size() % 4 == 1)
        return std::nullopt;
    if ((data.size() + pad) % 4 != 0)
        return std::nullopt;
    auto bytes = crypto::base64url_decode(data);
    if (!bytes)
        return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

crypto::Bytes bytes_of(const std::string& s)
{
    return crypto::Bytes(s.begin(), s.end());
}

std::string join(const std::vector<std::string>& v, const std::string& sep)
{
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i)
        out += (i ? sep : "") + v[i];
    return out;
}

std::string rstrip_slash(std::string s)
{
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

bool ct_equal(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    return crypto::constant_time_equal(reinterpret_cast<const std::uint8_t*>(a.data()),
                                       reinterpret_cast<const std::uint8_t*>(b.data()), a.size());
}

bool contains(const std::vector<std::string>& v, const std::string& s)
{
    return std::find(v.begin(), v.end(), s) != v.end();
}

std::optional<std::string> opt_str(const Json& j, const char* k)
{
    if (j.contains(k) && j[k].is_string())
        return j[k].get<std::string>();
    return std::nullopt;
}

std::optional<double> opt_num(const Json& j, const char* k)
{
    if (j.contains(k) && j[k].is_number())
        return j[k].get<double>();
    return std::nullopt;
}

std::vector<std::string> str_list(const Json& j, const char* k)
{
    std::vector<std::string> out;
    if (j.contains(k) && j[k].is_array())
        for (const auto& v : j[k])
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}

Json opt_json(const std::optional<std::string>& v)
{
    return v ? Json(*v) : Json(nullptr);
}

Json opt_json(const std::optional<double>& v)
{
    return v ? Json(*v) : Json(nullptr);
}

/// int() of a token-response value.
/// @throws std::invalid_argument as int() raises ValueError/TypeError.
long long py_int(const Json& v)
{
    if (v.is_boolean())
        return v.get<bool>() ? 1 : 0;
    if (v.is_number_integer())
        return v.get<long long>();
    if (v.is_number_float())
        return static_cast<long long>(std::trunc(v.get<double>()));
    if (v.is_string())
    {
        const std::string s = detail::strip(v.get<std::string>());
        std::string digits;
        for (char c : s)
            if (c != '_')
                digits.push_back(c);
        std::size_t pos = 0;
        try
        {
            const long long n = std::stoll(digits, &pos, 10);
            if (!digits.empty() && pos == digits.size())
                return n;
        }
        catch (...)
        {
        }
        throw std::invalid_argument("invalid literal for int() with base 10: " +
                                    pu::py_repr(v.get<std::string>()));
    }
    throw std::invalid_argument("int() argument must be a string, a bytes-like object or a real "
                                "number, not '" +
                                pyjson::type_name(OJ::parse(v.dump())) + "'");
}

std::chrono::seconds secs(long long s)
{
    return std::chrono::seconds(s);
}

std::string http_url_join(const std::string& base, const std::string& path)
{
    return rstrip_slash(base) + path;
}

std::optional<std::string> header(const CustomRouteRequest& req, const std::string& name)
{
    const std::string want = detail::lower(name);
    for (const auto& [k, v] : req.headers)
        if (detail::lower(k) == want)
            return v;
    return std::nullopt;
}

/// Starlette's QueryParams.get: the last value of a repeated parameter.
std::optional<std::string> query_param(const CustomRouteRequest& req, const std::string& key)
{
    return FormData::from_query(req).get(key);
}

bool truthy(const std::optional<std::string>& v)
{
    return v && !v->empty();
}

/// The Location header value, quoted as Starlette's RedirectResponse does.
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

CustomRouteResponse redirect(const std::string& url)
{
    CustomRouteResponse r;
    r.status = 302;
    r.content_type = "";
    r.headers["location"] = quote_location(url);
    return r;
}

CustomRouteResponse html_response(const std::string& html, int status = 200)
{
    CustomRouteResponse r;
    r.status = status;
    r.body = html;
    r.content_type = "text/html; charset=utf-8";
    return r;
}

CustomRouteResponse secure_html_response(const std::string& html, int status = 200)
{
    auto r = html_response(html, status);
    for (const auto& [k, v] : ui::secure_html_headers())
        r.headers[k] = v;
    return r;
}

std::string pkce_challenge(const std::string& verifier)
{
    const auto d = crypto::sha256(verifier);
    return crypto::base64url_encode(d.data(), d.size());
}

/// A stored proxy client, with every member, as the reference stores it.
Json client_record_json(const OAuthClientInformation& c)
{
    Json j = Json::object();
    const auto put_list = [&j](const char* k, const std::optional<std::vector<std::string>>& v)
    { j[k] = v ? Json(*v) : Json(nullptr); };
    j["response_types"] = c.response_types;
    j["scope"] = opt_json(c.scope);
    j["client_name"] = opt_json(c.client_name);
    j["client_uri"] = opt_json(c.client_uri);
    j["logo_uri"] = opt_json(c.logo_uri);
    put_list("contacts", c.contacts);
    j["tos_uri"] = opt_json(c.tos_uri);
    j["policy_uri"] = opt_json(c.policy_uri);
    j["jwks_uri"] = opt_json(c.jwks_uri);
    j["jwks"] = c.jwks;
    j["software_id"] = opt_json(c.software_id);
    j["software_version"] = opt_json(c.software_version);
    put_list("redirect_uris", c.redirect_uris);
    j["token_endpoint_auth_method"] = opt_json(c.token_endpoint_auth_method);
    j["grant_types"] = c.grant_types;
    j["application_type"] = opt_json(c.application_type);
    j["client_id"] = c.client_id;
    j["client_secret"] = opt_json(c.client_secret);
    j["client_id_issued_at"] = c.client_id_issued_at ? Json(*c.client_id_issued_at) : Json(nullptr);
    j["client_secret_expires_at"] =
        c.client_secret_expires_at ? Json(*c.client_secret_expires_at) : Json(nullptr);
    j["issuer"] = opt_json(c.issuer);
    put_list("allowed_redirect_uri_patterns", c.allowed_redirect_uri_patterns);
    j["cimd_document"] =
        c.cimd_document ? Json::parse(c.cimd_document->to_json().dump()) : Json(nullptr);
    j["cimd_fetched_at"] = opt_json(c.cimd_fetched_at);
    return j;
}

OAuthClientInformation client_record_from_json(const Json& j)
{
    OAuthClientInformation c = OAuthClientInformation::from_json(j);
    c.proxy_client = true;
    if (j.contains("allowed_redirect_uri_patterns") &&
        j["allowed_redirect_uri_patterns"].is_array())
        c.allowed_redirect_uri_patterns = str_list(j, "allowed_redirect_uri_patterns");
    if (j.contains("cimd_document") && j["cimd_document"].is_object())
        c.cimd_document = std::make_shared<const CIMDDocument>(
            CIMDDocument::from_json(OJ::parse(j["cimd_document"].dump())));
    c.cimd_fetched_at = opt_num(j, "cimd_fetched_at");
    return c;
}

Json refresh_metadata_json(const std::string& client_id, const std::vector<std::string>& scopes,
                           std::optional<long long> expires_at, double created_at)
{
    Json j = Json::object();
    j["client_id"] = client_id;
    j["scopes"] = scopes;
    j["expires_at"] = expires_at ? Json(*expires_at) : Json(nullptr);
    j["created_at"] = created_at;
    return j;
}

Json jti_mapping_json(const std::string& jti, const std::string& upstream_token_id,
                      double created_at)
{
    Json j = Json::object();
    j["jti"] = jti;
    j["upstream_token_id"] = upstream_token_id;
    j["created_at"] = created_at;
    return j;
}

/// Scopes granted by an ID-JAG, from its `scope` or `scp` claim.
std::vector<std::string> assertion_granted_scopes(const Json& claims)
{
    if (claims.contains("scope") && claims["scope"].is_string())
        return detail::split_whitespace(claims["scope"].get<std::string>());
    if (claims.contains("scp"))
    {
        const auto& scp = claims["scp"];
        if (scp.is_array())
        {
            std::vector<std::string> out;
            for (const auto& s : scp)
                out.push_back(detail::py_str(s));
            return out;
        }
        if (scp.is_string())
            return detail::split_whitespace(scp.get<std::string>());
    }
    return {};
}

/// The ownership details the revocation route needs for an access token it
/// loaded, handed from load_access_token() to revoke_token() on one thread.
struct PendingRevocation
{
    std::string token;
    std::string fastmcp_jti;
    bool upstream_live = true;
};
thread_local std::optional<PendingRevocation> t_pending_revocation;

} // namespace

/// The provider behind the proxy's `/revoke` route. Token loads report the
/// downstream client that holds the FastMCP token, so the ownership check
/// compares against it, and revocation retires that token's mapping.
class ProxyRevocationAdapter : public OAuthProvider
{
  public:
    explicit ProxyRevocationAdapter(OAuthProxy& proxy)
        : OAuthProvider(
              [&proxy]()
              {
                  OAuthProviderOptions o;
                  o.base_url = proxy.base_url();
                  return o;
              }()),
          proxy_(proxy)
    {
    }

    std::optional<OAuthClientInformation> get_client(const std::string& client_id) override
    {
        return proxy_.get_client(client_id);
    }
    void register_client(const OAuthClientInformation& client_info) override
    {
        proxy_.register_client(client_info);
    }
    std::string authorize(const OAuthClientInformation& client,
                          const AuthorizationParams& params) override
    {
        return proxy_.authorize(client, params);
    }
    std::optional<AuthorizationCode> load_authorization_code(const OAuthClientInformation& client,
                                                             const std::string& code) override
    {
        return proxy_.load_authorization_code(client, code);
    }
    OAuthToken exchange_authorization_code(const OAuthClientInformation& client,
                                           const AuthorizationCode& code) override
    {
        return proxy_.exchange_authorization_code(client, code);
    }
    std::optional<RefreshToken> load_refresh_token(const OAuthClientInformation& client,
                                                   const std::string& refresh_token) override
    {
        return proxy_.load_refresh_token(client, refresh_token);
    }
    OAuthToken exchange_refresh_token(const OAuthClientInformation& client,
                                      const RefreshToken& refresh_token,
                                      const std::vector<std::string>& scopes) override
    {
        return proxy_.exchange_refresh_token(client, refresh_token, scopes);
    }
    OAuthToken exchange_identity_assertion(const OAuthClientInformation& client,
                                           const IdentityAssertionParams& params) override
    {
        return proxy_.exchange_identity_assertion(client, params);
    }

    std::optional<AccessToken> load_access_token(const std::string& token) override
    {
        t_pending_revocation.reset();
        Json payload;
        try
        {
            payload = proxy_.jwt_issuer().verify_token(token);
        }
        catch (const std::runtime_error&)
        {
            if (!proxy_.jwt_issuer_)
                throw;
            return std::nullopt;
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }
        const auto client_id = opt_str(payload, "client_id");
        const auto jti = opt_str(payload, "jti");
        if (!truthy(client_id) || !truthy(jti))
            return std::nullopt;
        std::optional<AccessToken> validated;
        try
        {
            validated = proxy_.load_access_token(token);
        }
        catch (const TokenVerificationError&)
        {
            validated.reset();
        }
        AccessToken out;
        if (!validated)
        {
            out.token = token;
            out.client_id = *client_id;
            if (payload.contains("scope") && payload["scope"].is_string())
                out.scopes = detail::split_whitespace(payload["scope"].get<std::string>());
            if (payload.contains("exp") && payload["exp"].is_number() &&
                !payload["exp"].is_boolean())
                out.expires_at =
                    static_cast<std::int64_t>(std::trunc(payload["exp"].get<double>()));
            out.claims = payload;
            t_pending_revocation = PendingRevocation{out.token, *jti, false};
            return out;
        }
        out = *validated;
        out.client_id = *client_id;
        t_pending_revocation = PendingRevocation{out.token, *jti, true};
        return out;
    }

    void revoke_token(const std::variant<AccessToken, RefreshToken>& token) override
    {
        auto pending = std::move(t_pending_revocation);
        t_pending_revocation.reset();
        if (const auto* access = std::get_if<AccessToken>(&token);
            access && pending && pending->token == access->token)
        {
            proxy_.revoke_token_impl(token, pending->fastmcp_jti, pending->upstream_live);
            return;
        }
        proxy_.revoke_token(token);
    }

  private:
    OAuthProxy& proxy_;
};

std::string hash_token(const std::string& token)
{
    return sha256_hex(token);
}

// ================================================================== UI

std::string create_consent_html(const std::string& client_id, const std::string& redirect_uri,
                                const std::vector<std::string>& scopes, const std::string& txn_id,
                                const std::string& csrf_token,
                                const std::optional<std::string>& client_name,
                                const std::string& title,
                                const std::optional<std::string>& server_name,
                                const std::optional<std::string>& server_icon_url,
                                const std::optional<std::string>& server_website_url,
                                const std::optional<std::string>& client_website_url,
                                const std::optional<std::string>& csp_policy, bool is_cimd_client,
                                const std::optional<std::string>& cimd_domain)
{
    const auto or_default = [](const std::optional<std::string>& v, const std::string& d)
    { return truthy(v) ? *v : d; };
    const std::string client_display = ui::html_escape(or_default(client_name, client_id));
    const std::string server_name_escaped = ui::html_escape(or_default(server_name, "FastMCP"));
    std::string server_display = server_name_escaped;
    if (truthy(server_website_url))
        server_display = "<a href=\"" + ui::html_escape(*server_website_url) +
                         "\" target=\"_blank\" rel=\"noopener noreferrer\" "
                         "class=\"server-name-link\">" +
                         server_name_escaped + "</a>";

    const std::string intro_box =
        "\n        <div class=\"info-box\">\n            <p>The application <strong>" +
        client_display + "</strong> wants to access the MCP server <strong>" + server_display +
        "</strong>. Please ensure you recognize the callback address below.</p>\n        "
        "</div>\n    ";

    std::string cimd_badge;
    if (is_cimd_client && truthy(cimd_domain))
        cimd_badge = "\n        <div class=\"cimd-badge\">\n            <span "
                     "class=\"cimd-check\">&#x2713;</span>\n            Verified domain: <strong>" +
                     ui::html_escape(*cimd_domain) + "</strong>\n        </div>\n        ";

    const std::string redirect_uri_escaped = ui::html_escape(redirect_uri);
    const std::string redirect_section =
        "\n        <div class=\"redirect-section\">\n            <span class=\"label\">Credentials "
        "will be sent to:</span>\n            <div class=\"value\">" +
        redirect_uri_escaped + "</div>\n        </div>\n    ";

    std::vector<std::string> escaped_scopes;
    for (const auto& s : scopes)
        escaped_scopes.push_back(ui::html_escape(s));
    const std::vector<std::pair<std::string, std::string>> detail_rows{
        {"Application Name", ui::html_escape(or_default(client_name, client_id))},
        {"Application Website", ui::html_escape(or_default(client_website_url, "N/A"))},
        {"Application ID", ui::html_escape(client_id)},
        {"Redirect URI", redirect_uri_escaped},
        {"Requested Scopes", scopes.empty() ? "None" : join(escaped_scopes, ", ")},
    };
    std::string detail_rows_html;
    for (std::size_t i = 0; i < detail_rows.size(); ++i)
    {
        if (i)
            detail_rows_html += "\n";
        detail_rows_html += "\n        <div class=\"detail-row\">\n            <div "
                            "class=\"detail-label\">" +
                            detail_rows[i].first +
                            ":</div>\n            <div class=\"detail-value\">" +
                            detail_rows[i].second + "</div>\n        </div>\n        ";
    }
    const std::string advanced_details =
        "\n        <details>\n            <summary>Advanced Details</summary>\n            <div "
        "class=\"detail-box\">\n                " +
        detail_rows_html + "\n            </div>\n        </details>\n    ";

    const std::string form =
        "\n        <form id=\"consentForm\" method=\"POST\" action=\"\">\n            <input "
        "type=\"hidden\" name=\"txn_id\" value=\"" +
        txn_id + "\" />\n            <input type=\"hidden\" name=\"csrf_token\" value=\"" +
        csrf_token +
        "\" />\n            <input type=\"hidden\" name=\"submit\" value=\"true\" />\n"
        "            <div class=\"button-group\">\n                <button type=\"submit\" "
        "name=\"action\" value=\"approve\" class=\"btn-approve\">Allow Access</button>\n"
        "                <button type=\"submit\" name=\"action\" value=\"deny\" "
        "class=\"btn-deny\">Deny</button>\n            </div>\n        </form>\n    ";

    const std::string help_link =
        "\n        <div class=\"help-link-container\">\n            <span class=\"help-link\">\n"
        "                Why am I seeing this?\n                <span class=\"tooltip\">\n"
        "                    This FastMCP server requires your consent to allow a new client\n"
        "                    to connect. This protects you from <a\n"
        "                    href=\"https://modelcontextprotocol.io/specification/2025-06-18/"
        "basic/security_best_practices#confused-deputy-problem\"\n"
        "                    target=\"_blank\" class=\"tooltip-link\">confused deputy\n"
        "                    attacks</a>, where malicious clients could impersonate you\n"
        "                    and steal access.<br><br>\n                    <a\n"
        "                    href=\"https://gofastmcp.com/servers/auth/"
        "oauth-proxy#confused-deputy-attacks\"\n"
        "                    target=\"_blank\" class=\"tooltip-link\">Learn more about\n"
        "                    FastMCP security \xE2\x86\x92</a>\n                </span>\n"
        "            </span>\n        </div>\n    ";

    const std::string content =
        "\n        <div class=\"container\">\n            " +
        ui::create_logo(server_icon_url, or_default(server_name, "FastMCP")) +
        "\n            <h1>Application Access Request</h1>\n            " + intro_box +
        "\n            " + cimd_badge + "\n            " + redirect_section + "\n            " +
        advanced_details + "\n            " + form + "\n        </div>\n        " + help_link +
        "\n    ";

    const std::string cimd_badge_styles = "\n        .cimd-badge {\n"
                                          "            background: #ecfdf5;\n"
                                          "            border: 1px solid #6ee7b7;\n"
                                          "            border-radius: 8px;\n"
                                          "            padding: 8px 16px;\n"
                                          "            margin-bottom: 16px;\n"
                                          "            font-size: 14px;\n"
                                          "            color: #065f46;\n"
                                          "            text-align: center;\n"
                                          "        }\n"
                                          "        .cimd-check {\n"
                                          "            color: #059669;\n"
                                          "            font-weight: bold;\n"
                                          "            margin-right: 4px;\n"
                                          "        }\n    ";
    const std::string additional_styles =
        std::string(ui::INFO_BOX_STYLES) + ui::REDIRECT_SECTION_STYLES + ui::DETAILS_STYLES +
        ui::DETAIL_BOX_STYLES + ui::BUTTON_STYLES + ui::TOOLTIP_STYLES + cimd_badge_styles;
    return ui::create_page(content, title, additional_styles,
                           csp_policy ? *csp_policy
                                      : std::string("default-src 'none'; style-src "
                                                    "'unsafe-inline'; img-src https: data:; "
                                                    "base-uri 'none'"));
}

std::string create_error_html(const std::string& error_title, const std::string& error_message,
                              const std::vector<std::pair<std::string, std::string>>& error_details,
                              const std::optional<std::string>& server_name,
                              const std::optional<std::string>& server_icon_url)
{
    const std::string error_box = "\n        <div class=\"info-box error\">\n            <p>" +
                                  ui::html_escape(error_message) + "</p>\n        </div>\n    ";
    std::string details_section;
    if (!error_details.empty())
    {
        std::string rows;
        for (std::size_t i = 0; i < error_details.size(); ++i)
        {
            if (i)
                rows += "\n";
            rows += "\n            <div class=\"detail-row\">\n                <div "
                    "class=\"detail-label\">" +
                    ui::html_escape(error_details[i].first) +
                    ":</div>\n                <div class=\"detail-value\">" +
                    ui::html_escape(error_details[i].second) +
                    "</div>\n            </div>\n            ";
        }
        details_section = "\n            <details>\n                <summary>Error "
                          "Details</summary>\n                <div class=\"detail-box\">\n"
                          "                    " +
                          rows + "\n                </div>\n            </details>\n        ";
    }
    const std::string content =
        "\n        <div class=\"container\">\n            " +
        ui::create_logo(server_icon_url, truthy(server_name) ? *server_name : "FastMCP") +
        "\n            <h1>" + ui::html_escape(error_title) + "</h1>\n            " + error_box +
        "\n            " + details_section + "\n        </div>\n    ";
    const std::string additional_styles = std::string(ui::INFO_BOX_STYLES) + ui::DETAILS_STYLES +
                                          ui::DETAIL_BOX_STYLES +
                                          "\n        .info-box.error {\n"
                                          "            color: #111827;\n"
                                          "        }\n        ";
    return ui::create_page(
        content, error_title, additional_styles,
        "default-src 'none'; style-src 'unsafe-inline'; img-src https: data:; base-uri 'none'");
}

// ================================================================== models

Json OAuthTransaction::to_json() const
{
    Json j = Json::object();
    j["txn_id"] = txn_id;
    j["client_id"] = client_id;
    j["client_redirect_uri"] = client_redirect_uri;
    j["client_state"] = client_state;
    j["code_challenge"] = opt_json(code_challenge);
    j["code_challenge_method"] = code_challenge_method;
    j["scopes"] = scopes;
    j["created_at"] = created_at;
    j["resource"] = opt_json(resource);
    j["proxy_code_verifier"] = opt_json(proxy_code_verifier);
    j["csrf_token"] = opt_json(csrf_token);
    j["csrf_expires_at"] = opt_json(csrf_expires_at);
    j["consent_token"] = opt_json(consent_token);
    return j;
}

OAuthTransaction OAuthTransaction::from_json(const Json& j)
{
    OAuthTransaction t;
    t.txn_id = j.at("txn_id").get<std::string>();
    t.client_id = j.at("client_id").get<std::string>();
    t.client_redirect_uri = j.at("client_redirect_uri").get<std::string>();
    t.client_state = j.at("client_state").get<std::string>();
    t.code_challenge = opt_str(j, "code_challenge");
    t.code_challenge_method = j.value("code_challenge_method", std::string("S256"));
    t.scopes = str_list(j, "scopes");
    t.created_at = j.at("created_at").get<double>();
    t.resource = opt_str(j, "resource");
    t.proxy_code_verifier = opt_str(j, "proxy_code_verifier");
    t.csrf_token = opt_str(j, "csrf_token");
    t.csrf_expires_at = opt_num(j, "csrf_expires_at");
    t.consent_token = opt_str(j, "consent_token");
    return t;
}

Json ClientCode::to_json() const
{
    Json j = Json::object();
    j["code"] = code;
    j["client_id"] = client_id;
    j["redirect_uri"] = redirect_uri;
    j["code_challenge"] = opt_json(code_challenge);
    j["code_challenge_method"] = code_challenge_method;
    j["scopes"] = scopes;
    j["idp_tokens"] = idp_tokens;
    j["expires_at"] = expires_at;
    j["created_at"] = created_at;
    return j;
}

ClientCode ClientCode::from_json(const Json& j)
{
    ClientCode c;
    c.code = j.at("code").get<std::string>();
    c.client_id = j.at("client_id").get<std::string>();
    c.redirect_uri = j.at("redirect_uri").get<std::string>();
    c.code_challenge = opt_str(j, "code_challenge");
    c.code_challenge_method = j.value("code_challenge_method", std::string("S256"));
    c.scopes = str_list(j, "scopes");
    c.idp_tokens = j.at("idp_tokens");
    c.expires_at = j.at("expires_at").get<double>();
    c.created_at = j.at("created_at").get<double>();
    return c;
}

Json UpstreamTokenSet::to_json() const
{
    Json j = Json::object();
    j["upstream_token_id"] = upstream_token_id;
    j["access_token"] = access_token;
    j["refresh_token"] = opt_json(refresh_token);
    j["refresh_token_expires_at"] = opt_json(refresh_token_expires_at);
    j["expires_at"] = expires_at;
    j["token_type"] = token_type;
    j["scope"] = scope;
    j["client_id"] = client_id;
    j["created_at"] = created_at;
    j["raw_token_data"] = raw_token_data;
    j["upstream_claims"] = upstream_claims;
    return j;
}

UpstreamTokenSet UpstreamTokenSet::from_json(const Json& j)
{
    UpstreamTokenSet t;
    t.upstream_token_id = j.at("upstream_token_id").get<std::string>();
    t.access_token = j.at("access_token").get<std::string>();
    t.refresh_token = opt_str(j, "refresh_token");
    t.refresh_token_expires_at = opt_num(j, "refresh_token_expires_at");
    t.expires_at = j.at("expires_at").get<double>();
    t.token_type = j.value("token_type", std::string("Bearer"));
    t.scope = j.at("scope").get<std::string>();
    t.client_id = j.at("client_id").get<std::string>();
    t.created_at = j.at("created_at").get<double>();
    if (j.contains("raw_token_data") && j["raw_token_data"].is_object())
        t.raw_token_data = j["raw_token_data"];
    if (j.contains("upstream_claims"))
        t.upstream_claims = j["upstream_claims"];
    return t;
}

// ================================================================== construction

OAuthProxy::OAuthProxy(OAuthProxyOptions options)
    : OAuthProvider(
          [&options]()
          {
              if (!options.token_verifier)
                  throw std::invalid_argument("token_verifier is required");
              const auto default_scopes = options.valid_scopes && !options.valid_scopes->empty()
                                              ? *options.valid_scopes
                                              : options.token_verifier->required_scopes();
              ClientRegistrationOptions reg;
              reg.enabled = true;
              reg.valid_scopes = default_scopes;
              reg.default_scopes = default_scopes;
              OAuthProviderOptions o;
              if (!options.base_url.empty())
                  o.base_url = options.base_url;
              o.resource_base_url = options.resource_base_url;
              o.issuer_url = options.issuer_url;
              o.service_documentation_url = options.service_documentation_url;
              o.client_registration_options = reg;
              if (truthy(options.upstream_revocation_endpoint))
                  o.revocation_options = RevocationOptions{true};
              o.required_scopes = options.token_verifier->required_scopes();
              return o;
          }()),
      options_(std::move(options)), clock_(detail::now_seconds)
{
    default_scope_str_ =
        join(client_registration_options_->valid_scopes.value_or(std::vector<std::string>{}), " ");

    if (!options_.redirect_path || options_.redirect_path->empty())
        redirect_path_ = "/auth/callback";
    else
        redirect_path_ = (*options_.redirect_path)[0] == '/' ? *options_.redirect_path
                                                             : "/" + *options_.redirect_path;

    static const std::set<std::string> kConsentModes{"true", "false", "remember", "external"};
    if (!kConsentModes.count(options_.require_authorization_consent))
        throw std::invalid_argument(
            "require_authorization_consent must be \"true\", \"false\", \"remember\" or "
            "\"external\"");

    if (options_.client_registration_ttl_seconds && *options_.client_registration_ttl_seconds <= 0)
        throw std::invalid_argument("client_registration_ttl_seconds must be positive");
    if (!options_.fallback_refresh_token_expiry_seconds)
        options_.fallback_refresh_token_expiry_seconds = DEFAULT_REFRESH_TOKEN_EXPIRY_SECONDS;

    if (options_.jwt_signing_key_bytes)
        jwt_signing_key_ = *options_.jwt_signing_key_bytes;
    else if (options_.jwt_signing_key)
        jwt_signing_key_ =
            derive_jwt_key(std::nullopt, *options_.jwt_signing_key, "fastmcp-jwt-signing-key");
    else
    {
        if (!options_.upstream_client_secret)
            throw std::invalid_argument(
                "jwt_signing_key is required when upstream_client_secret is not provided. The JWT "
                "signing key cannot be derived without a client secret.");
        jwt_signing_key_ = derive_jwt_key(*options_.upstream_client_secret, std::nullopt,
                                          "fastmcp-jwt-signing-key");
    }

    if (!options_.http)
        options_.http =
            client::auth::default_http_function(true, std::chrono::seconds(HTTP_TIMEOUT_SECONDS));

    client_storage_ = options_.client_storage;
    if (!client_storage_)
    {
        const std::string storage_key =
            derive_jwt_key(jwt_signing_key_, std::nullopt, "fastmcp-storage-encryption-key");
        const std::string fingerprint = sha256_hex(storage_key).substr(0, 12);
        const auto dir =
            std::filesystem::path(Settings::from_env().home) / "oauth-proxy" / fingerprint;
        std::filesystem::create_directories(dir);
        default_file_store_ = std::make_shared<client::auth::FileTreeStore>(dir);
        client_storage_ = std::make_shared<client::auth::FernetEncryptionWrapper>(
            default_file_store_, storage_key, false);
    }

    if (options_.enable_cimd)
        cimd_manager_ = std::make_shared<CIMDClientManager>(
            true, default_scope_str_, options_.allowed_client_redirect_uris,
            client_registration_options_->valid_scopes);

    if (options_.identity_assertion)
        identity_assertion_validator_ =
            std::make_unique<IdentityAssertionValidator>(*options_.identity_assertion, issuer_url_);
}

OAuthProxy::~OAuthProxy()
{
    if (cull_task_.valid())
        cull_task_.wait();
}

double OAuthProxy::now() const
{
    return clock_();
}

void OAuthProxy::set_clock(std::function<double()> clock)
{
    clock_ = std::move(clock);
}

std::string OAuthProxy::default_storage_directory() const
{
    return default_file_store_ ? default_file_store_->data_directory().string() : std::string();
}

void OAuthProxy::maybe_cull_expired_state()
{
    if (!default_file_store_)
        return;
    std::lock_guard<std::mutex> lock(mu_);
    if (cull_task_.valid() &&
        cull_task_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (last_cull_started_ && t - *last_cull_started_ < kStateCullIntervalSeconds)
        return;
    last_cull_started_ = t;
    auto store = default_file_store_;
    cull_task_ = std::async(std::launch::async,
                            [store]()
                            {
                                try
                                {
                                    store->cull();
                                }
                                catch (...)
                                {
                                }
                            });
}

void OAuthProxy::update_default_scopes(const std::vector<std::string>& scopes)
{
    default_scope_str_ = join(scopes, " ");
    if (cimd_manager_)
    {
        cimd_manager_->default_scope = default_scope_str_;
        cimd_manager_->valid_scopes = scopes;
    }
    if (client_registration_options_)
    {
        client_registration_options_->default_scopes = scopes;
        client_registration_options_->valid_scopes = scopes;
    }
}

void OAuthProxy::set_server_info(std::optional<std::string> name,
                                 std::optional<std::string> icon_url,
                                 std::optional<std::string> website_url)
{
    server_name_ = std::move(name);
    server_icon_url_ = std::move(icon_url);
    server_website_url_ = std::move(website_url);
}

void OAuthProxy::set_mcp_path(const std::optional<std::string>& mcp_path)
{
    OAuthProvider::set_mcp_path(mcp_path);
    resource_url_ = resource_url(mcp_path);
    jwt_issuer_ =
        std::make_unique<JWTIssuer>(issuer_url_, resource_url_.value_or("None"), jwt_signing_key_);
}

const JWTIssuer& OAuthProxy::jwt_issuer() const
{
    if (!jwt_issuer_)
        throw std::runtime_error(
            "JWT issuer not initialized. Ensure get_routes() is called before token operations.");
    return *jwt_issuer_;
}

std::string OAuthProxy::token_endpoint_url() const
{
    return rstrip_slash(*base_url_) + "/token";
}

std::shared_ptr<UpstreamOAuthClient> OAuthProxy::create_upstream_oauth_client()
{
    return std::make_shared<UpstreamOAuthClient>(options_.upstream_client_id,
                                                 options_.upstream_client_secret,
                                                 options_.token_endpoint_auth_method, options_.http,
                                                 std::chrono::seconds(HTTP_TIMEOUT_SECONDS));
}

std::vector<std::string>
OAuthProxy::prepare_scopes_for_token_exchange(const std::vector<std::string>&)
{
    return {};
}

std::vector<std::string>
OAuthProxy::translate_scopes_from_idp(const std::vector<std::string>& scopes)
{
    return scopes;
}

std::vector<std::string>
OAuthProxy::prepare_scopes_for_upstream_refresh(const std::vector<std::string>& scopes)
{
    return scopes;
}

std::optional<Json> OAuthProxy::extract_upstream_claims(const Json&)
{
    return std::nullopt;
}

std::optional<std::string> OAuthProxy::get_verification_token(const UpstreamTokenSet& tokens)
{
    return tokens.access_token;
}

bool OAuthProxy::uses_alternate_verification() const
{
    return false;
}

std::shared_ptr<std::mutex> OAuthProxy::refresh_lock(const std::string& token_id)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (auto it = refresh_locks_.find(token_id); it != refresh_locks_.end())
    {
        refresh_lock_order_.splice(refresh_lock_order_.end(), refresh_lock_order_,
                                   it->second.second);
        return it->second.first;
    }
    auto m = std::make_shared<std::mutex>();
    refresh_lock_order_.push_back(token_id);
    refresh_locks_[token_id] = {m, std::prev(refresh_lock_order_.end())};
    if (refresh_locks_.size() > kRefreshLockCacheSize)
    {
        refresh_locks_.erase(refresh_lock_order_.front());
        refresh_lock_order_.pop_front();
    }
    return m;
}

std::string OAuthProxy::build_upstream_authorize_url(const std::string& txn_id,
                                                     const OAuthTransaction& txn) const
{
    std::vector<std::pair<std::string, std::string>> q{
        {"response_type", "code"},
        {"client_id", options_.upstream_client_id},
        {"redirect_uri", http_url_join(*base_url_, redirect_path_)},
        {"state", txn_id},
    };
    const auto set = [&q](const std::string& k, const std::string& v)
    {
        for (auto& [qk, qv] : q)
            if (qk == k)
            {
                qv = v;
                return;
            }
        q.emplace_back(k, v);
    };
    const auto& scopes = !txn.scopes.empty() ? txn.scopes : required_scopes_;
    if (!scopes.empty())
        set("scope", join(scopes, " "));
    if (truthy(txn.proxy_code_verifier))
    {
        set("code_challenge", pkce_challenge(*txn.proxy_code_verifier));
        set("code_challenge_method", "S256");
    }
    if (options_.forward_resource && truthy(txn.resource))
        set("resource", *txn.resource);
    for (const auto& [k, v] : options_.extra_authorize_params)
        set(k, v);
    const auto& ep = options_.upstream_authorization_endpoint;
    return ep + (ep.find('?') != std::string::npos ? "&" : "?") + pu::urlencode(q);
}

bool OAuthProxy::resource_matches(const std::string& requested) const
{
    const std::string& server_url = *resource_url_;
    if (server_url_has_query(server_url))
        return rstrip_slash(requested) == rstrip_slash(server_url);
    return normalize_resource_url(requested) == normalize_resource_url(server_url);
}

std::optional<OAuthTransaction> OAuthProxy::load_transaction(const std::string& txn_id)
{
    if (const auto j = client_storage_->get(kTransactions, txn_id))
        return OAuthTransaction::from_json(*j);
    return std::nullopt;
}

// ================================================================== clients

std::optional<OAuthClientInformation> OAuthProxy::get_client(const std::string& client_id)
{
    std::optional<OAuthClientInformation> client;
    if (const auto j = client_storage_->get(kClients, client_id))
        client = client_record_from_json(*j);

    if (client)
    {
        if (options_.allowed_client_redirect_uris)
            client->allowed_redirect_uri_patterns = options_.allowed_client_redirect_uris;
        if (!client->cimd_document)
            return client;
    }

    if (cimd_manager_ && cimd_manager_->is_cimd_client_id(client_id))
    {
        if (auto cimd_client = cimd_manager_->get_client(client_id))
        {
            if (client)
                client_storage_->remove(kClients, client_id);
            return cimd_client;
        }
        if (client)
        {
            if (client->cimd_document)
                client->scope = cimd_manager_->restrict_scope(client->scope);
            return client;
        }
    }

    if (client_id == options_.upstream_client_id)
    {
        OAuthClientInformation c;
        c.client_id = client_id;
        c.redirect_uris = std::vector<std::string>{"http://localhost/"};
        c.grant_types = {"authorization_code", "refresh_token"};
        if (options_.identity_assertion)
            c.grant_types.push_back(JWT_BEARER_GRANT_TYPE);
        c.scope = default_scope_str_;
        c.token_endpoint_auth_method = "none";
        c.proxy_client = true;
        c.allowed_redirect_uri_patterns = options_.allowed_client_redirect_uris;
        c.allow_unregistered_redirect_uris = true;
        return c;
    }
    return std::nullopt;
}

void OAuthProxy::register_client(const OAuthClientInformation& client_info)
{
    if (client_info.client_id.empty())
        throw std::invalid_argument("client_id is required for client registration");
    const std::string application_type = client_info.application_type.value_or("native");

    if (client_info.redirect_uris && !client_info.redirect_uris->empty())
    {
        for (const auto& uri : *client_info.redirect_uris)
        {
            if (!validate_redirect_uri(uri, options_.allowed_client_redirect_uris))
                throw RegistrationError("invalid_redirect_uri",
                                        "Redirect URI '" + uri + "' is not allowed.");
            if (!is_redirect_uri_allowed_for_application_type(uri, application_type))
                throw RegistrationError("invalid_redirect_uri",
                                        "Redirect URI '" + uri +
                                            "' is not allowed for application_type '" +
                                            application_type + "'.");
        }
    }
    else if (application_type == "web")
        throw RegistrationError("invalid_redirect_uri",
                                "redirect_uris is required for application_type 'web'; web clients "
                                "must register a non-loopback https redirect URI.");

    OAuthClientInformation stored;
    stored.client_id = client_info.client_id;
    stored.redirect_uris = client_info.redirect_uris && !client_info.redirect_uris->empty()
                               ? *client_info.redirect_uris
                               : std::vector<std::string>{"http://localhost/"};
    stored.grant_types = client_info.grant_types.empty()
                             ? std::vector<std::string>{"authorization_code", "refresh_token"}
                             : client_info.grant_types;
    if (options_.identity_assertion && !contains(stored.grant_types, JWT_BEARER_GRANT_TYPE))
        stored.grant_types.push_back(JWT_BEARER_GRANT_TYPE);
    stored.scope = truthy(client_info.scope) ? *client_info.scope : default_scope_str_;
    stored.token_endpoint_auth_method = "none";
    stored.application_type = application_type;
    stored.allowed_redirect_uri_patterns = options_.allowed_client_redirect_uris;
    stored.client_name = client_info.client_name;

    std::optional<std::chrono::seconds> ttl;
    if (options_.client_registration_ttl_seconds)
        ttl = secs(*options_.client_registration_ttl_seconds);
    client_storage_->put(kClients, client_info.client_id, client_record_json(stored), ttl);
}

void OAuthProxy::renew_client_registration(const OAuthClientInformation& client)
{
    if (!options_.client_registration_ttl_seconds || client.client_id.empty())
        return;
    const auto stored = client_storage_->get(kClients, client.client_id);
    if (!stored || (stored->contains("cimd_document") && !(*stored)["cimd_document"].is_null()))
        return;
    client_storage_->put(kClients, client.client_id, *stored,
                         secs(*options_.client_registration_ttl_seconds));
}

// ================================================================== authorization

std::string OAuthProxy::authorize(const OAuthClientInformation& client,
                                  const AuthorizationParams& params)
{
    maybe_cull_expired_state();
    if (truthy(params.resource) && resource_url_ && !resource_matches(*params.resource))
        throw AuthorizeError("invalid_target", "Resource does not match this server");

    const std::string txn_id = token_urlsafe(32);
    std::optional<std::string> proxy_code_verifier;
    if (options_.forward_pkce && !params.code_challenge.empty())
        proxy_code_verifier = token_urlsafe(36);

    if (client.client_id.empty())
        throw AuthorizeError("invalid_client", "Client ID is required");

    OAuthTransaction txn;
    txn.txn_id = txn_id;
    txn.client_id = client.client_id;
    txn.client_redirect_uri = params.redirect_uri;
    txn.client_state = params.state.value_or("");
    txn.code_challenge = params.code_challenge;
    txn.code_challenge_method = "S256";
    txn.scopes = params.scopes && !params.scopes->empty() ? *params.scopes : required_scopes_;
    txn.created_at = now();
    txn.resource = params.resource;
    txn.proxy_code_verifier = proxy_code_verifier;
    client_storage_->put(kTransactions, txn_id, txn.to_json(), secs(kTransactionTtl));

    const auto& mode = options_.require_authorization_consent;
    if (mode == "false" || mode == "external")
        return build_upstream_authorize_url(txn_id, txn);
    return rstrip_slash(*base_url_) + "/consent?txn_id=" + txn_id;
}

std::optional<AuthorizationCode>
OAuthProxy::load_authorization_code(const OAuthClientInformation& client, const std::string& code)
{
    const auto j = client_storage_->get(kCodes, code);
    if (!j)
        return std::nullopt;
    const auto model = ClientCode::from_json(*j);
    if (now() > model.expires_at)
    {
        client_storage_->remove(kCodes, code);
        return std::nullopt;
    }
    if (model.client_id != client.client_id)
        return std::nullopt;
    if (client.client_id.empty())
        throw AuthorizeError("invalid_client", "Client ID is required");
    AuthorizationCode ac;
    ac.code = code;
    ac.client_id = client.client_id;
    ac.redirect_uri = model.redirect_uri;
    ac.redirect_uri_provided_explicitly = true;
    ac.scopes = model.scopes;
    ac.expires_at = model.expires_at;
    ac.code_challenge = model.code_challenge.value_or("");
    return ac;
}

OAuthToken OAuthProxy::exchange_authorization_code(const OAuthClientInformation& client,
                                                   const AuthorizationCode& authorization_code)
{
    maybe_cull_expired_state();
    const auto j = client_storage_->get(kCodes, authorization_code.code);
    if (!j)
        throw TokenError("invalid_grant", "Authorization code not found");
    const auto code_model = ClientCode::from_json(*j);
    const Json& idp_tokens = code_model.idp_tokens;

    if (idp_tokens.contains("expires_in") && py_int(idp_tokens["expires_in"]) <= 0)
        throw TokenError("invalid_grant", "Upstream access token has a non-positive expires_in");

    std::vector<std::string> granted_scopes =
        idp_tokens.contains("scope")
            ? parse_scopes(idp_tokens["scope"]).value_or(std::vector<std::string>{})
            : authorization_code.scopes;
    granted_scopes = translate_scopes_from_idp(granted_scopes);

    if (!client_storage_->erase(kCodes, authorization_code.code))
        throw TokenError("invalid_grant", "Authorization code already used");

    const bool has_refresh =
        idp_tokens.contains("refresh_token") && detail::truthy(idp_tokens["refresh_token"]);
    const std::string upstream_token_id = token_urlsafe(32);
    const std::string access_jti = token_urlsafe(32);
    const std::optional<std::string> refresh_jti =
        has_refresh ? std::optional<std::string>(token_urlsafe(32)) : std::nullopt;

    long long expires_in;
    if (idp_tokens.contains("expires_in"))
        expires_in = py_int(idp_tokens["expires_in"]);
    else if (options_.fallback_access_token_expiry_seconds)
        expires_in = *options_.fallback_access_token_expiry_seconds;
    else if (has_refresh)
        expires_in = DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS;
    else
        expires_in = DEFAULT_ACCESS_TOKEN_EXPIRY_NO_REFRESH_SECONDS;

    long long fastmcp_access_expires_in = expires_in;
    if (options_.fastmcp_access_token_expiry_seconds)
    {
        fastmcp_access_expires_in = *options_.fastmcp_access_token_expiry_seconds;
        if (!has_refresh)
            fastmcp_access_expires_in = std::min(fastmcp_access_expires_in, expires_in);
    }

    std::optional<long long> refresh_expires_in;
    std::optional<double> refresh_token_expires_at;
    if (has_refresh)
    {
        if (idp_tokens.contains("refresh_expires_in") && py_int(idp_tokens["refresh_expires_in"]))
            refresh_expires_in = py_int(idp_tokens["refresh_expires_in"]);
        else
            refresh_expires_in = *options_.fallback_refresh_token_expiry_seconds;
        refresh_token_expires_at = now() + static_cast<double>(*refresh_expires_in);
    }

    UpstreamTokenSet set;
    set.upstream_token_id = upstream_token_id;
    set.access_token = idp_tokens.at("access_token").get<std::string>();
    if (has_refresh)
        set.refresh_token = idp_tokens["refresh_token"].get<std::string>();
    set.refresh_token_expires_at = refresh_token_expires_at;
    set.expires_at = now() + static_cast<double>(expires_in);
    set.token_type = idp_tokens.contains("token_type") ? idp_tokens["token_type"].get<std::string>()
                                                       : std::string("Bearer");
    set.scope = join(granted_scopes, " ");
    set.client_id = client.client_id;
    set.created_at = now();
    set.raw_token_data = idp_tokens;
    client_storage_->put(kUpstreamTokens, upstream_token_id, set.to_json(),
                         secs(std::max({refresh_expires_in.value_or(0), expires_in,
                                        fastmcp_access_expires_in, 1LL})));

    const auto upstream_claims = extract_upstream_claims(idp_tokens);

    if (client.client_id.empty())
        throw TokenError("invalid_client", "Client ID is required");
    const std::string access_token = jwt_issuer().issue_access_token(
        client.client_id, granted_scopes, access_jti, fastmcp_access_expires_in, upstream_claims);

    std::optional<std::string> refresh_token;
    if (refresh_jti && refresh_expires_in && *refresh_expires_in)
        refresh_token = jwt_issuer().issue_refresh_token(
            client.client_id, granted_scopes, *refresh_jti, *refresh_expires_in, upstream_claims);

    client_storage_->put(kJtiMappings, access_jti,
                         jti_mapping_json(access_jti, upstream_token_id, now()),
                         secs(fastmcp_access_expires_in));
    if (refresh_jti)
        client_storage_->put(kJtiMappings, *refresh_jti,
                             jti_mapping_json(*refresh_jti, upstream_token_id, now()),
                             secs(refresh_expires_in && *refresh_expires_in
                                      ? *refresh_expires_in
                                      : *options_.fallback_refresh_token_expiry_seconds));

    if (refresh_token && refresh_expires_in && *refresh_expires_in)
        client_storage_->put(
            kRefreshTokens, hash_token(*refresh_token),
            refresh_metadata_json(client.client_id, granted_scopes,
                                  static_cast<long long>(now()) + *refresh_expires_in, now()),
            secs(*refresh_expires_in));

    renew_client_registration(client);

    OAuthToken out;
    out.access_token = access_token;
    out.token_type = "Bearer";
    out.expires_in = fastmcp_access_expires_in;
    out.refresh_token = refresh_token;
    out.scope = join(granted_scopes, " ");
    return out;
}

OAuthToken OAuthProxy::exchange_identity_assertion(const OAuthClientInformation& client,
                                                   const IdentityAssertionParams& params)
{
    if (!options_.identity_assertion || !identity_assertion_validator_)
        throw TokenError("unsupported_grant_type",
                         "The JWT bearer grant is not supported by this authorization server");

    if (truthy(params.resource) && resource_url_ && !resource_matches(*params.resource))
        throw TokenError("invalid_target", "Resource does not match this server");

    Json claims;
    try
    {
        claims = identity_assertion_validator_->validate(params.assertion, client.client_id,
                                                         resource_url_);
    }
    catch (const IdentityAssertionError& e)
    {
        std::cerr << "[fastmcpp] ID-JAG rejected: " << e.what() << "\n";
        throw TokenError("invalid_grant", "Invalid identity assertion");
    }

    const std::string subject = detail::py_str(claims.at("sub"));
    const auto& ia = *options_.identity_assertion;
    std::vector<std::string> authoritative = assertion_granted_scopes(claims);
    if (authoritative.empty())
        authoritative = ia.required_scopes.value_or(std::vector<std::string>{});
    const std::vector<std::string> required =
        ia.required_scopes.value_or(std::vector<std::string>{});

    std::vector<std::string> granted;
    if (params.scopes && !params.scopes->empty())
    {
        for (const auto& s : authoritative)
            if (contains(required, s) || contains(*params.scopes, s))
                granted.push_back(s);
    }
    else
        granted = authoritative;

    const long long expires_in = ia.access_token_expiry_seconds;
    const std::string access_jti = token_urlsafe(32);
    Json extra = Json::object();
    extra["fastmcp_grant"] = kIdJagGrantMarker;
    extra["assertion_iss"] = claims.contains("iss") ? detail::py_str(claims["iss"]) : "None";
    const std::string access_token = jwt_issuer().issue_access_token(
        client.client_id, granted, access_jti, expires_in, std::nullopt, subject, extra);

    renew_client_registration(client);

    OAuthToken out;
    out.access_token = access_token;
    out.token_type = "Bearer";
    out.expires_in = expires_in;
    out.scope = join(granted, " ");
    return out;
}

// ================================================================== refresh

std::optional<RefreshToken> OAuthProxy::load_refresh_token(const OAuthClientInformation& client,
                                                           const std::string& refresh_token)
{
    const std::string token_hash = hash_token(refresh_token);
    const auto metadata = client_storage_->get(kRefreshTokens, token_hash);
    if (!metadata)
    {
        end_grant_on_refresh_reuse(client, token_hash);
        return std::nullopt;
    }
    const std::string owner = metadata->at("client_id").get<std::string>();
    if (owner != client.client_id)
        return std::nullopt;
    RefreshToken rt;
    rt.token = refresh_token;
    rt.client_id = owner;
    rt.scopes = str_list(*metadata, "scopes");
    if (metadata->contains("expires_at") && (*metadata)["expires_at"].is_number())
        rt.expires_at = (*metadata)["expires_at"].get<std::int64_t>();
    return rt;
}

void OAuthProxy::end_grant(const std::string& upstream_token_id)
{
    Json ended = Json::object();
    ended["ended_at"] = now();
    client_storage_->put(kEndedGrants, upstream_token_id, ended,
                         secs(*options_.fallback_refresh_token_expiry_seconds));
    client_storage_->remove(kUpstreamTokens, upstream_token_id);
}

bool OAuthProxy::store_upstream_token_set(const UpstreamTokenSet& tokens, long long ttl)
{
    client_storage_->put(kUpstreamTokens, tokens.upstream_token_id, tokens.to_json(), secs(ttl));
    if (client_storage_->get(kEndedGrants, tokens.upstream_token_id))
    {
        client_storage_->remove(kUpstreamTokens, tokens.upstream_token_id);
        return false;
    }
    return true;
}

void OAuthProxy::end_grant_on_refresh_reuse(const OAuthClientInformation& client,
                                            const std::string& token_hash)
{
    const auto rotated = client_storage_->get(kRotatedRefresh, token_hash);
    if (!rotated || rotated->value("client_id", std::string()) != client.client_id)
        return;
    if (now() - rotated->at("rotated_at").get<double>() <= kRefreshReuseGraceSeconds)
        return;
    const std::string upstream_token_id = rotated->at("upstream_token_id").get<std::string>();
    if (const auto j = client_storage_->get(kUpstreamTokens, upstream_token_id))
    {
        const auto set = UpstreamTokenSet::from_json(*j);
        if (truthy(set.refresh_token))
            revoke_upstream(*set.refresh_token);
    }
    end_grant(upstream_token_id);
    std::cerr << "[fastmcpp] Rotated refresh token reused for client=" << client.client_id
              << "; ended its grant\n";
}

OAuthToken OAuthProxy::exchange_refresh_token(const OAuthClientInformation& client,
                                              const RefreshToken& refresh_token,
                                              const std::vector<std::string>& scopes)
{
    maybe_cull_expired_state();
    std::string refresh_jti;
    try
    {
        const Json payload = jwt_issuer().verify_token(refresh_token.token, "refresh");
        refresh_jti = payload.at("jti").get<std::string>();
    }
    catch (const std::exception&)
    {
        throw TokenError("invalid_grant", "Invalid refresh token");
    }

    const auto mapping = client_storage_->get(kJtiMappings, refresh_jti);
    if (!mapping)
        throw TokenError("invalid_grant", "Refresh token mapping not found");
    const auto upstream_json =
        client_storage_->get(kUpstreamTokens, mapping->at("upstream_token_id").get<std::string>());
    if (!upstream_json)
        throw TokenError("invalid_grant", "Upstream token not found");
    UpstreamTokenSet set = UpstreamTokenSet::from_json(*upstream_json);
    if (!truthy(set.refresh_token))
        throw TokenError("invalid_grant", "Refresh not supported for this token");

    const auto upstream_scopes = prepare_scopes_for_upstream_refresh(scopes);
    Json response;
    try
    {
        TokenParams params;
        if (!upstream_scopes.empty())
            params.emplace_back("scope", join(upstream_scopes, " "));
        for (const auto& kv : options_.extra_token_params)
            params.push_back(kv);
        response = create_upstream_oauth_client()->refresh_token(options_.upstream_token_endpoint,
                                                                 set.refresh_token, params);
    }
    catch (const UpstreamOAuthError& e)
    {
        if (e.error == "invalid_grant")
            throw TokenError("invalid_grant", "Upstream refresh token was rejected");
        throw;
    }

    if (response.contains("expires_in") && py_int(response["expires_in"]) <= 0)
        throw TokenError("invalid_grant", "Upstream access token has a non-positive expires_in");

    long long new_expires_in;
    if (response.contains("expires_in"))
        new_expires_in = py_int(response["expires_in"]);
    else if (options_.fallback_access_token_expiry_seconds)
        new_expires_in = *options_.fallback_access_token_expiry_seconds;
    else
        new_expires_in = DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS;
    set.access_token = response.at("access_token").get<std::string>();
    set.expires_at = now() + static_cast<double>(new_expires_in);

    const long long fastmcp_access_expires_in =
        options_.fastmcp_access_token_expiry_seconds.value_or(new_expires_in);

    std::vector<std::string> refreshed_scopes =
        response.contains("scope")
            ? parse_scopes(response["scope"]).value_or(std::vector<std::string>{})
            : scopes;
    refreshed_scopes = translate_scopes_from_idp(refreshed_scopes);
    set.scope = join(refreshed_scopes, " ");

    std::optional<long long> new_refresh_expires_in;
    if (response.contains("refresh_token") && detail::truthy(response["refresh_token"]))
    {
        const std::string new_refresh = response["refresh_token"].get<std::string>();
        if (new_refresh != set.refresh_token)
            set.refresh_token = new_refresh;
        if (response.contains("refresh_expires_in") && py_int(response["refresh_expires_in"]))
        {
            new_refresh_expires_in = py_int(response["refresh_expires_in"]);
            set.refresh_token_expires_at = now() + static_cast<double>(*new_refresh_expires_in);
        }
        else if (set.refresh_token_expires_at && *set.refresh_token_expires_at != 0.0)
            new_refresh_expires_in =
                static_cast<long long>(std::trunc(*set.refresh_token_expires_at - now()));
        else
        {
            new_refresh_expires_in = *options_.fallback_refresh_token_expiry_seconds;
            set.refresh_token_expires_at = now() + static_cast<double>(*new_refresh_expires_in);
        }
    }

    for (const auto& [k, v] : response.items())
        set.raw_token_data[k] = v;

    long long refresh_ttl;
    if (new_refresh_expires_in && *new_refresh_expires_in)
        refresh_ttl = *new_refresh_expires_in;
    else if (set.refresh_token_expires_at && *set.refresh_token_expires_at != 0.0)
        refresh_ttl = static_cast<long long>(std::trunc(*set.refresh_token_expires_at - now()));
    else
        refresh_ttl = *options_.fallback_refresh_token_expiry_seconds;
    refresh_ttl = std::max(refresh_ttl, 1LL);

    std::optional<Json> upstream_claims;
    bool grant_alive = true;
    const long long store_ttl =
        std::max({refresh_ttl, new_expires_in, fastmcp_access_expires_in, 1LL});
    try
    {
        upstream_claims = extract_upstream_claims(set.raw_token_data);
        set.upstream_claims =
            upstream_claims && detail::truthy(*upstream_claims) ? *upstream_claims : Json::object();
    }
    catch (...)
    {
        store_upstream_token_set(set, store_ttl);
        throw;
    }
    grant_alive = store_upstream_token_set(set, store_ttl);
    if (!grant_alive)
        throw TokenError("invalid_grant", "Grant was revoked");

    if (!client_storage_->erase(kRefreshTokens, hash_token(refresh_token.token)))
        throw TokenError("invalid_grant", "Refresh token already used");

    if (client.client_id.empty())
        throw TokenError("invalid_client", "Client ID is required");
    const std::string new_access_jti = token_urlsafe(32);
    const std::string new_access =
        jwt_issuer().issue_access_token(client.client_id, refreshed_scopes, new_access_jti,
                                        fastmcp_access_expires_in, upstream_claims);
    client_storage_->put(kJtiMappings, new_access_jti,
                         jti_mapping_json(new_access_jti, set.upstream_token_id, now()),
                         secs(fastmcp_access_expires_in));

    const std::string new_refresh_jti = token_urlsafe(32);
    const std::string new_refresh = jwt_issuer().issue_refresh_token(
        client.client_id, refreshed_scopes, new_refresh_jti, refresh_ttl, upstream_claims);
    client_storage_->put(kJtiMappings, new_refresh_jti,
                         jti_mapping_json(new_refresh_jti, set.upstream_token_id, now()),
                         secs(refresh_ttl));

    client_storage_->remove(kJtiMappings, refresh_jti);
    Json rotated = Json::object();
    rotated["client_id"] = client.client_id;
    rotated["upstream_token_id"] = set.upstream_token_id;
    rotated["rotated_at"] = now();
    client_storage_->put(kRotatedRefresh, hash_token(refresh_token.token), rotated,
                         secs(refresh_ttl));

    client_storage_->put(kRefreshTokens, hash_token(new_refresh),
                         refresh_metadata_json(client.client_id, refreshed_scopes,
                                               static_cast<long long>(now()) + refresh_ttl, now()),
                         secs(refresh_ttl));

    renew_client_registration(client);

    OAuthToken out;
    out.access_token = new_access;
    out.token_type = "Bearer";
    out.expires_in = fastmcp_access_expires_in;
    out.refresh_token = new_refresh;
    out.scope = join(refreshed_scopes, " ");
    return out;
}

UpstreamTokenSet OAuthProxy::try_transparent_refresh(UpstreamTokenSet set)
{
    const std::vector<std::string> scopes =
        set.scope.empty() ? std::vector<std::string>{} : detail::split_whitespace(set.scope);
    const auto upstream_scopes = prepare_scopes_for_upstream_refresh(scopes);
    TokenParams params;
    if (!upstream_scopes.empty())
        params.emplace_back("scope", join(upstream_scopes, " "));
    for (const auto& kv : options_.extra_token_params)
        params.push_back(kv);
    const Json response = create_upstream_oauth_client()->refresh_token(
        options_.upstream_token_endpoint, set.refresh_token, params);

    long long new_expires_in;
    if (response.contains("expires_in"))
        new_expires_in = py_int(response["expires_in"]);
    else if (options_.fallback_access_token_expiry_seconds)
        new_expires_in = *options_.fallback_access_token_expiry_seconds;
    else
        new_expires_in = DEFAULT_ACCESS_TOKEN_EXPIRY_SECONDS;

    set.access_token = response.at("access_token").get<std::string>();
    set.expires_at = now() + static_cast<double>(new_expires_in);
    const std::vector<std::string> refreshed_scopes =
        response.contains("scope")
            ? parse_scopes(response["scope"]).value_or(std::vector<std::string>{})
            : scopes;
    set.scope = join(translate_scopes_from_idp(refreshed_scopes), " ");

    std::optional<long long> new_refresh_expires_in;
    if (response.contains("refresh_token") && detail::truthy(response["refresh_token"]))
    {
        const std::string new_refresh = response["refresh_token"].get<std::string>();
        if (new_refresh != set.refresh_token)
            set.refresh_token = new_refresh;
        if (response.contains("refresh_expires_in") && py_int(response["refresh_expires_in"]))
        {
            new_refresh_expires_in = py_int(response["refresh_expires_in"]);
            set.refresh_token_expires_at = now() + static_cast<double>(*new_refresh_expires_in);
        }
        else if (set.refresh_token_expires_at && *set.refresh_token_expires_at != 0.0)
            new_refresh_expires_in =
                static_cast<long long>(std::trunc(*set.refresh_token_expires_at - now()));
        else
        {
            new_refresh_expires_in = *options_.fallback_refresh_token_expiry_seconds;
            set.refresh_token_expires_at = now() + static_cast<double>(*new_refresh_expires_in);
        }
    }

    for (const auto& [k, v] : response.items())
        set.raw_token_data[k] = v;

    std::optional<Json> refreshed_claims;
    try
    {
        refreshed_claims = extract_upstream_claims(set.raw_token_data);
    }
    catch (...)
    {
        refreshed_claims.reset();
    }
    set.upstream_claims =
        refreshed_claims && detail::truthy(*refreshed_claims) ? *refreshed_claims : Json::object();

    long long refresh_ttl;
    if (new_refresh_expires_in && *new_refresh_expires_in)
        refresh_ttl = *new_refresh_expires_in;
    else if (set.refresh_token_expires_at && *set.refresh_token_expires_at != 0.0)
        refresh_ttl = static_cast<long long>(std::trunc(*set.refresh_token_expires_at - now()));
    else
        refresh_ttl = *options_.fallback_refresh_token_expiry_seconds;
    if (!store_upstream_token_set(
            set, std::max({refresh_ttl, new_expires_in,
                           options_.fastmcp_access_token_expiry_seconds.value_or(0), 1LL})))
        throw TokenError("invalid_grant", "Grant was revoked");
    return set;
}

// ================================================================== validation

std::optional<AccessToken> OAuthProxy::load_access_token(const std::string& token)
{
    try
    {
        const Json payload = jwt_issuer().verify_token(token);
        const std::string jti = payload.at("jti").get<std::string>();
        Json upstream_claims =
            payload.contains("upstream_claims") ? payload["upstream_claims"] : Json(nullptr);

        if (payload.contains("fastmcp_grant") && payload["fastmcp_grant"] == kIdJagGrantMarker)
        {
            if (!options_.identity_assertion)
                return std::nullopt;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (revoked_id_jag_jtis_.count(jti))
                    return std::nullopt;
            }
            AccessToken at;
            at.token = token;
            at.client_id =
                payload.contains("client_id") ? detail::py_str(payload["client_id"]) : "";
            if (payload.contains("scope") && detail::truthy(payload["scope"]))
                at.scopes = detail::split_whitespace(payload["scope"].get<std::string>());
            if (payload.contains("exp") && detail::truthy(payload["exp"]))
                at.expires_at = static_cast<std::int64_t>(py_int(payload["exp"]));
            if (payload.contains("sub") && detail::truthy(payload["sub"]))
                at.subject = detail::py_str(payload["sub"]);
            at.claims = payload;
            return at;
        }

        const auto mapping = client_storage_->get(kJtiMappings, jti);
        if (!mapping)
            return std::nullopt;
        const auto upstream_json = client_storage_->get(
            kUpstreamTokens, mapping->at("upstream_token_id").get<std::string>());
        if (!upstream_json)
            return std::nullopt;
        UpstreamTokenSet set = UpstreamTokenSet::from_json(*upstream_json);

        auto verification_token = get_verification_token(set);
        if (!verification_token)
            return std::nullopt;
        auto& verifier = *options_.token_verifier;
        std::optional<AccessToken> validated = verifier.verify_token(*verification_token);

        const long long threshold = options_.token_expiry_threshold_seconds;
        const bool needs_refresh =
            truthy(set.refresh_token) && set.expires_at <= now() + static_cast<double>(threshold);
        const bool should_refresh = needs_refresh && (!validated || threshold > 0);

        if (should_refresh)
        {
            try
            {
                const std::string token_id = set.upstream_token_id;
                auto lock = refresh_lock(token_id);
                std::lock_guard<std::mutex> guard(*lock);
                if (const auto reread = client_storage_->get(kUpstreamTokens, token_id))
                    set = UpstreamTokenSet::from_json(*reread);
                verification_token = get_verification_token(set);
                if (verification_token)
                    validated = verifier.verify_token(*verification_token);
                if (set.expires_at <= now() + static_cast<double>(threshold))
                {
                    set = try_transparent_refresh(set);
                    verification_token = get_verification_token(set);
                    if (verification_token)
                        validated = verifier.verify_token(*verification_token);
                }
            }
            catch (const TokenVerificationError&)
            {
                throw;
            }
            catch (const std::exception&)
            {
                try
                {
                    if (const auto reloaded_json =
                            client_storage_->get(kUpstreamTokens, set.upstream_token_id))
                    {
                        const auto reloaded = UpstreamTokenSet::from_json(*reloaded_json);
                        verification_token = get_verification_token(reloaded);
                        if (verification_token)
                        {
                            validated = verifier.verify_token(*verification_token);
                            if (validated)
                                set = reloaded;
                        }
                    }
                }
                catch (const TokenVerificationError&)
                {
                    throw;
                }
                catch (const std::exception&)
                {
                }
            }
        }

        if (!validated)
            return std::nullopt;

        if (uses_alternate_verification())
        {
            validated->token = set.access_token;
            if (!set.scope.empty())
                validated->scopes = detail::split_whitespace(set.scope);
            validated->expires_at = static_cast<std::int64_t>(std::trunc(set.expires_at));
            if (verifier.required_scopes().empty())
                for (const auto& s : required_scopes_)
                    if (!contains(validated->scopes, s))
                        return std::nullopt;
        }

        if (!set.upstream_claims.is_null())
            upstream_claims = set.upstream_claims;

        if (detail::truthy(upstream_claims))
        {
            if (!validated->claims.is_object())
                validated->claims = Json::object();
            validated->claims["upstream_claims"] = upstream_claims;
        }
        return validated;
    }
    catch (const TokenVerificationError&)
    {
        throw;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

// ================================================================== revocation

void OAuthProxy::revoke_token(const std::variant<AccessToken, RefreshToken>& token)
{
    revoke_token_impl(token, std::nullopt, true);
}

void OAuthProxy::revoke_token_impl(const std::variant<AccessToken, RefreshToken>& token,
                                   const std::optional<std::string>& fastmcp_jti,
                                   bool upstream_live)
{
    if (const auto* rt = std::get_if<RefreshToken>(&token))
    {
        client_storage_->remove(kRefreshTokens, hash_token(rt->token));
        revoke_refresh_grant(*rt);
        return;
    }
    const auto& at = std::get<AccessToken>(token);

    const JWTIssuer& issuer = jwt_issuer();
    std::optional<Json> payload;
    try
    {
        payload = issuer.verify_token(at.token);
    }
    catch (const std::exception&)
    {
        payload.reset();
    }
    if (payload && payload->contains("fastmcp_grant") &&
        (*payload)["fastmcp_grant"] == kIdJagGrantMarker)
    {
        const double t = now();
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = revoked_id_jag_jtis_.begin(); it != revoked_id_jag_jtis_.end();)
            it = it->second > t ? std::next(it) : revoked_id_jag_jtis_.erase(it);
        const auto jti = opt_str(*payload, "jti");
        if (truthy(jti))
        {
            const bool numeric_exp = payload->contains("exp") && (*payload)["exp"].is_number();
            revoked_id_jag_jtis_[*jti] = numeric_exp ? (*payload)["exp"].get<double>() : t + 3600;
        }
        return;
    }

    if (fastmcp_jti)
    {
        client_storage_->remove(kJtiMappings, *fastmcp_jti);
        if (!upstream_live)
            return;
    }
    revoke_upstream(at.token);
}

void OAuthProxy::revoke_refresh_grant(const RefreshToken& token)
{
    std::string refresh_jti;
    try
    {
        const Json payload = jwt_issuer().verify_token(token.token, "refresh");
        refresh_jti = payload.at("jti").get<std::string>();
    }
    catch (const std::exception& e)
    {
        std::cerr << "[fastmcpp] Failed to revoke token with upstream server: " << e.what() << "\n";
        return;
    }
    const auto mapping = client_storage_->get(kJtiMappings, refresh_jti);
    if (!mapping)
        return;
    const std::string upstream_token_id = mapping->at("upstream_token_id").get<std::string>();
    if (const auto j = client_storage_->get(kUpstreamTokens, upstream_token_id))
    {
        const auto set = UpstreamTokenSet::from_json(*j);
        if (truthy(set.refresh_token))
            revoke_upstream(*set.refresh_token);
    }
    client_storage_->remove(kJtiMappings, refresh_jti);
    end_grant(upstream_token_id);
}

void OAuthProxy::revoke_upstream(const std::string& upstream_token)
{
    if (!truthy(options_.upstream_revocation_endpoint))
        return;
    try
    {
        client::auth::HttpRequest req;
        req.method = "POST";
        req.url = *options_.upstream_revocation_endpoint;
        std::vector<std::pair<std::string, std::string>> form{{"token", upstream_token}};
        const auto client = create_upstream_oauth_client();
        if (client->client_secret())
        {
            const std::string creds = options_.upstream_client_id + ":" + *client->client_secret();
            req.headers["Authorization"] = "Basic " + b64encode(creds);
        }
        else
            form.emplace_back("client_id", options_.upstream_client_id);
        req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        req.body = pu::urlencode(form);
        (void)options_.http(req);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[fastmcpp] Failed to revoke token with upstream server: " << e.what() << "\n";
    }
}

// ================================================================== routes

std::vector<CustomRoute> OAuthProxy::get_routes(const std::optional<std::string>& mcp_path)
{
    auto routes = OAuthProvider::get_routes(mcp_path);
    std::vector<CustomRoute> out;

    const std::string base_url = *base_url_;
    auto authorize_handler =
        std::make_shared<EnhancedAuthorizationHandler>(*this, base_url, issuer_url_);

    std::function<CustomRouteResponse(const CustomRouteRequest&)> token_route;
    if (cimd_manager_ || options_.identity_assertion)
    {
        std::shared_ptr<ClientAuthenticator> authenticator;
        if (cimd_manager_)
            authenticator = std::make_shared<PrivateKeyJWTClientAuthenticator>(
                *this, cimd_manager_, token_endpoint_url());
        else
            authenticator = std::make_shared<ClientAuthenticator>(*this);
        auto token_handler = std::make_shared<TokenHandler>(
            *this, authenticator, options_.identity_assertion.has_value());
        token_route = cors_handler([token_handler](const CustomRouteRequest& req)
                                   { return token_handler->handle(req); }, {"POST", "OPTIONS"});
    }

    std::vector<CustomRoute> revocation_routes;
    if (revocation_options_ && revocation_options_->enabled)
    {
        revocation_adapter_ = std::make_shared<ProxyRevocationAdapter>(*this);
        AuthRoutesOptions ro;
        ro.base_url = base_url;
        ro.revocation_options = *revocation_options_;
        for (auto& r : create_auth_routes(*revocation_adapter_, ro))
            if (r.path == "/revoke")
                revocation_routes.push_back(std::move(r));
    }

    auto registration = std::make_shared<RegistrationHandler>(
        *this, client_registration_options_.value_or(ClientRegistrationOptions{}));
    auto registration_route = cors_handler(
        [registration](const CustomRouteRequest& req)
        {
            auto res = registration->handle(req);
            if (res.status == 201)
            {
                auto body = OJ::parse(res.body);
                body["token_endpoint_auth_method"] = "none";
                body.erase("client_secret");
                body.erase("client_secret_expires_at");
                res.body = body.dump();
            }
            return res;
        },
        {"POST", "OPTIONS"});

    std::string metadata_body;
    {
        auto metadata =
            build_metadata(base_url, service_documentation_url_,
                           client_registration_options_.value_or(ClientRegistrationOptions{}),
                           revocation_options_.value_or(RevocationOptions{}),
                           options_.identity_assertion.has_value());
        metadata["issuer"] = issuer_url_;
        std::vector<std::string> auth_methods{"none"};
        if (cimd_manager_)
            auth_methods.push_back("private_key_jwt");
        OJ m;
        for (auto it = metadata.begin(); it != metadata.end(); ++it)
        {
            if (it.key() == "token_endpoint_auth_methods_supported")
                m[it.key()] = auth_methods;
            else
                m[it.key()] = it.value();
            if (it.key() == "code_challenge_methods_supported")
            {
                if (cimd_manager_)
                    m["client_id_metadata_document_supported"] = true;
                m["authorization_response_iss_parameter_supported"] = true;
            }
        }
        if (!m.contains("authorization_response_iss_parameter_supported"))
        {
            if (cimd_manager_)
                m["client_id_metadata_document_supported"] = true;
            m["authorization_response_iss_parameter_supported"] = true;
        }
        metadata_body = m.dump();
    }
    auto metadata_route = cors_handler(
        [metadata_body](const CustomRouteRequest&)
        {
            CustomRouteResponse r;
            r.body = metadata_body;
            r.content_type = "application/json";
            r.headers["Cache-Control"] = "public, max-age=3600";
            return r;
        },
        {"GET", "OPTIONS"});

    bool revoke_added = false;
    for (auto& route : routes)
    {
        if (route.path == "/authorize")
            out.push_back(CustomRoute{route.method, route.path,
                                      [authorize_handler](const CustomRouteRequest& req)
                                      { return authorize_handler->handle(req); }});
        else if (route.path == "/token" && token_route)
            out.push_back(CustomRoute{route.method, route.path, token_route});
        else if (route.path == "/revoke")
        {
            if (!revoke_added)
                for (const auto& r : revocation_routes)
                    out.push_back(r);
            revoke_added = true;
        }
        else if (route.path == "/register")
            out.push_back(CustomRoute{route.method, route.path, registration_route});
        else if (route.path.rfind("/.well-known/oauth-authorization-server", 0) == 0)
            out.push_back(CustomRoute{route.method, route.path, metadata_route});
        else
            out.push_back(std::move(route));
    }

    out.push_back(CustomRoute{"GET", redirect_path_, [this](const CustomRouteRequest& req)
                              { return handle_idp_callback(req); }});
    const auto consent = [this](const CustomRouteRequest& req) { return handle_consent(req); };
    out.push_back(CustomRoute{"GET", "/consent", consent});
    out.push_back(CustomRoute{"POST", "/consent", consent});
    return out;
}

// ================================================================== IdP callback

CustomRouteResponse OAuthProxy::handle_idp_callback(const CustomRouteRequest& req)
{
    const auto error_page = [](const std::string& title, const std::string& message, int status,
                               std::vector<std::pair<std::string, std::string>> details = {})
    { return html_response(create_error_html(title, message, details), status); };
    try
    {
        const auto idp_code = query_param(req, "code");
        const auto txn_id = query_param(req, "state");
        const auto error = query_param(req, "error");

        if (!truthy(idp_code) && !truthy(error))
            return error_page(
                "OAuth Error",
                "Missing authorization code or transaction ID from the identity provider.", 400);

        const std::optional<OAuthTransaction> txn =
            truthy(txn_id) ? load_transaction(*txn_id) : std::nullopt;

        if (truthy(error))
        {
            const auto error_description = query_param(req, "error_description");
            if (txn)
            {
                if (!validate_client_redirect_uri(txn->client_redirect_uri))
                    return error_page("OAuth Error", "Invalid redirect URI", 400);
                QueryParams params{{"error", *error}, {"state", txn->client_state}};
                if (truthy(error_description))
                    params.emplace_back("error_description", *error_description);
                return redirect(
                    build_client_redirect(txn->client_redirect_uri, params, issuer_url_));
            }
            return error_page("OAuth Error",
                              "Authentication failed: " + (truthy(error_description)
                                                               ? *error_description
                                                               : std::string("Unknown error")),
                              400, {{"Error Code", *error}});
        }
        if (!truthy(txn_id) || !txn)
            return error_page(
                "OAuth Error",
                "Invalid or expired authorization transaction. Please try authenticating again.",
                400);

        if (!validate_client_redirect_uri(txn->client_redirect_uri))
            return error_page("OAuth Error", "Invalid redirect URI", 400);

        const auto& mode = options_.require_authorization_consent;
        if (mode == "true" || mode == "remember")
        {
            if (!truthy(txn->consent_token))
                return error_page("Authorization Error",
                                  "Invalid authorization flow. Please try authenticating again.",
                                  403);
            if (!verify_consent_binding_cookie(req, *txn_id, *txn->consent_token))
                return error_page("Authorization Error",
                                  "Authorization session mismatch. This can happen if you "
                                  "followed a link from another person or your session expired. "
                                  "Please try authenticating again.",
                                  403);
        }

        Json idp_tokens;
        try
        {
            TokenParams params{{"code", *idp_code},
                               {"redirect_uri", http_url_join(*base_url_, redirect_path_)}};
            if (truthy(txn->proxy_code_verifier))
                params.emplace_back("code_verifier", *txn->proxy_code_verifier);
            const auto exchange_scopes = prepare_scopes_for_token_exchange(txn->scopes);
            if (!exchange_scopes.empty())
                params.emplace_back("scope", join(exchange_scopes, " "));
            for (const auto& [k, v] : options_.extra_token_params)
            {
                bool replaced = false;
                for (auto& [pk, pv] : params)
                    if (pk == k)
                    {
                        pv = v;
                        replaced = true;
                    }
                if (!replaced)
                    params.emplace_back(k, v);
            }
            idp_tokens = create_upstream_oauth_client()->fetch_token(
                options_.upstream_token_endpoint, params);
        }
        catch (const std::exception& e)
        {
            return error_page(
                "OAuth Error",
                std::string("Token exchange with identity provider failed: ") + e.what(), 500);
        }

        const std::string client_code = token_urlsafe(32);
        ClientCode code;
        code.code = client_code;
        code.client_id = txn->client_id;
        code.redirect_uri = txn->client_redirect_uri;
        code.code_challenge = txn->code_challenge;
        code.code_challenge_method = txn->code_challenge_method;
        code.scopes = txn->scopes;
        code.idp_tokens = idp_tokens;
        code.expires_at = static_cast<double>(static_cast<long long>(
            std::trunc(now() + static_cast<double>(DEFAULT_AUTH_CODE_EXPIRY_SECONDS))));
        code.created_at = now();
        client_storage_->put(kCodes, client_code, code.to_json(),
                             secs(DEFAULT_AUTH_CODE_EXPIRY_SECONDS));
        client_storage_->remove(kTransactions, *txn_id);

        auto response = redirect(build_client_redirect(
            txn->client_redirect_uri, {{"code", client_code}, {"state", txn->client_state}},
            issuer_url_));
        expire_cookie(response, consent_binding_cookie_name(*txn_id));
        return response;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[fastmcpp] Error in IdP callback handler: " << e.what() << "\n";
        return error_page(
            "OAuth Error",
            "Internal server error during OAuth callback processing. Please try again.", 500);
    }
}

// ================================================================== consent

bool OAuthProxy::is_https() const
{
    return base_url_->rfind("https://", 0) == 0;
}

std::string OAuthProxy::cookie_name(const std::string& base) const
{
    return (is_https() ? "__Host-" : "__") + base;
}

namespace
{
std::string consent_state_base_name(const std::string& csrf_token)
{
    return std::string(kConsentStateCookieBase) + "_" + sha256_hex(csrf_token).substr(0, 32);
}
} // namespace

std::string OAuthProxy::sign_cookie(const std::string& payload) const
{
    const std::string key =
        options_.upstream_client_secret ? *options_.upstream_client_secret : jwt_signing_key_;
    const auto sig = crypto::hmac_sha256(
        bytes_of(key), reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
    return payload + "." + crypto::base64_encode(sig.data(), sig.size());
}

std::optional<std::string> OAuthProxy::verify_cookie(const std::string& signed_value) const
{
    const auto dot = signed_value.rfind('.');
    if (dot == std::string::npos)
        return std::nullopt;
    const std::string payload = signed_value.substr(0, dot);
    const std::string expected = sign_cookie(payload).substr(payload.size() + 1);
    const auto expected_bytes = b64decode(expected);
    const auto provided = b64decode(signed_value.substr(dot + 1));
    if (!expected_bytes || !provided || !ct_equal(*expected_bytes, *provided))
        return std::nullopt;
    return payload;
}

std::vector<std::string> OAuthProxy::decode_list_cookie(const CustomRouteRequest& req,
                                                        const std::string& base) const
{
    const auto cookies = ck::request_cookies(req);
    std::string raw;
    if (auto it = cookies.find(cookie_name(base)); it != cookies.end())
        raw = it->second;
    if (raw.empty() && !is_https())
        if (auto it = cookies.find("__" + base); it != cookies.end())
            raw = it->second;
    if (raw.empty())
        return {};
    try
    {
        const auto payload = verify_cookie(raw);
        if (!truthy(payload))
            return {};
        const auto data = b64decode(*payload);
        if (!data)
            return {};
        const auto value = pyjson::loads(*data);
        if (value.is_array())
        {
            std::vector<std::string> out;
            for (const auto& v : value)
                out.push_back(pyjson::str(v));
            return out;
        }
    }
    catch (...)
    {
    }
    return {};
}

std::string OAuthProxy::encode_list_cookie(const std::vector<std::string>& values) const
{
    return sign_cookie(b64encode(pyjson::dumps_compact(OJ(values))));
}

void OAuthProxy::set_list_cookie(CustomRouteResponse& res, const std::string& base,
                                 const std::string& value, long long max_age) const
{
    ck::CookieOptions o;
    o.max_age = max_age;
    o.secure = is_https();
    o.httponly = true;
    o.samesite = "lax";
    o.path = "/";
    ck::set_cookie(res, cookie_name(base), value, o);
}

std::map<std::string, std::pair<std::string, double>>
OAuthProxy::read_consent_state_cookies(const CustomRouteRequest& req) const
{
    const std::string prefix = cookie_name(std::string(kConsentStateCookieBase) + "_");
    std::map<std::string, std::pair<std::string, double>> found;
    for (const auto& [name, raw] : ck::request_cookies(req))
    {
        if (name.rfind(prefix, 0) != 0)
            continue;
        const auto payload = verify_cookie(raw);
        if (!truthy(payload))
            continue;
        OJ data;
        try
        {
            const auto decoded = b64decode(*payload);
            if (!decoded)
                continue;
            data = pyjson::loads(*decoded);
        }
        catch (...)
        {
            continue;
        }
        if (!data.is_object())
            continue;
        const auto txn = data.find("txn");
        const auto iat = data.find("iat");
        if (txn != data.end() && txn->is_string() && iat != data.end() &&
            (iat->is_number() || iat->is_boolean()))
            found[name] = {txn->get<std::string>(),
                           iat->is_boolean() ? (iat->get<bool>() ? 1.0 : 0.0) : iat->get<double>()};
    }
    return found;
}

void OAuthProxy::set_consent_state_cookie(CustomRouteResponse& res, const std::string& csrf_token,
                                          const std::string& txn_id, double issued_at) const
{
    OJ state = OJ::object();
    state["txn"] = txn_id;
    state["iat"] = issued_at;
    ck::CookieOptions o;
    o.max_age = kTransactionTtl;
    o.secure = is_https();
    o.httponly = true;
    o.samesite = "lax";
    o.path = "/";
    ck::set_cookie(res, cookie_name(consent_state_base_name(csrf_token)),
                   sign_cookie(b64encode(pyjson::dumps_compact(state))), o);
}

void OAuthProxy::clear_consent_state_for_transaction(const CustomRouteRequest& req,
                                                     CustomRouteResponse& res,
                                                     const std::string& txn_id,
                                                     bool include_legacy) const
{
    for (const auto& [name, entry] : read_consent_state_cookies(req))
        if (ct_equal(entry.first, txn_id))
            expire_cookie(res, name);
    if (include_legacy)
        set_list_cookie(res, kConsentStateCookieBase, encode_list_cookie({}), 60);
}

void OAuthProxy::expire_cookie(CustomRouteResponse& res, const std::string& name) const
{
    ck::CookieOptions o;
    o.max_age = 0;
    o.secure = is_https();
    o.httponly = true;
    o.samesite = "lax";
    o.path = "/";
    ck::set_cookie(res, name, "", o);
}

std::map<std::string, std::string>
OAuthProxy::read_consent_bindings(const CustomRouteRequest& req) const
{
    const auto cookies = ck::request_cookies(req);
    std::string raw;
    if (auto it = cookies.find(cookie_name(kConsentBindingCookieBase)); it != cookies.end())
        raw = it->second;
    if (raw.empty() && !is_https())
        if (auto it = cookies.find("__MCP_CONSENT_BINDING"); it != cookies.end())
            raw = it->second;
    if (raw.empty())
        return {};
    const auto payload = verify_cookie(raw);
    if (!truthy(payload))
        return {};
    try
    {
        const auto decoded = b64decode(*payload);
        if (!decoded)
            return {};
        const auto data = pyjson::loads(*decoded);
        if (data.is_object())
        {
            std::map<std::string, std::string> out;
            for (auto it = data.begin(); it != data.end(); ++it)
                out[it.key()] = pyjson::str(it.value());
            return out;
        }
    }
    catch (...)
    {
    }
    return {};
}

std::string OAuthProxy::consent_binding_cookie_name(const std::string& txn_id) const
{
    return cookie_name(std::string(kConsentBindingCookieBase) + "_" +
                       sha256_hex(txn_id).substr(0, 32));
}

void OAuthProxy::set_consent_binding_cookie(CustomRouteResponse& res, const std::string& txn_id,
                                            const std::string& consent_token) const
{
    ck::CookieOptions o;
    o.max_age = kTransactionTtl;
    o.secure = is_https();
    o.httponly = true;
    o.samesite = "lax";
    o.path = "/";
    ck::set_cookie(res, consent_binding_cookie_name(txn_id), sign_cookie(consent_token), o);
}

bool OAuthProxy::verify_consent_binding_cookie(const CustomRouteRequest& req,
                                               const std::string& txn_id,
                                               const std::string& expected_token) const
{
    const auto cookies = ck::request_cookies(req);
    if (auto it = cookies.find(consent_binding_cookie_name(txn_id));
        it != cookies.end() && !it->second.empty())
    {
        const auto actual = verify_cookie(it->second);
        if (truthy(actual) && ct_equal(*actual, expected_token))
            return true;
    }
    const auto bindings = read_consent_bindings(req);
    const auto it = bindings.find(txn_id);
    return it != bindings.end() && !it->second.empty() && ct_equal(it->second, expected_token);
}

std::string OAuthProxy::make_client_key(const std::string& client_id,
                                        const std::string& redirect_uri,
                                        const std::vector<std::string>& scopes) const
{
    const auto parsed = pu::urlparse(redirect_uri);
    std::string normalized =
        detail::lower(parsed.scheme) + "://" + detail::lower(parsed.netloc) + parsed.path;
    if (!normalized.empty() && normalized.back() == '/' && parsed.path.size() > 1)
        normalized.pop_back();
    if (!parsed.query.empty())
        normalized += "?" + parsed.query;
    const std::set<std::string> unique(scopes.begin(), scopes.end());
    OJ material = OJ::array(
        {client_id, normalized, OJ(std::vector<std::string>(unique.begin(), unique.end()))});
    return sha256_hex(pyjson::dumps_compact(material));
}

bool OAuthProxy::validate_client_redirect_uri(const std::string& redirect_uri) const
{
    return validate_redirect_uri(redirect_uri, options_.allowed_client_redirect_uris);
}

CustomRouteResponse OAuthProxy::handle_consent(const CustomRouteRequest& req)
{
    if (req.method == "POST")
        return submit_consent(req);
    return show_consent_page(req);
}

CustomRouteResponse OAuthProxy::show_consent_page(const CustomRouteRequest& req)
{
    const auto txn_id = query_param(req, "txn_id");
    if (!truthy(txn_id))
        return secure_html_response("<h1>Error</h1><p>Invalid or expired transaction</p>", 400);
    auto txn = load_transaction(*txn_id);
    if (!txn)
        return secure_html_response("<h1>Error</h1><p>Invalid or expired transaction</p>", 400);

    const std::string client_key =
        make_client_key(txn->client_id, txn->client_redirect_uri, txn->scopes);

    if (options_.require_authorization_consent == "remember")
    {
        const auto sec_fetch_site = header(req, "Sec-Fetch-Site");
        const bool silent_eligible =
            sec_fetch_site && (*sec_fetch_site == "same-origin" || *sec_fetch_site == "same-site" ||
                               *sec_fetch_site == "none");
        if (silent_eligible)
        {
            const auto approved = decode_list_cookie(req, kApprovedClients);
            const auto denied = decode_list_cookie(req, kDeniedClients);
            if (contains(approved, client_key))
            {
                const std::string consent_token = token_urlsafe(32);
                const OAuthTransaction before = *txn;
                txn->consent_token = consent_token;
                client_storage_->put(kTransactions, *txn_id, txn->to_json(), secs(kTransactionTtl));
                auto response = redirect(build_upstream_authorize_url(*txn_id, before));
                set_consent_binding_cookie(response, *txn_id, consent_token);
                return response;
            }
            if (contains(denied, client_key))
            {
                if (!validate_client_redirect_uri(txn->client_redirect_uri))
                    return secure_html_response("<h1>Error</h1><p>Invalid redirect URI</p>", 400);
                return redirect(build_client_redirect(
                    txn->client_redirect_uri,
                    {{"error", "access_denied"}, {"state", txn->client_state}}, issuer_url_));
            }
        }
    }

    const std::string csrf_token = token_urlsafe(32);
    const double issued_at = now();
    Json csrf_record = Json::object();
    csrf_record["txn_id"] = *txn_id;
    csrf_record["expires_at"] = issued_at + static_cast<double>(kTransactionTtl);
    client_storage_->put(kConsentCsrf, hash_token(csrf_token), csrf_record, secs(kTransactionTtl));

    const auto client = get_client(txn->client_id);
    const std::optional<std::string> client_name = client ? client->client_name : std::nullopt;
    bool is_cimd_client = false;
    std::optional<std::string> cimd_domain;
    if (client && client->proxy_client && client->cimd_document)
    {
        is_cimd_client = true;
        cimd_domain = pu::hostname(pu::urlparse(txn->client_id));
    }

    const std::string html = create_consent_html(
        txn->client_id, txn->client_redirect_uri, txn->scopes, *txn_id, csrf_token, client_name,
        "Application Access Request", server_name_, server_icon_url_, server_website_url_,
        std::nullopt, options_.consent_csp_policy, is_cimd_client, cimd_domain);
    auto response = secure_html_response(html);
    set_consent_state_cookie(response, csrf_token, *txn_id, issued_at);

    auto others = read_consent_state_cookies(req);
    others.erase(cookie_name(consent_state_base_name(csrf_token)));
    const long long surplus =
        static_cast<long long>(others.size()) + 1 - static_cast<long long>(kMaxCsrfTokens);
    if (surplus > 0)
    {
        std::vector<std::pair<std::string, double>> by_age;
        for (const auto& [name, entry] : others)
            by_age.emplace_back(name, entry.second);
        std::stable_sort(by_age.begin(), by_age.end(),
                         [](const auto& a, const auto& b) { return a.second < b.second; });
        for (long long i = 0; i < surplus; ++i)
            expire_cookie(response, by_age[static_cast<std::size_t>(i)].first);
    }
    return response;
}

CustomRouteResponse OAuthProxy::submit_consent(const CustomRouteRequest& req)
{
    const FormData form = FormData::parse(req);
    const std::string txn_id = form.get("txn_id").value_or("");
    const std::string action = form.get("action").value_or("");
    const std::string csrf_token = form.get("csrf_token").value_or("");

    if (txn_id.empty())
        return secure_html_response("<h1>Error</h1><p>Invalid or expired transaction</p>", 400);
    auto txn = load_transaction(txn_id);
    if (!txn)
        return secure_html_response("<h1>Error</h1><p>Invalid or expired transaction</p>", 400);

    std::optional<Json> csrf_record;
    if (!csrf_token.empty())
        csrf_record = client_storage_->get(kConsentCsrf, hash_token(csrf_token));
    bool csrf_valid = false;
    bool legacy_csrf = false;
    if (csrf_record)
        csrf_valid = ct_equal(csrf_record->at("txn_id").get<std::string>(), txn_id) &&
                     now() <= csrf_record->at("expires_at").get<double>();
    else
    {
        csrf_valid = !csrf_token.empty() && truthy(txn->csrf_token) &&
                     ct_equal(*txn->csrf_token, csrf_token) &&
                     now() <= txn->csrf_expires_at.value_or(0.0);
        legacy_csrf = csrf_valid;
    }
    if (!csrf_valid)
        return secure_html_response("<h1>Error</h1><p>Invalid or expired consent token</p>", 400);

    bool cookie_ok;
    if (legacy_csrf)
        cookie_ok = contains(decode_list_cookie(req, kConsentStateCookieBase), csrf_token);
    else
    {
        const auto states = read_consent_state_cookies(req);
        const auto it = states.find(cookie_name(consent_state_base_name(csrf_token)));
        cookie_ok = it != states.end() && ct_equal(it->second.first, txn_id);
    }
    if (!cookie_ok)
        return secure_html_response(
            "<h1>Error</h1><p>Authorization session mismatch. Please try authenticating "
            "again.</p>",
            403);

    const std::string client_key =
        make_client_key(txn->client_id, txn->client_redirect_uri, txn->scopes);
    const bool remember_mode = options_.require_authorization_consent == "remember";

    const auto remember = [&](CustomRouteResponse& response, const char* base)
    {
        auto values = decode_list_cookie(req, base);
        values.erase(std::remove(values.begin(), values.end(), client_key), values.end());
        values.push_back(client_key);
        if (values.size() > kMaxRememberedClients)
            values.erase(values.begin(),
                         values.begin() +
                             static_cast<std::ptrdiff_t>(values.size() - kMaxRememberedClients));
        set_list_cookie(response, base, encode_list_cookie(values), kRememberMaxAge);
    };

    if (action == "approve")
    {
        const std::string consent_token = token_urlsafe(32);
        const OAuthTransaction before = *txn;
        txn->consent_token = consent_token;
        client_storage_->put(kTransactions, txn_id, txn->to_json(), secs(kTransactionTtl));
        auto response = redirect(build_upstream_authorize_url(txn_id, before));
        if (remember_mode)
            remember(response, kApprovedClients);
        client_storage_->remove(kConsentCsrf, hash_token(csrf_token));
        clear_consent_state_for_transaction(req, response, txn_id, legacy_csrf);
        set_consent_binding_cookie(response, txn_id, consent_token);
        return response;
    }
    if (action == "deny")
    {
        if (!validate_client_redirect_uri(txn->client_redirect_uri))
            return secure_html_response("<h1>Error</h1><p>Invalid redirect URI</p>", 400);
        auto response = redirect(build_client_redirect(
            txn->client_redirect_uri, {{"error", "access_denied"}, {"state", txn->client_state}},
            issuer_url_));
        if (remember_mode)
            remember(response, kDeniedClients);
        client_storage_->remove(kConsentCsrf, hash_token(csrf_token));
        clear_consent_state_for_transaction(req, response, txn_id, legacy_csrf);
        return response;
    }
    return secure_html_response("<h1>Error</h1><p>Invalid action</p>", 400);
}

// ================================================================== OIDC

namespace
{

using pyjson::LineError;

/// pydantic's lax bool: bools, 0/1 numbers and the usual words.
std::optional<bool> parse_bool(const OJ& v, const std::string& loc, std::vector<LineError>& errs)
{
    if (v.is_boolean())
        return v.get<bool>();
    if (v.is_number_integer() || v.is_number_unsigned())
    {
        if (pyjson::is_int(v) && v.is_number_integer())
        {
            const auto n = v.get<long long>();
            if (n == 0 || n == 1)
                return n == 1;
        }
        errs.push_back(
            {loc, "bool_parsing", "Input should be a valid boolean, unable to interpret input", v});
        return std::nullopt;
    }
    if (v.is_number_float())
    {
        const double d = v.get<double>();
        if (d == 0.0 || d == 1.0)
            return d == 1.0;
        if (std::isfinite(d) && std::trunc(d) == d)
            errs.push_back({loc, "bool_parsing",
                            "Input should be a valid boolean, unable to interpret input", v});
        else
            errs.push_back({loc, "bool_type", "Input should be a valid boolean", v});
        return std::nullopt;
    }
    if (v.is_string())
    {
        const std::string s = detail::lower(v.get<std::string>());
        static const std::set<std::string> yes{"1", "on", "t", "true", "y", "yes"};
        static const std::set<std::string> no{"0", "off", "f", "false", "n", "no"};
        if (yes.count(s))
            return true;
        if (no.count(s))
            return false;
        errs.push_back(
            {loc, "bool_parsing", "Input should be a valid boolean, unable to interpret input", v});
        return std::nullopt;
    }
    errs.push_back({loc, "bool_type", "Input should be a valid boolean", v});
    return std::nullopt;
}

enum class FieldKind
{
    Url,
    Strings,
    Bool,
    Str
};

struct OIDCField
{
    const char* name;
    FieldKind kind;
};

const std::vector<OIDCField>& oidc_fields()
{
    static const std::vector<OIDCField> fields{
        {"issuer", FieldKind::Url},
        {"authorization_endpoint", FieldKind::Url},
        {"token_endpoint", FieldKind::Url},
        {"userinfo_endpoint", FieldKind::Url},
        {"jwks_uri", FieldKind::Url},
        {"registration_endpoint", FieldKind::Url},
        {"scopes_supported", FieldKind::Strings},
        {"response_types_supported", FieldKind::Strings},
        {"response_modes_supported", FieldKind::Strings},
        {"grant_types_supported", FieldKind::Strings},
        {"acr_values_supported", FieldKind::Strings},
        {"subject_types_supported", FieldKind::Strings},
        {"id_token_signing_alg_values_supported", FieldKind::Strings},
        {"id_token_encryption_alg_values_supported", FieldKind::Strings},
        {"id_token_encryption_enc_values_supported", FieldKind::Strings},
        {"userinfo_signing_alg_values_supported", FieldKind::Strings},
        {"userinfo_encryption_alg_values_supported", FieldKind::Strings},
        {"userinfo_encryption_enc_values_supported", FieldKind::Strings},
        {"request_object_signing_alg_values_supported", FieldKind::Strings},
        {"request_object_encryption_alg_values_supported", FieldKind::Strings},
        {"request_object_encryption_enc_values_supported", FieldKind::Strings},
        {"token_endpoint_auth_methods_supported", FieldKind::Strings},
        {"token_endpoint_auth_signing_alg_values_supported", FieldKind::Strings},
        {"display_values_supported", FieldKind::Strings},
        {"claim_types_supported", FieldKind::Strings},
        {"claims_supported", FieldKind::Strings},
        {"service_documentation", FieldKind::Url},
        {"claims_locales_supported", FieldKind::Strings},
        {"ui_locales_supported", FieldKind::Strings},
        {"claims_parameter_supported", FieldKind::Bool},
        {"request_parameter_supported", FieldKind::Bool},
        {"request_uri_parameter_supported", FieldKind::Bool},
        {"require_request_uri_registration", FieldKind::Bool},
        {"op_policy_uri", FieldKind::Url},
        {"op_tos_uri", FieldKind::Url},
        {"revocation_endpoint", FieldKind::Url},
        {"revocation_endpoint_auth_methods_supported", FieldKind::Strings},
        {"revocation_endpoint_auth_signing_alg_values_supported", FieldKind::Strings},
        {"introspection_endpoint", FieldKind::Url},
        {"introspection_endpoint_auth_methods_supported", FieldKind::Strings},
        {"introspection_endpoint_auth_signing_alg_values_supported", FieldKind::Strings},
        {"code_challenge_methods_supported", FieldKind::Strings},
        {"signed_metadata", FieldKind::Str},
    };
    return fields;
}

struct OIDCCacheEntry
{
    double expires_at;
    OIDCConfiguration config;
    std::list<std::string>::iterator position;
};

std::mutex g_oidc_cache_mutex;
std::map<std::string, OIDCCacheEntry> g_oidc_cache;
std::list<std::string> g_oidc_cache_order;
constexpr double kOidcCacheTtl = 300;
constexpr std::size_t kOidcCacheMaxSize = 128;

double monotonic_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

OIDCConfiguration OIDCConfiguration::from_json(const nlohmann::ordered_json& data)
{
    std::vector<LineError> errs;
    if (!data.is_object())
    {
        errs.push_back({"", "model_type",
                        "Input should be a valid dictionary or instance of OIDCConfiguration",
                        data});
        throw std::invalid_argument(pyjson::validation_error_str("OIDCConfiguration", errs));
    }

    OIDCConfiguration c;
    if (const auto it = data.find("strict"); it != data.end())
    {
        if (const auto b = parse_bool(*it, "strict", errs))
            c.strict = *b;
    }

    std::map<std::string, std::optional<std::string>> strings;
    std::map<std::string, bool> present_lists;
    for (const auto& field : oidc_fields())
    {
        const auto it = data.find(field.name);
        if (it == data.end() || it->is_null())
            continue;
        const OJ& v = *it;
        switch (field.kind)
        {
        case FieldKind::Url:
            if (v.is_string())
                strings[field.name] = v.get<std::string>();
            else
            {
                errs.push_back({std::string(field.name) + ".function-wrap[wrap_val()]", "url_type",
                                "URL input should be a string or URL", v});
                errs.push_back({std::string(field.name) + ".str", "string_type",
                                "Input should be a valid string", v});
            }
            break;
        case FieldKind::Str:
            if (!v.is_string())
                errs.push_back({field.name, "string_type", "Input should be a valid string", v});
            break;
        case FieldKind::Bool:
            parse_bool(v, field.name, errs);
            break;
        case FieldKind::Strings:
            if (v.is_string())
            {
                LineError e{field.name, "sequence_str",
                            "'str' instances are not allowed as a Sequence value", v};
                e.url = false;
                errs.push_back(e);
            }
            else if (!v.is_array())
                errs.push_back(
                    {field.name, "is_instance_of", "Input should be an instance of Sequence", v});
            else
            {
                bool ok = true;
                for (std::size_t i = 0; i < v.size(); ++i)
                    if (!v[i].is_string())
                    {
                        errs.push_back({std::string(field.name) + "." + std::to_string(i),
                                        "string_type", "Input should be a valid string", v[i]});
                        ok = false;
                    }
                if (ok)
                    present_lists[field.name] = !v.empty();
            }
            break;
        }
    }
    if (!errs.empty())
        throw std::invalid_argument(pyjson::validation_error_str("OIDCConfiguration", errs));

    const auto get = [&strings](const char* k) -> std::optional<std::string>
    {
        const auto it = strings.find(k);
        return it == strings.end() ? std::nullopt : it->second;
    };
    c.issuer = get("issuer");
    c.authorization_endpoint = get("authorization_endpoint");
    c.token_endpoint = get("token_endpoint");
    c.userinfo_endpoint = get("userinfo_endpoint");
    c.jwks_uri = get("jwks_uri");
    c.registration_endpoint = get("registration_endpoint");
    c.revocation_endpoint = get("revocation_endpoint");
    c.introspection_endpoint = get("introspection_endpoint");
    c.service_documentation = get("service_documentation");
    c.raw = Json::parse(data.dump());

    if (c.strict)
    {
        const auto fail = [&data](const std::string& message)
        {
            std::vector<LineError> e{{"", "value_error", "Value error, " + message, data}};
            throw std::invalid_argument(pyjson::validation_error_str("OIDCConfiguration", e));
        };
        for (const char* name : {"issuer", "authorization_endpoint", "token_endpoint", "jwks_uri"})
        {
            const auto v = get(name);
            if (!truthy(v))
                fail(std::string("Missing required configuration metadata: ") + name);
            if (!pu::validate_url(*v, false, true).url)
                fail(std::string("Invalid URL for configuration metadata: ") + name);
        }
        for (const char* name : {"response_types_supported", "subject_types_supported",
                                 "id_token_signing_alg_values_supported"})
        {
            const auto it = present_lists.find(name);
            if (it == present_lists.end() || !it->second)
                fail(std::string("Missing required configuration metadata: ") + name);
        }
    }
    return c;
}

OIDCConfiguration OIDCConfiguration::get_oidc_configuration(const std::string& config_url,
                                                            std::optional<bool> strict,
                                                            std::optional<int> timeout_seconds,
                                                            HttpFunction http)
{
    const std::string key = config_url + "\n" + (strict ? (*strict ? "1" : "0") : "-") + "\n" +
                            (timeout_seconds ? std::to_string(*timeout_seconds) : "-");
    {
        std::lock_guard<std::mutex> lock(g_oidc_cache_mutex);
        if (const auto it = g_oidc_cache.find(key); it != g_oidc_cache.end())
        {
            if (it->second.expires_at > monotonic_seconds())
            {
                g_oidc_cache_order.splice(g_oidc_cache_order.end(), g_oidc_cache_order,
                                          it->second.position);
                return it->second.config;
            }
            g_oidc_cache_order.erase(it->second.position);
            g_oidc_cache.erase(it);
        }
    }

    if (!http)
        http = client::auth::default_http_function(
            true, std::chrono::seconds(timeout_seconds.value_or(5)));
    client::auth::HttpRequest req;
    req.method = "GET";
    req.url = config_url;
    req.headers["Accept"] = "*/*";
    const auto response = http(req);
    if (const auto err = detail::http_status_error(response, config_url))
        throw std::runtime_error(*err);
    OJ data = pyjson::loads(response.body);
    if (strict)
    {
        if (!data.is_object())
            throw std::invalid_argument("'" + pyjson::type_name(data) +
                                        "' object does not support item assignment");
        data["strict"] = *strict;
    }
    OIDCConfiguration config = from_json(data);

    std::lock_guard<std::mutex> lock(g_oidc_cache_mutex);
    if (const auto it = g_oidc_cache.find(key); it != g_oidc_cache.end())
    {
        g_oidc_cache_order.erase(it->second.position);
        g_oidc_cache.erase(it);
    }
    g_oidc_cache_order.push_back(key);
    g_oidc_cache.emplace(key, OIDCCacheEntry{monotonic_seconds() + kOidcCacheTtl, config,
                                             std::prev(g_oidc_cache_order.end())});
    while (g_oidc_cache.size() > kOidcCacheMaxSize)
    {
        g_oidc_cache.erase(g_oidc_cache_order.front());
        g_oidc_cache_order.pop_front();
    }
    return config;
}

void OIDCConfiguration::clear_cache()
{
    std::lock_guard<std::mutex> lock(g_oidc_cache_mutex);
    g_oidc_cache.clear();
    g_oidc_cache_order.clear();
}

OIDCConfiguration OIDCProxy::validated_config(const OIDCProxyOptions& options)
{
    if (options.config_url.empty())
        throw std::invalid_argument("Missing required config URL");
    if (options.client_id.empty())
        throw std::invalid_argument("Missing required client id");
    if (!truthy(options.client_secret) && !truthy(options.jwt_signing_key) &&
        !truthy(options.jwt_signing_key_bytes))
        throw std::invalid_argument(
            "Either client_secret or jwt_signing_key must be provided. jwt_signing_key is "
            "required when client_secret is omitted (e.g., for PKCE public clients).");
    if (options.base_url.empty())
        throw std::invalid_argument("Missing required base URL");
    if (options.token_verifier)
    {
        if (options.algorithm)
            throw std::invalid_argument(
                "Cannot specify 'algorithm' when providing a custom token_verifier. Configure the "
                "algorithm on your token verifier instead.");
        if (options.required_scopes)
            throw std::invalid_argument(
                "Cannot specify 'required_scopes' when providing a custom token_verifier. "
                "Configure required scopes on your token verifier instead.");
    }
    if (!pu::validate_url(options.config_url, false, true).url)
        throw std::invalid_argument("Invalid config URL: " + options.config_url);
    const std::string url = *pu::validate_url(options.config_url, false, true).url;
    auto config = OIDCConfiguration::get_oidc_configuration(url, options.strict,
                                                            options.timeout_seconds, options.http);
    if (!truthy(config.authorization_endpoint) || !truthy(config.token_endpoint))
        throw std::invalid_argument("Missing required OIDC endpoints");
    return config;
}

OAuthProxyOptions OIDCProxy::proxy_options(OIDCProxyOptions& options,
                                           const OIDCConfiguration& config)
{
    auto verifier = options.token_verifier;
    if (!verifier)
    {
        JWTVerifierOptions v;
        v.jwks_uri = config.jwks_uri.value_or("None");
        v.issuer = config.issuer.value_or("None");
        v.algorithm = options.algorithm;
        const auto& audience = options.verify_id_token
                                   ? std::optional<std::string>(options.client_id)
                                   : options.audience;
        if (audience)
            v.audience = *audience;
        if (!options.verify_id_token)
            v.required_scopes = options.required_scopes;
        if (options.http)
            v.http = options.http;
        verifier = std::make_shared<JWTVerifier>(v);
    }

    OAuthProxyOptions o;
    o.upstream_authorization_endpoint = *config.authorization_endpoint;
    o.upstream_token_endpoint = *config.token_endpoint;
    o.upstream_client_id = options.client_id;
    o.upstream_client_secret = options.client_secret;
    if (truthy(config.revocation_endpoint))
        o.upstream_revocation_endpoint = config.revocation_endpoint;
    o.token_verifier = verifier;
    o.base_url = options.base_url;
    o.resource_base_url = options.resource_base_url;
    o.issuer_url =
        options.issuer_url ? options.issuer_url : std::optional<std::string>(options.base_url);
    o.service_documentation_url = config.service_documentation;
    o.allowed_client_redirect_uris = options.allowed_client_redirect_uris;
    o.valid_scopes = options.valid_scopes;
    o.client_storage = options.client_storage;
    o.jwt_signing_key = options.jwt_signing_key;
    o.jwt_signing_key_bytes = options.jwt_signing_key_bytes;
    o.token_endpoint_auth_method = options.token_endpoint_auth_method;
    o.require_authorization_consent = options.require_authorization_consent;
    o.consent_csp_policy = options.consent_csp_policy;
    o.forward_resource = options.forward_resource;
    o.fallback_access_token_expiry_seconds = options.fallback_access_token_expiry_seconds;
    o.fallback_refresh_token_expiry_seconds = options.fallback_refresh_token_expiry_seconds;
    o.fastmcp_access_token_expiry_seconds = options.fastmcp_access_token_expiry_seconds;
    o.token_expiry_threshold_seconds = options.token_expiry_threshold_seconds;
    o.client_registration_ttl_seconds = options.client_registration_ttl_seconds;
    o.enable_cimd = options.enable_cimd;
    o.identity_assertion = options.identity_assertion;
    o.http = options.http;
    if (truthy(options.redirect_path))
        o.redirect_path = options.redirect_path;

    const auto merge = [](std::vector<std::pair<std::string, std::string>>& into,
                          const std::vector<std::pair<std::string, std::string>>& from)
    {
        for (const auto& [k, v] : from)
        {
            bool replaced = false;
            for (auto& [ik, iv] : into)
                if (ik == k)
                {
                    iv = v;
                    replaced = true;
                }
            if (!replaced)
                into.emplace_back(k, v);
        }
    };
    if (truthy(options.audience))
    {
        o.extra_authorize_params.emplace_back("audience", *options.audience);
        o.extra_token_params.emplace_back("audience", *options.audience);
    }
    merge(o.extra_authorize_params, options.extra_authorize_params);
    merge(o.extra_token_params, options.extra_token_params);
    return o;
}

OIDCProxy::OIDCProxy(OIDCProxyOptions options) : OIDCProxy(options, validated_config(options)) {}

OIDCProxy::OIDCProxy(OIDCProxyOptions options, OIDCConfiguration config)
    : OAuthProxy(proxy_options(options, config)), oidc_config_(std::move(config)),
      verify_id_token_(options.verify_id_token)
{
    if (verify_id_token_)
    {
        if (options.required_scopes && !options.required_scopes->empty())
            required_scopes_ = *options.required_scopes;
        const auto advertised = options.valid_scopes && !options.valid_scopes->empty()
                                    ? options.valid_scopes
                                    : options.required_scopes;
        if (advertised && !advertised->empty())
            update_default_scopes(*advertised);
    }
}

std::optional<std::string> OIDCProxy::get_verification_token(const UpstreamTokenSet& tokens)
{
    if (verify_id_token_)
    {
        if (tokens.raw_token_data.contains("id_token") &&
            !tokens.raw_token_data["id_token"].is_null())
            return detail::py_str(tokens.raw_token_data["id_token"]);
        std::cerr << "[fastmcpp] verify_id_token is enabled but no id_token found in upstream "
                     "token response\n";
        return std::nullopt;
    }
    return tokens.access_token;
}

bool OIDCProxy::uses_alternate_verification() const
{
    return verify_id_token_;
}

} // namespace fastmcpp::server::auth
