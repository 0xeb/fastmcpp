#pragma once
/// @file client/header_provider.hpp
/// @brief Per-request header computation for the HTTP client transports.
/// @details Lives in its own header because both transports.hpp and
///          http_rpc_transport.hpp need these types, and transports.hpp
///          includes http_rpc_transport.hpp for StreamableHttpTransport's base.

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fastmcpp::client
{

/// The outgoing request a HeaderProvider is being asked to supply headers for.
///
/// Every view points at storage owned by the caller and is valid only for the
/// duration of the provider call. A provider that needs to retain any of it
/// must copy.
struct HeaderRequest
{
    std::string_view method; ///< HTTP method, e.g. "POST".
    std::string_view url;    ///< Absolute URL the request is going to.
    std::string_view path;   ///< Path component of @ref url.
    std::string_view body;   ///< Serialized request body, empty when there is none.
};

/// Computes headers at request time rather than at construction time.
///
/// The `headers` constructor argument is fixed for the lifetime of a transport,
/// which is a problem for credentials that rotate: OAuth access tokens are
/// typically valid for an hour or less, and an authorization server may revoke
/// one before it expires. Rebuilding the transport to change a header also
/// discards its session state.
///
/// A provider is consulted on every request and its entries are merged over the
/// static map, so a long-lived client can refresh a bearer token in place.
///
/// The provider is handed a description of the request it is supplying headers
/// for. A bearer-token provider can ignore it:
///
/// @code
/// transport.set_header_provider([&auth](const HeaderRequest&) {
///     return std::unordered_map<std::string, std::string>{
///         {"Authorization", "Bearer " + auth.current_token()}};
/// });
/// @endcode
///
/// while a scheme that signs the request -- AWS SigV4, HTTP Message Signatures
/// -- needs the method, path and body, which is why they are passed rather than
/// left for a later, breaking, addition.
using HeaderProvider =
    std::function<std::unordered_map<std::string, std::string>(const HeaderRequest&)>;

} // namespace fastmcpp::client
