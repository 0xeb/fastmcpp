#include "fastmcpp/client/transports.hpp"

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"

#include <chrono>
#include <easywsclient.hpp>
#include <fstream>
#include <httplib.h>
#include <sstream>
#include <thread>
#ifdef FASTMCPP_POST_STREAMING
#include <curl/curl.h>
#endif
#ifdef TINY_PROCESS_LIB_AVAILABLE
#include <process.hpp>
#endif

namespace fastmcpp::client
{

namespace
{
struct ParsedUrl
{
    std::string scheme; // "http" or "https"
    std::string host;
    int port;
    bool is_https;
};

struct ParsedUrlWithPath
{
    ParsedUrl base;
    std::string path; // includes leading '/'
};

ParsedUrl parse_url(const std::string& base)
{
    ParsedUrl result;
    std::string remaining = base;

    // Extract scheme
    auto scheme_pos = remaining.find("://");
    if (scheme_pos != std::string::npos)
    {
        result.scheme = remaining.substr(0, scheme_pos);
        remaining = remaining.substr(scheme_pos + 3);
    }
    else
    {
        // Default to http if no scheme specified
        result.scheme = "http";
    }

    // Validate scheme (only allow http/https)
    if (result.scheme != "http" && result.scheme != "https")
    {
        throw fastmcpp::TransportError("Unsupported URL scheme: " + result.scheme +
                                       " (only http and https are allowed)");
    }

    result.is_https = (result.scheme == "https");

    // If path segments exist, strip them
    auto slash_pos = remaining.find('/');
    if (slash_pos != std::string::npos)
        remaining = remaining.substr(0, slash_pos);

    // Extract port if provided
    auto colon_pos = remaining.rfind(':');
    if (colon_pos != std::string::npos)
    {
        std::string port_str = remaining.substr(colon_pos + 1);
        result.host = remaining.substr(0, colon_pos);
        try
        {
            result.port = std::stoi(port_str);
        }
        catch (...)
        {
            // Use default port for scheme
            result.port = result.is_https ? 443 : 80;
        }
    }
    else
    {
        result.host = remaining;
        // Use default port for scheme
        result.port = result.is_https ? 443 : 80;
    }

    return result;
}

ParsedUrlWithPath parse_url_with_path(const std::string& url)
{
    ParsedUrlWithPath out;
    out.base = parse_url(url);

    // Extract path (if any) from the original string, preserving query/fragment (if present)
    auto scheme_pos = url.find("://");
    size_t host_start = (scheme_pos == std::string::npos) ? 0 : (scheme_pos + 3);
    auto path_pos = url.find('/', host_start);
    if (path_pos == std::string::npos)
        out.path = "/";
    else
        out.path = url.substr(path_pos);

    if (out.path.empty() || out.path[0] != '/')
        out.path.insert(out.path.begin(), '/');

    return out;
}

bool is_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

std::pair<std::string, std::string> resolve_redirect_target(const std::string& current_full_url,
                                                            const std::string& current_path,
                                                            const std::string& location)
{
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0)
    {
        auto parsed = parse_url_with_path(location);
        std::string full_url =
            parsed.base.scheme + "://" + parsed.base.host + ":" + std::to_string(parsed.base.port);
        return {std::move(full_url), parsed.path};
    }

    if (!location.empty() && location[0] == '/')
        return {current_full_url, location};

    // Relative redirect - resolve against current path
    std::string base_dir = "/";
    auto last_slash = current_path.rfind('/');
    if (last_slash != std::string::npos)
        base_dir = current_path.substr(0, last_slash + 1);
    return {current_full_url, base_dir + location};
}
} // namespace

fastmcpp::Json HttpTransport::request(const std::string& route, const fastmcpp::Json& payload)
{
    auto url = parse_url(base_url_);

    // Security: Create client with full scheme://host:port URL for proper TLS handling
    std::string full_url = url.scheme + "://" + url.host + ":" + std::to_string(url.port);
    httplib::Client cli(full_url.c_str());

    cli.set_connection_timeout(5, 0);
    cli.set_keep_alive(true);
    cli.set_read_timeout(10, 0);

    // Security: Disable redirects by default to prevent SSRF and TLS downgrade attacks
    cli.set_follow_location(false);

    cli.set_default_headers({{"Accept", "text/event-stream, application/json"}});
    auto res = cli.Post(("/" + route).c_str(), payload.dump(), "application/json");
    if (!res)
        throw fastmcpp::TransportError("HTTP request failed: no response");
    if (res->status < 200 || res->status >= 300)
        throw fastmcpp::TransportError("HTTP error: " + std::to_string(res->status));
    return fastmcpp::util::json::parse(res->body);
}

void HttpTransport::request_stream(const std::string& route, const fastmcpp::Json& /*payload*/,
                                   const std::function<void(const fastmcpp::Json&)>& on_event)
{
    auto url = parse_url(base_url_);

    // Security: Create client with full scheme://host:port URL for proper TLS handling
    std::string full_url = url.scheme + "://" + url.host + ":" + std::to_string(url.port);
    httplib::Client cli(full_url.c_str());

    cli.set_connection_timeout(5, 0);
    cli.set_keep_alive(true);
    cli.set_read_timeout(10, 0);

    std::string path = "/" + route;
    httplib::Headers headers = {{"Accept", "text/event-stream, application/json"}};

    std::string buffer;
    std::string last_emitted;
    std::atomic<bool> any_data{false};
    auto content_receiver = [&](const char* data, size_t len)
    {
        any_data.store(true, std::memory_order_relaxed);
        buffer.append(data, len);
        // Try to parse SSE-style events separated by double newlines
        size_t pos = 0;
        while (true)
        {
            size_t sep = buffer.find("\n\n", pos);
            if (sep == std::string::npos)
                break;
            std::string chunk = buffer.substr(pos, sep - pos);
            pos = sep + 2;

            // Extract data: lines
            std::string aggregated;
            size_t line_start = 0;
            while (line_start < chunk.size())
            {
                size_t line_end = chunk.find('\n', line_start);
                std::string line = chunk.substr(line_start, line_end == std::string::npos
                                                                ? std::string::npos
                                                                : (line_end - line_start));
                if (line.rfind("data:", 0) == 0)
                {
                    std::string data_part = line.substr(5);
                    if (!data_part.empty() && data_part[0] == ' ')
                        data_part.erase(0, 1);
                    aggregated += data_part;
                }
                if (line_end == std::string::npos)
                    break;
                line_start = line_end + 1;
            }

            if (!aggregated.empty())
            {
                // De-duplicate identical consecutive chunks to avoid repeated delivery
                if (aggregated == last_emitted)
                {
                    // skip duplicate
                }
                else
                {
                    last_emitted = aggregated;
                    try
                    {
                        auto evt = fastmcpp::util::json::parse(aggregated);
                        if (on_event)
                            on_event(evt);
                    }
                    catch (...)
                    {
                        // Fallback: deliver raw chunk as text if not JSON
                        fastmcpp::Json item =
                            fastmcpp::Json{{"type", "text"}, {"text", aggregated}};
                        fastmcpp::Json evt =
                            fastmcpp::Json{{"content", fastmcpp::Json::array({item})}};
                        if (on_event)
                            on_event(evt);
                    }
                }
            }
        }
        // Erase processed portion
        if (pos > 0)
            buffer.erase(0, pos);
        return true; // continue
    };

    auto response_handler = [&](const httplib::Response& r)
    {
        // Accept only 200 and event-stream or json
        return r.status >= 200 && r.status < 300;
    };
    // Retry for a short window in case the server isn't immediately ready
    httplib::Result res;
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        // First, try full handler variant
        res = cli.Get(path.c_str(), headers, response_handler, content_receiver);
        if (res)
            break;
        // Fallback: some environments behave better with the simpler overload
        res = cli.Get(path.c_str(), content_receiver);
        if (res)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!res)
    {
        // Some environments may close the connection without a formal response
        // even though chunks were delivered. If we received any data, treat as ok.
        if (any_data.load(std::memory_order_relaxed))
            return;
        throw fastmcpp::TransportError("HTTP stream request failed: no response");
    }
    if (res->status < 200 || res->status >= 300)
        throw fastmcpp::TransportError("HTTP stream error: " + std::to_string(res->status));
}

void HttpTransport::request_stream_post(const std::string& route, const fastmcpp::Json& payload,
                                        const std::function<void(const fastmcpp::Json&)>& on_event)
{
#ifdef FASTMCPP_POST_STREAMING
    CURL* curl = curl_easy_init();
    if (!curl)
        throw fastmcpp::TransportError("libcurl init failed");

    std::string url = base_url_;
    if (url.find("://") == std::string::npos)
        url = "http://" + url;
    if (!url.empty() && url.back() != '/')
        url.push_back('/');
    url += route;

    std::string body = payload.dump();
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream, application/json");

    std::string buffer;
    auto parse_and_emit = [&](bool flush_all = false)
    {
        size_t pos = 0;
        while (true)
        {
            size_t sep = buffer.find("\n\n", pos);
            if (sep == std::string::npos)
                break;
            std::string chunk = buffer.substr(pos, sep - pos);
            pos = sep + 2;

            std::string aggregated;
            size_t line_start = 0;
            while (line_start < chunk.size())
            {
                size_t line_end = chunk.find('\n', line_start);
                std::string line = chunk.substr(line_start, line_end == std::string::npos
                                                                ? std::string::npos
                                                                : (line_end - line_start));
                if (line.rfind("data:", 0) == 0)
                {
                    std::string data_part = line.substr(5);
                    if (!data_part.empty() && data_part[0] == ' ')
                        data_part.erase(0, 1);
                    aggregated += data_part;
                }
                if (line_end == std::string::npos)
                    break;
                line_start = line_end + 1;
            }
            if (!aggregated.empty())
            {
                try
                {
                    auto evt = fastmcpp::util::json::parse(aggregated);
                    if (on_event)
                        on_event(evt);
                }
                catch (...)
                {
                    fastmcpp::Json item = fastmcpp::Json{{"type", "text"}, {"text", aggregated}};
                    fastmcpp::Json evt = fastmcpp::Json{{"content", fastmcpp::Json::array({item})}};
                    if (on_event)
                        on_event(evt);
                }
            }
        }
        if (flush_all && pos < buffer.size())
        {
            std::string rest = buffer.substr(pos);
            if (!rest.empty())
            {
                try
                {
                    auto evt = fastmcpp::util::json::parse(rest);
                    if (on_event)
                        on_event(evt);
                }
                catch (...)
                {
                    fastmcpp::Json item = fastmcpp::Json{{"type", "text"}, {"text", rest}};
                    fastmcpp::Json evt = fastmcpp::Json{{"content", fastmcpp::Json::array({item})}};
                    if (on_event)
                        on_event(evt);
                }
            }
            buffer.clear();
        }
        if (pos > 0)
            buffer.erase(0, pos);
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t
        {
            auto* buf = static_cast<std::string*>(userdata);
            buf->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // no overall timeout

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // Parse whatever accumulated
    parse_and_emit(true);

    if (code != CURLE_OK && code != CURLE_PARTIAL_FILE)
    {
        throw fastmcpp::TransportError(std::string("HTTP stream POST failed: ") +
                                       curl_easy_strerror(code));
    }
    if (status < 200 || status >= 300)
        throw fastmcpp::TransportError("HTTP stream POST error: " + std::to_string(status));
#else
    (void)route;
    (void)payload;
    (void)on_event;
    throw fastmcpp::TransportError(
        "libcurl not available; POST streaming unsupported in this build");
#endif
}

fastmcpp::Json WebSocketTransport::request(const std::string& route, const fastmcpp::Json& payload)
{
    using easywsclient::WebSocket;
    std::string full = url_;
    if (!full.empty() && full.back() != '/')
        full.push_back('/');
    full += route;
    std::unique_ptr<WebSocket> ws(WebSocket::from_url(full));
    if (!ws)
        throw fastmcpp::TransportError("WS connect failed: " + full);
    ws->send(payload.dump());
    std::string resp;
    bool got = false;
    auto onmsg = [&](const std::string& msg)
    {
        resp = msg;
        got = true;
    };
    // Wait up to ~2s
    for (int i = 0; i < 40 && !got; ++i)
    {
        ws->poll(50);
        ws->dispatch(onmsg);
    }
    ws->close();
    if (!got)
        throw fastmcpp::TransportError("WS no response");
    return fastmcpp::util::json::parse(resp);
}

void WebSocketTransport::request_stream(const std::string& route, const fastmcpp::Json& payload,
                                        const std::function<void(const fastmcpp::Json&)>& on_event)
{
    using easywsclient::WebSocket;
    std::string full = url_;
    if (!full.empty() && full.back() != '/')
        full.push_back('/');
    full += route;
    std::unique_ptr<WebSocket> ws(WebSocket::from_url(full));
    if (!ws)
        throw fastmcpp::TransportError("WS connect failed: " + full);

    // Send initial payload
    ws->send(payload.dump());

    // Pump loop: dispatch frames for a reasonable period or until closed
    // Stop after a short idle timeout window to avoid hanging indefinitely
    std::string frame;
    auto onmsg = [&](const std::string& msg) { frame = msg; };

    const int max_iters = 400; // ~20s total at 50ms per poll
    int idle_iters = 0;
    for (int i = 0; i < max_iters; ++i)
    {
        ws->poll(50);
        frame.clear();
        ws->dispatch(onmsg);
        if (!frame.empty())
        {
            try
            {
                auto evt = fastmcpp::util::json::parse(frame);
                if (on_event)
                    on_event(evt);
            }
            catch (...)
            {
                fastmcpp::Json item = fastmcpp::Json{{"type", "text"}, {"text", frame}};
                fastmcpp::Json evt = fastmcpp::Json{{"content", fastmcpp::Json::array({item})}};
                if (on_event)
                    on_event(evt);
            }
            idle_iters = 0; // reset idle counter on data
        }
        else
        {
            // No message arrived in this poll slice
            if (++idle_iters > 60)
            {
                // ~3s idle without frames → assume stream done
                break;
            }
        }
    }
    ws->close();
}

fastmcpp::Json StdioTransport::request(const std::string& route, const fastmcpp::Json& payload)
{
    // Use TinyProcessLibrary (fetched via CMake) for cross-platform subprocess handling
    // Build command line
    std::ostringstream cmd;
    cmd << command_;
    for (const auto& a : args_)
        cmd << " " << a;

#ifdef TINY_PROCESS_LIB_AVAILABLE
    using namespace TinyProcessLib;
    std::string stdout_data;
    std::string stderr_data;

    // Open log file if path was provided (RAII - closes automatically)
    std::ofstream log_file_stream;
    std::ostream* stderr_target = nullptr;

    if (log_file_.has_value())
    {
        // Open file in append mode
        log_file_stream.open(log_file_.value(), std::ios::app);
        if (log_file_stream.is_open())
            stderr_target = &log_file_stream;
    }
    else if (log_stream_ != nullptr)
    {
        stderr_target = log_stream_;
    }

    // Stderr callback: write to log file/stream if configured, otherwise capture
    auto stderr_callback = [&](const char* bytes, size_t n)
    {
        if (stderr_target != nullptr)
        {
            stderr_target->write(bytes, n);
            stderr_target->flush();
        }
        // Always capture for error messages (in case of process failure)
        stderr_data.append(bytes, n);
    };

    Process process(
        cmd.str(), "", [&](const char* bytes, size_t n) { stdout_data.append(bytes, n); },
        stderr_callback, true);

    // Write single-line JSON-RPC request
    fastmcpp::Json request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", route},
        {"params", payload},
    };
    const std::string line = request.dump() + "\n";
    process.write(line);
    process.close_stdin();
    int exit_code = process.get_exit_status();
    if (exit_code != 0)
    {
        throw fastmcpp::TransportError(
            "StdioTransport process exit code: " + std::to_string(exit_code) +
            (stderr_data.empty() ? std::string("") : ("; stderr: ") + stderr_data));
    }
    // Read first line from stdout_data
    auto pos = stdout_data.find('\n');
    std::string first_line = pos == std::string::npos ? stdout_data : stdout_data.substr(0, pos);
    if (first_line.empty())
        throw fastmcpp::TransportError("StdioTransport: no response");
    return fastmcpp::util::json::parse(first_line);
#else
    throw fastmcpp::TransportError("TinyProcessLib is not integrated; cannot run StdioTransport");
#endif
}

// =============================================================================
// SseClientTransport implementation
// =============================================================================

SseClientTransport::SseClientTransport(std::string base_url, std::string sse_path,
                                       std::string messages_path)
    : base_url_(std::move(base_url)), sse_path_(std::move(sse_path)),
      messages_path_(std::move(messages_path))
{
    start_sse_listener();
}

SseClientTransport::~SseClientTransport()
{
    stop_sse_listener();
}

bool SseClientTransport::is_connected() const
{
    return connected_.load(std::memory_order_acquire);
}

std::string SseClientTransport::session_id() const
{
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    return session_id_;
}

bool SseClientTransport::has_session() const
{
    std::lock_guard<std::mutex> lock(endpoint_mutex_);
    return !session_id_.empty();
}

void SseClientTransport::set_server_request_handler(ServerRequestHandler handler)
{
    std::lock_guard<std::mutex> lock(request_handler_mutex_);
    server_request_handler_ = std::move(handler);
}

void SseClientTransport::reset(bool /*full*/)
{
    stop_sse_listener();
    {
        std::lock_guard<std::mutex> lock(endpoint_mutex_);
        endpoint_path_.clear();
        session_id_.clear();
    }
    start_sse_listener();
}

void SseClientTransport::start_sse_listener()
{
    running_.store(true, std::memory_order_release);

    sse_thread_ = std::make_unique<std::thread>(
        [this]()
        {
            auto url = parse_url(base_url_);

            // Use two-argument constructor for better Windows compatibility
            httplib::Client cli(url.host.c_str(), url.port);
            cli.set_connection_timeout(10, 0);
            cli.set_read_timeout(300, 0); // Long timeout for SSE stream (5 minutes)
            cli.set_keep_alive(true);

            std::string buffer;
            auto content_receiver = [this, &buffer](const char* data, size_t len)
            {
                if (!running_.load(std::memory_order_acquire))
                    return false;

                buffer.append(data, len);

                // Parse SSE events (data: lines separated by double newlines)
                size_t pos = 0;
                while (true)
                {
                    // Try both \n\n (Unix) and \r\n\r\n (Windows/HTTP) for compatibility
                    size_t sep = buffer.find("\n\n", pos);
                    int sep_len = 2;
                    if (sep == std::string::npos)
                    {
                        sep = buffer.find("\r\n\r\n", pos);
                        sep_len = 4;
                    }
                    if (sep == std::string::npos)
                        break;

                    std::string chunk = buffer.substr(pos, sep - pos);
                    pos = sep + sep_len;

                    // Extract event type and data lines
                    std::string event_type;
                    std::string aggregated;
                    size_t line_start = 0;
                    while (line_start < chunk.size())
                    {
                        size_t line_end = chunk.find('\n', line_start);
                        std::string line = chunk.substr(line_start, line_end == std::string::npos
                                                                        ? std::string::npos
                                                                        : (line_end - line_start));
                        // Strip trailing \r for CRLF line endings
                        if (!line.empty() && line.back() == '\r')
                            line.pop_back();

                        if (line.rfind("event:", 0) == 0)
                        {
                            event_type = line.substr(6);
                            if (!event_type.empty() && event_type[0] == ' ')
                                event_type.erase(0, 1);
                        }
                        else if (line.rfind("data:", 0) == 0)
                        {
                            std::string data_part = line.substr(5);
                            if (!data_part.empty() && data_part[0] == ' ')
                                data_part.erase(0, 1);
                            aggregated += data_part;
                        }
                        if (line_end == std::string::npos)
                            break;
                        line_start = line_end + 1;
                    }

                    if (!aggregated.empty())
                    {
                        // Handle endpoint event specially - it's not JSON
                        if (event_type == "endpoint")
                        {
                            std::lock_guard<std::mutex> lock(endpoint_mutex_);
                            endpoint_path_ = aggregated;

                            // Parse session_id from "...?session_id=<id>"
                            session_id_.clear();
                            auto pos = endpoint_path_.find("session_id=");
                            if (pos != std::string::npos)
                            {
                                pos += std::string("session_id=").size();
                                auto end = endpoint_path_.find_first_of("&#", pos);
                                session_id_ = endpoint_path_.substr(pos, end == std::string::npos
                                                                             ? std::string::npos
                                                                             : (end - pos));
                            }
                        }
                        else
                        {
                            // Try to parse as JSON for other events
                            try
                            {
                                auto evt = fastmcpp::util::json::parse(aggregated);
                                process_sse_event(evt);
                            }
                            catch (...)
                            {
                                // Ignore parse errors
                            }
                        }
                    }
                }

                if (pos > 0)
                    buffer.erase(0, pos);

                return running_.load(std::memory_order_acquire);
            };

            auto response_handler = [this](const httplib::Response& r)
            {
                if (r.status >= 200 && r.status < 300)
                {
                    connected_.store(true, std::memory_order_release);
                    return true;
                }
                return false;
            };

            // Try to connect with retries
            httplib::Headers headers = {{"Accept", "text/event-stream"}};
            for (int attempt = 0; attempt < 50 && running_.load(std::memory_order_acquire);
                 ++attempt)
            {
                auto res = cli.Get(sse_path_.c_str(), headers, response_handler, content_receiver);
                if (res)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            connected_.store(false, std::memory_order_release);
        });

    // Wait for connection to establish
    for (int i = 0; i < 50 && !is_connected(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void SseClientTransport::stop_sse_listener()
{
    running_.store(false, std::memory_order_release);

    // Wake up any pending requests
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        for (auto& [id, promise] : pending_requests_)
        {
            try
            {
                promise.set_exception(
                    std::make_exception_ptr(fastmcpp::TransportError("SSE connection closed")));
            }
            catch (...)
            {
            }
        }
        pending_requests_.clear();
    }
    pending_cv_.notify_all();

    if (sse_thread_ && sse_thread_->joinable())
        sse_thread_->join();
}

void SseClientTransport::process_sse_event(const fastmcpp::Json& event)
{
    if (!event.contains("id"))
        return;

    const fastmcpp::Json id_val = event.at("id");

    // First: try to treat as a response to a client-initiated request.
    std::optional<int64_t> numeric_id;
    if (id_val.is_number_integer())
        numeric_id = id_val.get<int64_t>();
    else if (id_val.is_string())
    {
        try
        {
            numeric_id = std::stoll(id_val.get<std::string>());
        }
        catch (...)
        {
        }
    }

    if (numeric_id.has_value())
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_requests_.find(*numeric_id);
        if (it != pending_requests_.end())
        {
            it->second.set_value(event);
            pending_requests_.erase(it);
            pending_cv_.notify_all();
            return;
        }
    }

    // Otherwise: treat as a server-initiated request (e.g., sampling/createMessage).
    if (!event.contains("method"))
        return;

    const std::string method = event.value("method", std::string());
    const fastmcpp::Json params = event.value("params", fastmcpp::Json::object());

    ServerRequestHandler handler;
    {
        std::lock_guard<std::mutex> lock(request_handler_mutex_);
        handler = server_request_handler_;
    }

    fastmcpp::Json rpc_response = {{"jsonrpc", "2.0"}, {"id", id_val}};
    if (!handler)
    {
        rpc_response["error"] = {{"code", -32601}, {"message", "Method not handled: " + method}};
    }
    else
    {
        try
        {
            rpc_response["result"] = handler(method, params);
        }
        catch (const std::exception& e)
        {
            rpc_response["error"] = {{"code", -32603}, {"message", e.what()}};
        }
        catch (...)
        {
            rpc_response["error"] = {{"code", -32603}, {"message", "Unknown error"}};
        }
    }

    try
    {
        auto url = parse_url(base_url_);
        httplib::Client cli(url.host.c_str(), url.port);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(30, 0);

        std::string post_path;
        {
            std::lock_guard<std::mutex> lock(endpoint_mutex_);
            post_path = endpoint_path_.empty() ? messages_path_ : endpoint_path_;
        }
        (void)cli.Post(post_path.c_str(), rpc_response.dump(), "application/json");
    }
    catch (...)
    {
        // Best-effort: ignore failures in the listener thread.
    }
}

fastmcpp::Json SseClientTransport::request(const std::string& route, const fastmcpp::Json& payload)
{
    if (!is_connected())
        throw fastmcpp::TransportError("SSE client not connected");

    // Generate unique request ID
    int64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

    // Build JSON-RPC request
    fastmcpp::Json rpc_request = {
        {"jsonrpc", "2.0"}, {"method", route}, {"params", payload}, {"id", id}};

    // Create promise for response
    std::promise<fastmcpp::Json> response_promise;
    std::future<fastmcpp::Json> response_future = response_promise.get_future();

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[id] = std::move(response_promise);
    }

    // Send request via POST to /messages with session_id
    auto url = parse_url(base_url_);
    httplib::Client cli(url.host.c_str(), url.port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);

    // Use the endpoint path from SSE if available, otherwise use default
    std::string post_path;
    {
        std::lock_guard<std::mutex> lock(endpoint_mutex_);
        post_path = endpoint_path_.empty() ? messages_path_ : endpoint_path_;
    }
    auto res = cli.Post(post_path.c_str(), rpc_request.dump(), "application/json");
    if (!res)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(id);
        throw fastmcpp::TransportError("Failed to send request to " + messages_path_);
    }
    if (res->status < 200 || res->status >= 300)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(id);
        throw fastmcpp::TransportError("HTTP error: " + std::to_string(res->status));
    }

    // Wait for response from SSE stream (with timeout)
    auto status = response_future.wait_for(std::chrono::seconds(30));
    if (status == std::future_status::timeout)
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_.erase(id);
        throw fastmcpp::TransportError("Request timeout waiting for SSE response");
    }

    auto rpc_response = response_future.get();

    // Unwrap JSON-RPC response envelope
    // Response format: {"jsonrpc":"2.0","id":...,"result":{...}} or
    // {"jsonrpc":"2.0","id":...,"error":{...}}
    if (rpc_response.contains("error"))
    {
        auto error = rpc_response["error"];
        std::string message = error.value("message", "Unknown error");
        throw fastmcpp::TransportError("JSON-RPC error: " + message);
    }

    if (rpc_response.contains("result"))
        return rpc_response["result"];

    // If no result or error, return empty object (shouldn't happen with well-formed JSON-RPC)
    return fastmcpp::Json::object();
}

// =============================================================================
// StreamableHttpTransport implementation
// =============================================================================

StreamableHttpTransport::StreamableHttpTransport(
    std::string base_url, std::string mcp_path,
    std::unordered_map<std::string, std::string> headers)
    : base_url_(std::move(base_url)), mcp_path_(std::move(mcp_path)), headers_(std::move(headers))
{
}

StreamableHttpTransport::~StreamableHttpTransport() = default;

std::string StreamableHttpTransport::session_id() const
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    return session_id_;
}

bool StreamableHttpTransport::has_session() const
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    return !session_id_.empty();
}

void StreamableHttpTransport::set_notification_callback(
    std::function<void(const fastmcpp::Json&)> callback)
{
    notification_callback_ = std::move(callback);
}

void StreamableHttpTransport::reset(bool /*full*/)
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_id_.clear();
}

void StreamableHttpTransport::parse_session_id_from_response(const std::string& header_value)
{
    // The header comes as "Mcp-Session-Id: <value>"
    // We receive just the value from httplib
    if (!header_value.empty())
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_ = header_value;
    }
}

void StreamableHttpTransport::process_sse_line(const std::string& line,
                                               std::vector<fastmcpp::Json>& messages)
{
    // Parse SSE data lines (format: "data: {json}")
    if (line.rfind("data:", 0) == 0)
    {
        std::string data_part = line.substr(5);
        if (!data_part.empty() && data_part[0] == ' ')
            data_part.erase(0, 1);
        if (!data_part.empty())
        {
            try
            {
                auto json = fastmcpp::util::json::parse(data_part);
                messages.push_back(std::move(json));
            }
            catch (...)
            {
                // Ignore parse errors
            }
        }
    }
}

fastmcpp::Json StreamableHttpTransport::parse_response(const std::string& body,
                                                       const std::string& content_type)
{
    // Check if response is SSE stream
    bool is_sse = content_type.find("text/event-stream") != std::string::npos;

    if (is_sse)
    {
        // Parse SSE events and collect all JSON messages
        std::vector<fastmcpp::Json> messages;
        std::istringstream stream(body);
        std::string line;

        while (std::getline(stream, line))
        {
            // Strip trailing \r for CRLF
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            process_sse_line(line, messages);
        }

        // Process messages - notifications go to callback, find the main response
        fastmcpp::Json response;
        for (const auto& msg : messages)
        {
            // Check if this is a notification (has method, no id)
            if (msg.contains("method") && !msg.contains("id"))
            {
                if (notification_callback_)
                    notification_callback_(msg);
            }
            else if (msg.contains("id"))
            {
                // This is the response we're looking for
                response = msg;
            }
        }

        return response;
    }
    else
    {
        // Plain JSON response
        return fastmcpp::util::json::parse(body);
    }
}

fastmcpp::Json StreamableHttpTransport::request(const std::string& route,
                                                const fastmcpp::Json& payload)
{
    auto url = parse_url(base_url_);

    // Create client
    std::string full_url = url.scheme + "://" + url.host + ":" + std::to_string(url.port);

    // Build request headers
    httplib::Headers request_headers = {{"Accept", "application/json, text/event-stream"},
                                        {"Content-Type", "application/json"}};

    // Add custom headers
    for (const auto& [key, value] : headers_)
        request_headers.emplace(key, value);

    // Add session ID if we have one
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (!session_id_.empty())
            request_headers.emplace("Mcp-Session-Id", session_id_);
    }

    // Build JSON-RPC request (route is method, payload is params)
    int64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
    fastmcpp::Json rpc_request = {
        {"jsonrpc", "2.0"}, {"method", route}, {"params", payload}, {"id", id}};

    std::string path = mcp_path_.empty() ? "/mcp" : mcp_path_;
    if (!path.empty() && path[0] != '/')
        path.insert(path.begin(), '/');

    // Send request (follow redirects like the Python SDK's httpx follow_redirects=True)
    httplib::Result res;
    for (int redirects = 0; redirects <= 5; ++redirects)
    {
        httplib::Client cli(full_url.c_str());
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(60, 0); // Longer timeout for streaming
        cli.set_keep_alive(true);
        cli.set_follow_location(false);

        res = cli.Post(path.c_str(), request_headers, rpc_request.dump(), "application/json");
        if (!res)
            throw fastmcpp::TransportError("StreamableHttp request failed: no response");

        if (is_redirect_status(res->status))
        {
            auto loc = res->headers.find("Location");
            if (loc == res->headers.end())
                loc = res->headers.find("location");
            if (loc == res->headers.end())
                throw fastmcpp::TransportError("StreamableHttp redirect without Location header");

            auto next = resolve_redirect_target(full_url, path, loc->second);
            full_url = std::move(next.first);
            path = std::move(next.second);
            continue;
        }

        break;
    }

    if (!res)
        throw fastmcpp::TransportError("StreamableHttp request failed: no response");

    if (is_redirect_status(res->status))
        throw fastmcpp::TransportError("StreamableHttp redirect limit exceeded");

    if (res->status < 200 || res->status >= 300)
        throw fastmcpp::TransportError("StreamableHttp error: " + std::to_string(res->status));

    // Check for session ID in response headers
    auto session_header = res->headers.find("Mcp-Session-Id");
    if (session_header != res->headers.end())
        parse_session_id_from_response(session_header->second);

    // Also check lowercase (some servers may use different casing)
    session_header = res->headers.find("mcp-session-id");
    if (session_header != res->headers.end())
        parse_session_id_from_response(session_header->second);

    // Get content type
    std::string content_type = "application/json";
    auto ct_header = res->headers.find("Content-Type");
    if (ct_header != res->headers.end())
        content_type = ct_header->second;

    // Parse response
    auto rpc_response = parse_response(res->body, content_type);

    // Check for JSON-RPC error
    if (rpc_response.contains("error"))
    {
        auto error = rpc_response["error"];
        std::string message = error.value("message", "Unknown error");
        throw fastmcpp::TransportError("JSON-RPC error: " + message);
    }

    // Extract result from JSON-RPC envelope
    if (rpc_response.contains("result"))
        return rpc_response["result"];

    // If no result or error, return empty object
    return fastmcpp::Json::object();
}

} // namespace fastmcpp::client
