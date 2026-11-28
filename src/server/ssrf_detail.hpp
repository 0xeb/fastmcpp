#pragma once
// Test seams for server/ssrf: replace DNS resolution and the network fetch so
// every branch can be exercised without real HTTPS endpoints.

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fastmcpp::server::auth::ssrf::detail
{

/// One connection attempt.
struct Attempt
{
    /// The URL this attempt stands for: https://<ip>:<port><path> when
    /// pinned, the original URL in proxy-trust mode.
    std::string url;
    std::string hostname;
    int port = 443;
    std::string path;
    /// Address the connection is pinned to; nullopt in proxy-trust mode.
    std::optional<std::string> pinned_ip;
    std::optional<std::string> proxy_url;
    /// Request headers, including Host when pinned.
    std::map<std::string, std::string> headers;
    /// Timeout for connecting and for each read, in seconds.
    double timeout = 10.0;
};

/// Receives the response as it streams in. Either callback may throw to
/// abort the transfer; the exception reaches the caller unchanged.
struct Sink
{
    std::function<void(int status, const std::map<std::string, std::string>& headers)> on_headers;
    std::function<void(const char* data, std::size_t size)> on_data;
};

/// The connection failed or timed out; the next address is tried.
class ConnectFailure : public std::runtime_error
{
  public:
    ConnectFailure(const std::string& message, bool timed_out)
        : std::runtime_error(message), timed_out_(timed_out)
    {
    }
    bool timed_out() const
    {
        return timed_out_;
    }

  private:
    bool timed_out_;
};

using Resolver = std::function<std::vector<std::string>(const std::string& host, int port)>;
using Fetcher = std::function<void(const Attempt&, const Sink&)>;

/// Replace DNS resolution; an empty function restores getaddrinfo.
void set_resolver(Resolver resolver);
/// Replace the network fetch; an empty function restores the platform backend.
void set_fetcher(Fetcher fetcher);

} // namespace fastmcpp::server::auth::ssrf::detail
