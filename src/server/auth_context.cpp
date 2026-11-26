#include "fastmcpp/server/auth_context.hpp"

#include <algorithm>
#include <chrono>

namespace fastmcpp::server
{

namespace
{
thread_local std::shared_ptr<const AccessToken> t_access_token;

std::vector<std::string> string_list(const Json& value)
{
    std::vector<std::string> out;
    if (value.is_array())
        for (const auto& v : value)
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}
} // namespace

TokenVerifier static_token_verifier(std::map<std::string, Json> tokens,
                                    std::vector<std::string> required_scopes)
{
    return [tokens = std::move(tokens), required_scopes = std::move(required_scopes)](
               const std::string& token) -> std::optional<AccessToken>
    {
        auto it = tokens.find(token);
        if (it == tokens.end() || !it->second.is_object())
            return std::nullopt;
        const Json& data = it->second;

        AccessToken out;
        out.token = token;
        out.client_id = data.value("client_id", std::string());
        out.scopes = string_list(data.value("scopes", Json::array()));
        if (data.contains("expires_at") && data["expires_at"].is_number())
        {
            out.expires_at = data["expires_at"].get<std::int64_t>();
            const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            if (*out.expires_at < now)
                return std::nullopt;
        }
        for (const auto& scope : required_scopes)
            if (std::find(out.scopes.begin(), out.scopes.end(), scope) == out.scopes.end())
                return std::nullopt;
        if (data.contains("sub") && data["sub"].is_string())
            out.subject = data["sub"].get<std::string>();
        out.claims = data;
        return out;
    };
}

const AccessToken* current_access_token()
{
    return t_access_token.get();
}

std::shared_ptr<const AccessToken> current_access_token_handle()
{
    return t_access_token;
}

AccessTokenScope::AccessTokenScope(std::shared_ptr<const AccessToken> token)
    : previous_(std::move(t_access_token))
{
    t_access_token = std::move(token);
}

AccessTokenScope::~AccessTokenScope()
{
    t_access_token = std::move(previous_);
}

std::optional<std::string> current_principal()
{
    const AccessToken* token = current_access_token();
    if (!token)
        return std::nullopt;
    Json issuer = nullptr;
    if (token->claims.is_object() && token->claims.contains("iss") &&
        !token->claims["iss"].is_null())
    {
        const Json& iss = token->claims["iss"];
        issuer = iss.is_string() ? iss.get<std::string>() : iss.dump();
    }
    Json triple = Json::array(
        {token->client_id, issuer, token->subject ? Json(*token->subject) : Json(nullptr)});
    return triple.dump();
}

} // namespace fastmcpp::server
