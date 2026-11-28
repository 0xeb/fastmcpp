#pragma once
/// @file server/oauth_upstream.hpp
/// @brief Token-endpoint client the OAuth proxy uses to talk to the upstream
///        identity provider: form-encoded token requests with
///        client_secret_basic, client_secret_post or no client authentication.

#include "fastmcpp/client/auth.hpp"
#include "fastmcpp/types.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fastmcpp::server::auth
{

/// An RFC 6749 section 5.2 error response from the upstream token endpoint.
class UpstreamOAuthError : public std::runtime_error
{
  public:
    UpstreamOAuthError(std::optional<std::string> error, std::optional<std::string> description,
                       std::optional<std::string> uri = std::nullopt);
    std::string error;
    std::string description;
    std::optional<std::string> uri;
};

/// The upstream answered with a 5xx status.
class UpstreamHttpError : public std::runtime_error
{
  public:
    UpstreamHttpError(int status, const std::string& message)
        : std::runtime_error(message), status(status)
    {
    }
    int status;
};

/// Ordered form parameters. Pairs with an empty value are left out of the
/// request, as falsy values are upstream.
using TokenParams = std::vector<std::pair<std::string, std::string>>;

class UpstreamOAuthClient
{
  public:
    /// @p token_endpoint_auth_method defaults to client_secret_basic with a
    /// secret and to none without one.
    /// @throws std::invalid_argument for an unsupported method, when a request is sent.
    UpstreamOAuthClient(std::string client_id, std::optional<std::string> client_secret = {},
                        std::optional<std::string> token_endpoint_auth_method = {},
                        client::auth::HttpFunction http = {},
                        std::chrono::seconds timeout = std::chrono::seconds(30));
    virtual ~UpstreamOAuthClient() = default;

    /// POST grant_type=@p grant_type plus @p params to @p url.
    virtual Json fetch_token(const std::string& url, const TokenParams& params,
                             const std::string& grant_type = "authorization_code");
    /// POST grant_type=refresh_token. When the response carries no
    /// refresh_token, @p refresh_token is added to it (null when absent).
    virtual Json refresh_token(const std::string& url,
                               const std::optional<std::string>& refresh_token,
                               const TokenParams& params = {});

    const std::string& client_id() const
    {
        return client_id_;
    }
    const std::optional<std::string>& client_secret() const
    {
        return client_secret_;
    }
    const std::string& token_endpoint_auth_method() const
    {
        return method_;
    }

  protected:
    virtual Json request_token(const std::string& url, TokenParams data);

  private:
    std::string client_id_;
    std::optional<std::string> client_secret_;
    std::string method_;
    client::auth::HttpFunction http_;
};

} // namespace fastmcpp::server::auth