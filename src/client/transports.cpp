#include "fastmcpp/client/transports.hpp"

#include "../internal/process.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"
#include "http_detail.hpp"
#include "sse_stream.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <httplib.h>
#include <mutex>
#include <sstream>
#include <thread>
#ifdef FASTMCPP_POST_STREAMING
#include <curl/curl.h>
#endif

namespace fastmcpp::client
{

// URL parsing, redirect resolution and SSE framing are shared with the
// WinHTTP and libcurl transports; see src/client/http_detail.hpp.
using namespace detail;

struct StdioTransport::State
{
    fastmcpp::process::Process process;
    std::mutex request_mutex;
    // Stderr background reader (keep-alive mode)
    std::thread stderr_thread;
    std::atomic<bool> stderr_running{false};
    std::mutex stderr_mutex;
    std::string stderr_data;
    // Logging
    std::ofstream log_file_stream;
    std::ostream* stderr_target{nullptr};
};

namespace
{

/// Copy httplib's response headers into a container the public exception type
/// can expose without leaking httplib into fastmcpp's headers.
std::multimap<std::string, std::string> to_multimap(const httplib::Headers& headers)
{
    std::multimap<std::string, std::string> out;
    for (const auto& [key, value] : headers)
        out.emplace(key, value);
    return out;
}
} // namespace

std::unordered_map<std::string, std::string>
HttpTransport::effective_headers(const HeaderRequest& req) const
{
    auto merged = headers_;
    if (header_provider_)
        for (auto& [key, value] : header_provider_(req))
            merged[key] = std::move(value);
    return merged;
}

fastmcpp::Json HttpTransport::request(const std::string& route, const fastmcpp::Json& payload)
{
    auto url = parse_url(base_url_);

    // Security: Create client with full scheme://host:port URL for proper TLS handling
    std::string full_url = url.scheme + "://" + url.host + ":" + std::to_string(url.port);
    httplib::Client cli(full_url.c_str());
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    cli.enable_server_certificate_verification(verify_ssl_);
#endif

    cli.set_connection_timeout(5, 0);
    cli.set_keep_alive(true);
    cli.set_read_timeout(static_cast<int>(timeout_.count()), 0);

    // Security: Disable redirects by default to prevent SSRF and TLS downgrade attacks
    cli.set_follow_location(false);

    const std::string request_path = "/" + route;
    const std::string request_body = payload.dump();

    httplib::Headers headers = {{"Accept", "text/event-stream, application/json"}};
    for (const auto& [key, value] :
         effective_headers({"POST", full_url + request_path, request_path, request_body}))
        headers.emplace(key, value);

    auto res = cli.Post(request_path.c_str(), headers, request_body, "application/json");
    if (!res)
        throw fastmcpp::TransportError("HTTP request failed: no response");
    if (res->status < 200 || res->status >= 300)
        throw fastmcpp::TransportHttpError("HTTP error: " + std::to_string(res->status),
                                           res->status, to_multimap(res->headers), res->body);
    return fastmcpp::util::json::parse(res->body);
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
    for (const auto& [key, value] : effective_headers({"POST", url, "/" + route, body}))
    {
        std::string header = key + ": " + value;
        headers = curl_slist_append(headers, header.c_str());
    }

    // Framing is shared with the SSE listener; only the JSON policy below is
    // specific to this path.
    detail::SseEventStream framer;
    auto emit = [&on_event](const detail::SseEvent& event)
    {
        if (!on_event)
            return;
        try
        {
            on_event(fastmcpp::util::json::parse(event.data));
        }
        catch (...)
        {
            // A non-JSON payload is surfaced as text content rather than dropped.
            fastmcpp::Json item = fastmcpp::Json{{"type", "text"}, {"text", event.data}};
            on_event(fastmcpp::Json{{"content", fastmcpp::Json::array({item})}});
        }
    };

    // Bundles what the write callback needs, so events are delivered as bytes
    // arrive instead of only once the transfer finishes.
    struct StreamSink
    {
        detail::SseEventStream* framer;
        const std::function<void(const detail::SseEvent&)>* emit;
    };
    std::function<void(const detail::SseEvent&)> emit_fn = emit;
    StreamSink sink{&framer, &emit_fn};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
    curl_easy_setopt(
        curl, CURLOPT_WRITEFUNCTION,
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t
        {
            auto* s = static_cast<StreamSink*>(userdata);
            s->framer->feed(ptr, size * nmemb, *s->emit);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeout_.count()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // no overall timeout for streaming

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    // A stream that ended without a trailing blank line still carried an event.
    framer.flush(emit_fn);

    if (code != CURLE_OK && code != CURLE_PARTIAL_FILE)
    {
        throw fastmcpp::TransportError(std::string("HTTP stream POST failed: ") +
                                       curl_easy_strerror(code));
    }
    if (status < 200 || status >= 300)
        throw fastmcpp::TransportHttpError("HTTP stream POST error: " + std::to_string(status),
                                           static_cast<int>(status));
#else
    (void)route;
    (void)payload;
    (void)on_event;
    throw fastmcpp::TransportError(
        "libcurl not available; POST streaming unsupported in this build");
#endif
}

StdioTransport::StdioTransport(std::string command, std::vector<std::string> args,
                               std::optional<std::filesystem::path> log_file, bool keep_alive)
    : command_(std::move(command)), args_(std::move(args)), log_file_(std::move(log_file)),
      keep_alive_(keep_alive)
{
}

StdioTransport::StdioTransport(std::string command, std::vector<std::string> args,
                               std::ostream* log_stream, bool keep_alive)
    : command_(std::move(command)), args_(std::move(args)), log_stream_(log_stream),
      keep_alive_(keep_alive)
{
}

fastmcpp::Json StdioTransport::request(const std::string& route, const fastmcpp::Json& payload)
{
    return send_message(route, payload, true);
}

void StdioTransport::notify(const std::string& route, const fastmcpp::Json& payload)
{
    // A notification carries no "id"; the server must not reply, so waiting for
    // a response here would block until the read timeout.
    (void)send_message(route, payload, false);
}

fastmcpp::Json StdioTransport::send_message(const std::string& route, const fastmcpp::Json& payload,
                                            bool expect_response)
{
    namespace proc = fastmcpp::process;

    if (keep_alive_)
    {
        // Python fastmcp commit f5804f47 (#3630): if the subprocess died between calls,
        // reset state and respawn on the next request rather than failing forever.
        if (state_)
        {
            auto exit_code = state_->process.try_wait();
            if (exit_code.has_value())
            {
                // Process already exited — tear down so we respawn cleanly below.
                state_->stderr_running.store(false, std::memory_order_release);
                if (state_->stderr_thread.joinable())
                    state_->stderr_thread.join();
                state_.reset();
            }
        }

        // --- Keep-alive mode: spawn once, reuse across calls ---
        if (!state_)
        {
            state_ = std::make_unique<State>();

            if (log_file_.has_value())
            {
                state_->log_file_stream.open(log_file_.value(), std::ios::app);
                if (state_->log_file_stream.is_open())
                    state_->stderr_target = &state_->log_file_stream;
            }
            else if (log_stream_ != nullptr)
            {
                state_->stderr_target = log_stream_;
            }

            try
            {
                state_->process.spawn(command_, args_,
                                      proc::ProcessOptions{/*working_directory=*/{},
                                                           /*environment=*/{},
                                                           /*inherit_environment=*/true,
                                                           /*redirect_stdin=*/true,
                                                           /*redirect_stdout=*/true,
                                                           /*redirect_stderr=*/true,
                                                           /*create_no_window=*/true});
            }
            catch (const proc::ProcessError& e)
            {
                state_.reset();
                throw fastmcpp::TransportError(std::string("StdioTransport: spawn failed: ") +
                                               e.what());
            }

            // Background stderr reader to prevent pipe buffer deadlock
            state_->stderr_running.store(true, std::memory_order_release);
            state_->stderr_thread = std::thread(
                [st = state_.get()]()
                {
                    char buf[1024];
                    while (st->stderr_running.load(std::memory_order_acquire))
                    {
                        try
                        {
                            if (!st->process.stderr_pipe().is_open())
                                break;
                            if (!st->process.stderr_pipe().has_data(50))
                                continue;
                            size_t n = st->process.stderr_pipe().read(buf, sizeof(buf));
                            if (n == 0)
                                break;
                            std::lock_guard<std::mutex> lock(st->stderr_mutex);
                            if (st->stderr_target)
                            {
                                st->stderr_target->write(buf, static_cast<std::streamsize>(n));
                                st->stderr_target->flush();
                            }
                            st->stderr_data.append(buf, n);
                        }
                        catch (...)
                        {
                            break;
                        }
                    }
                });
        }

        auto* st = state_.get();
        std::lock_guard<std::mutex> request_lock(st->request_mutex);

        const int64_t id = next_id_++;
        fastmcpp::Json rpc_request = {
            {"jsonrpc", "2.0"},
            {"method", route},
            {"params", payload},
        };
        if (expect_response)
            rpc_request["id"] = id;

        {
            std::lock_guard<std::mutex> lock(st->stderr_mutex);
            st->stderr_data.clear();
        }

        try
        {
            st->process.stdin_pipe().write(rpc_request.dump() + "\n");
        }
        catch (const proc::ProcessError& e)
        {
            throw fastmcpp::TransportError(std::string("StdioTransport: failed to write: ") +
                                           e.what());
        }

        if (!expect_response)
            return fastmcpp::Json::object();

        // Read lines from stdout until we get a JSON response matching our ID
        for (;;)
        {
            // Check if process exited
            auto exit_code = st->process.try_wait();
            if (exit_code.has_value())
            {
                std::lock_guard<std::mutex> lock(st->stderr_mutex);
                throw fastmcpp::TransportError(
                    "StdioTransport process exited with code: " + std::to_string(*exit_code) +
                    (st->stderr_data.empty() ? std::string() : "; stderr: " + st->stderr_data));
            }

            // Wait for data with timeout, checking process liveness periodically
            bool have_data = false;
            constexpr int total_timeout_ms = 30000;
            constexpr int poll_ms = 200;
            for (int elapsed = 0; elapsed < total_timeout_ms; elapsed += poll_ms)
            {
                if (st->process.stdout_pipe().has_data(poll_ms))
                {
                    have_data = true;
                    break;
                }
                // Re-check process liveness during the wait
                auto code = st->process.try_wait();
                if (code.has_value())
                {
                    // Drain any remaining data from stdout before throwing
                    if (st->process.stdout_pipe().has_data(0))
                    {
                        have_data = true;
                        break;
                    }
                    std::lock_guard<std::mutex> lock(st->stderr_mutex);
                    throw fastmcpp::TransportError(
                        "StdioTransport process exited with code: " + std::to_string(*code) +
                        (st->stderr_data.empty() ? std::string() : "; stderr: " + st->stderr_data));
                }
            }
            if (!have_data)
                throw fastmcpp::TransportError("StdioTransport: timed out waiting for response");

            std::string line = st->process.stdout_pipe().read_line();
            // Strip trailing \r\n
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();

            if (line.empty())
                continue;

            try
            {
                auto parsed = fastmcpp::util::json::parse(line);
                if (parsed.contains("id") && parsed["id"].is_number_integer() &&
                    parsed["id"].get<int64_t>() == id)
                {
                    return parsed;
                }
            }
            catch (...)
            {
                // Ignore non-JSON stdout lines (e.g., server logs)
            }
        }
    }

    // --- One-shot mode: spawn per call ---
    proc::Process process;
    try
    {
        process.spawn(command_, args_,
                      proc::ProcessOptions{/*working_directory=*/{},
                                           /*environment=*/{},
                                           /*inherit_environment=*/true,
                                           /*redirect_stdin=*/true,
                                           /*redirect_stdout=*/true,
                                           /*redirect_stderr=*/true,
                                           /*create_no_window=*/true});
    }
    catch (const proc::ProcessError& e)
    {
        throw fastmcpp::TransportError(std::string("StdioTransport: spawn failed: ") + e.what());
    }

    // Write request then close stdin
    fastmcpp::Json rpc_request = {
        {"jsonrpc", "2.0"},
        {"method", route},
        {"params", payload},
    };
    if (expect_response)
        rpc_request["id"] = 1;
    process.stdin_pipe().write(rpc_request.dump() + "\n");
    process.stdin_pipe().close();

    if (!expect_response)
        return fastmcpp::Json::object();

    // Read all stdout synchronously
    std::string stdout_data;
    {
        char buf[4096];
        for (;;)
        {
            size_t n = process.stdout_pipe().read(buf, sizeof(buf));
            if (n == 0)
                break;
            stdout_data.append(buf, n);
        }
    }

    // Read all stderr synchronously
    std::string stderr_data;
    {
        char buf[4096];
        for (;;)
        {
            size_t n = process.stderr_pipe().read(buf, sizeof(buf));
            if (n == 0)
                break;
            stderr_data.append(buf, n);
        }
    }

    // Log stderr if configured
    std::ostream* stderr_target = nullptr;
    std::ofstream log_file_stream;
    if (log_file_.has_value())
    {
        log_file_stream.open(log_file_.value(), std::ios::app);
        if (log_file_stream.is_open())
            stderr_target = &log_file_stream;
    }
    else if (log_stream_ != nullptr)
    {
        stderr_target = log_stream_;
    }
    if (stderr_target && !stderr_data.empty())
    {
        stderr_target->write(stderr_data.data(), static_cast<std::streamsize>(stderr_data.size()));
        stderr_target->flush();
    }

    int exit_code = process.wait();
    if (exit_code != 0)
    {
        throw fastmcpp::TransportError(
            "StdioTransport process exit code: " + std::to_string(exit_code) +
            (stderr_data.empty() ? std::string() : "; stderr: " + stderr_data));
    }

    // Parse first JSON line from stdout
    auto pos = stdout_data.find('\n');
    std::string first_line = pos == std::string::npos ? stdout_data : stdout_data.substr(0, pos);
    // Strip trailing \r
    if (!first_line.empty() && first_line.back() == '\r')
        first_line.pop_back();
    if (first_line.empty())
        throw fastmcpp::TransportError("StdioTransport: no response");
    return fastmcpp::util::json::parse(first_line);
}

StdioTransport::StdioTransport(StdioTransport&&) noexcept = default;
StdioTransport& StdioTransport::operator=(StdioTransport&&) noexcept = default;

StdioTransport::~StdioTransport()
{
    if (state_)
    {
        // Stop stderr reader thread
        state_->stderr_running.store(false, std::memory_order_release);

        // Close stdin to signal the server to exit
        try
        {
            state_->process.stdin_pipe().close();
        }
        catch (...)
        {
        }

        // Poll for graceful exit
        for (int i = 0; i < 10; i++)
        {
            auto code = state_->process.try_wait();
            if (code.has_value())
            {
                if (state_->stderr_thread.joinable())
                    state_->stderr_thread.join();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Force kill if still running
        try
        {
            state_->process.kill();
            state_->process.wait();
        }
        catch (...)
        {
        }

        if (state_->stderr_thread.joinable())
            state_->stderr_thread.join();
    }
}

// =============================================================================
// SseClientTransport implementation
// =============================================================================

SseClientTransport::SseClientTransport(std::string base_url, std::string sse_path,
                                       std::string messages_path, bool verify_ssl)
    : base_url_(std::move(base_url)), sse_path_(std::move(sse_path)),
      messages_path_(std::move(messages_path)), verify_ssl_(verify_ssl)
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
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            cli.enable_server_certificate_verification(verify_ssl_);
#endif
            cli.set_connection_timeout(10, 0);
            cli.set_read_timeout(300, 0); // Long timeout for SSE stream (5 minutes)
            cli.set_keep_alive(true);

            detail::SseEventStream framer;
            auto content_receiver = [this, &framer](const char* data, size_t len)
            {
                if (!running_.load(std::memory_order_acquire))
                    return false;

                framer.feed(data, len,
                            [this](const detail::SseEvent& event)
                            {
                                // The endpoint event is not JSON: its data is the
                                // path every subsequent request and notification
                                // gets POSTed to, so losing it breaks the whole
                                // transport rather than just this message.
                                if (event.event == "endpoint")
                                {
                                    std::lock_guard<std::mutex> lock(endpoint_mutex_);
                                    endpoint_path_ = event.data;

                                    // Parse session_id from "...?session_id=<id>"
                                    session_id_.clear();
                                    auto pos = endpoint_path_.find("session_id=");
                                    if (pos != std::string::npos)
                                    {
                                        pos += std::string("session_id=").size();
                                        auto end = endpoint_path_.find_first_of("&#", pos);
                                        session_id_ = endpoint_path_.substr(
                                            pos, end == std::string::npos ? std::string::npos
                                                                          : (end - pos));
                                    }
                                    return;
                                }

                                try
                                {
                                    process_sse_event(fastmcpp::util::json::parse(event.data));
                                }
                                catch (...)
                                {
                                    // A non-JSON event on this stream is not fatal.
                                }
                            });

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
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        cli.enable_server_certificate_verification(verify_ssl_);
#endif
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

void SseClientTransport::notify(const std::string& route, const fastmcpp::Json& payload)
{
    if (!is_connected())
        throw fastmcpp::TransportError("SSE client not connected");

    // A notification carries no "id" and gets no reply on the event stream, so
    // it is posted without registering a pending request to wait on.
    fastmcpp::Json rpc_notification = {{"jsonrpc", "2.0"}, {"method", route}, {"params", payload}};

    auto url = parse_url(base_url_);
    httplib::Client cli(url.host.c_str(), url.port);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    cli.enable_server_certificate_verification(verify_ssl_);
#endif
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);

    std::string post_path;
    {
        std::lock_guard<std::mutex> lock(endpoint_mutex_);
        post_path = endpoint_path_.empty() ? messages_path_ : endpoint_path_;
    }

    auto res = cli.Post(post_path.c_str(), rpc_notification.dump(), "application/json");
    if (!res)
        throw fastmcpp::TransportError("Failed to send notification to " + messages_path_);
    if (res->status < 200 || res->status >= 300)
        throw fastmcpp::TransportHttpError("HTTP error: " + std::to_string(res->status),
                                           res->status, to_multimap(res->headers), res->body);
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
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    cli.enable_server_certificate_verification(verify_ssl_);
#endif
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
        throw fastmcpp::TransportHttpError("HTTP error: " + std::to_string(res->status),
                                           res->status, to_multimap(res->headers), res->body);
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
    std::unordered_map<std::string, std::string> headers, bool verify_ssl)
    : HttpRpcTransport(std::move(base_url), std::move(mcp_path), std::move(headers), verify_ssl)
{
}

StreamableHttpTransport::~StreamableHttpTransport() = default;

HttpResponse
StreamableHttpTransport::post(const std::string& url, const std::string& path,
                              const std::unordered_map<std::string, std::string>& headers,
                              const std::string& body)
{
    httplib::Client cli(url.c_str());
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    cli.enable_server_certificate_verification(verify_ssl());
#endif
    cli.set_connection_timeout(30, 0);
    cli.set_read_timeout(300, 0); // Align with MCP HTTP defaults (30s connect, 5min read)
    cli.set_keep_alive(true);
    // HttpRpcTransport runs the redirect loop for every transport alike, so that
    // a credential attached by a HeaderProvider is not replayed to another
    // origin. cpp-httplib must not follow a 3xx behind its back.
    cli.set_follow_location(false);

    httplib::Headers request_headers;
    for (const auto& [key, value] : headers)
        request_headers.emplace(key, value);

    auto res = cli.Post(path.c_str(), request_headers, body, "application/json");
    if (!res)
        throw fastmcpp::TransportError("StreamableHttp request failed: no response");

    return HttpResponse{res->status, to_multimap(res->headers), res->body};
}

} // namespace fastmcpp::client
