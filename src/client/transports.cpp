#include "fastmcpp/client/transports.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/util/json.hpp"
#include <httplib.h>
#include <easywsclient.hpp>
#include <sstream>
#include <fstream>
#include <thread>
#include <chrono>
#ifdef FASTMCPP_POST_STREAMING
#include <curl/curl.h>
#endif
#ifdef TINY_PROCESS_LIB_AVAILABLE
#include <process.hpp>
#endif

namespace fastmcpp::client {

namespace {
std::pair<std::string,int> parse_host_port(const std::string& base) {
  std::string host = base;
  int port = 80;
  // Strip scheme if present
  auto scheme_pos = host.find("://");
  if (scheme_pos != std::string::npos) {
    host = host.substr(scheme_pos + 3);
  }
  // If path segments exist, strip them
  auto slash_pos = host.find('/');
  if (slash_pos != std::string::npos) {
    host = host.substr(0, slash_pos);
  }
  // Extract port if provided
  auto colon_pos = host.rfind(':');
  if (colon_pos != std::string::npos) {
    std::string port_str = host.substr(colon_pos + 1);
    host = host.substr(0, colon_pos);
    try {
      port = std::stoi(port_str);
    } catch (...) {
      port = 80;
    }
  }
  return {host, port};
}
} // namespace

fastmcpp::Json HttpTransport::request(const std::string& route, const fastmcpp::Json& payload) {
  auto [host, port] = parse_host_port(base_url_);
  httplib::Client cli(host.c_str(), port);
  cli.set_connection_timeout(5, 0);
  cli.set_keep_alive(true);
  cli.set_read_timeout(10, 0);
  cli.set_follow_location(true);
  cli.set_default_headers({{"Accept", "text/event-stream, application/json"}});
  auto res = cli.Post(("/" + route).c_str(), payload.dump(), "application/json");
  if (!res) throw fastmcpp::TransportError("HTTP request failed: no response");
  if (res->status < 200 || res->status >= 300) throw fastmcpp::TransportError("HTTP error: " + std::to_string(res->status));
  return fastmcpp::util::json::parse(res->body);
}

void HttpTransport::request_stream(const std::string& route,
                                   const fastmcpp::Json& /*payload*/,
                                   const std::function<void(const fastmcpp::Json&)>& on_event) {
  auto [host, port] = parse_host_port(base_url_);
  httplib::Client cli(host.c_str(), port);
  cli.set_connection_timeout(5, 0);
  cli.set_keep_alive(true);
  cli.set_read_timeout(10, 0);

  std::string path = "/" + route;
  httplib::Headers headers = {
      {"Accept", "text/event-stream, application/json"}
  };

  std::string buffer;
  std::string last_emitted;
  std::atomic<bool> any_data{false};
  auto content_receiver = [&](const char* data, size_t len) {
    any_data.store(true, std::memory_order_relaxed);
    buffer.append(data, len);
    // Try to parse SSE-style events separated by double newlines
    size_t pos = 0;
    while (true) {
      size_t sep = buffer.find("\n\n", pos);
      if (sep == std::string::npos) break;
      std::string chunk = buffer.substr(pos, sep - pos);
      pos = sep + 2;

      // Extract data: lines
      std::string aggregated;
      size_t line_start = 0;
      while (line_start < chunk.size()) {
        size_t line_end = chunk.find('\n', line_start);
        std::string line = chunk.substr(line_start, line_end == std::string::npos ? std::string::npos : (line_end - line_start));
        if (line.rfind("data:", 0) == 0) {
          std::string data_part = line.substr(5);
          if (!data_part.empty() && data_part[0] == ' ') data_part.erase(0, 1);
          aggregated += data_part;
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
      }

      if (!aggregated.empty()) {
        // De-duplicate identical consecutive chunks to avoid repeated delivery
        if (aggregated == last_emitted) {
          // skip duplicate
        } else {
          last_emitted = aggregated;
          try {
            auto evt = fastmcpp::util::json::parse(aggregated);
            if (on_event) on_event(evt);
          } catch (...) {
            // Fallback: deliver raw chunk as text if not JSON
            fastmcpp::Json item = fastmcpp::Json{{"type","text"},{"text", aggregated}};
            fastmcpp::Json evt = fastmcpp::Json{{"content", fastmcpp::Json::array({item})}};
            if (on_event) on_event(evt);
          }
        }
      }
    }
    // Erase processed portion
    if (pos > 0) buffer.erase(0, pos);
    return true; // continue
  };

  auto response_handler = [&](const httplib::Response& r) {
    // Accept only 200 and event-stream or json
    return r.status >= 200 && r.status < 300;
  };
  // Retry for a short window in case the server isn't immediately ready
  httplib::Result res;
  for (int attempt = 0; attempt < 50; ++attempt) {
    // First, try full handler variant
    res = cli.Get(path.c_str(), headers, response_handler, content_receiver);
    if (res) break;
    // Fallback: some environments behave better with the simpler overload
    res = cli.Get(path.c_str(), content_receiver);
    if (res) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!res) {
    // Some environments may close the connection without a formal response
    // even though chunks were delivered. If we received any data, treat as ok.
    if (any_data.load(std::memory_order_relaxed)) return;
    throw fastmcpp::TransportError("HTTP stream request failed: no response");
  }
  if (res->status < 200 || res->status >= 300) throw fastmcpp::TransportError("HTTP stream error: " + std::to_string(res->status));
}

void HttpTransport::request_stream_post(const std::string& route,
                                        const fastmcpp::Json& payload,
                                        const std::function<void(const fastmcpp::Json&)>& on_event) {
#ifdef FASTMCPP_POST_STREAMING
  CURL* curl = curl_easy_init();
  if (!curl) throw fastmcpp::TransportError("libcurl init failed");

  std::string url = base_url_;
  if (!url.empty() && url.back() != '/') url.push_back('/');
  url += route;

  std::string body = payload.dump();
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: text/event-stream, application/json");

  std::string buffer;
  auto parse_and_emit = [&](bool flush_all=false){
    size_t pos = 0;
    while (true) {
      size_t sep = buffer.find("\n\n", pos);
      if (sep == std::string::npos) break;
      std::string chunk = buffer.substr(pos, sep - pos);
      pos = sep + 2;

      std::string aggregated;
      size_t line_start = 0;
      while (line_start < chunk.size()) {
        size_t line_end = chunk.find('\n', line_start);
        std::string line = chunk.substr(line_start, line_end == std::string::npos ? std::string::npos : (line_end - line_start));
        if (line.rfind("data:", 0) == 0) {
          std::string data_part = line.substr(5);
          if (!data_part.empty() && data_part[0] == ' ') data_part.erase(0, 1);
          aggregated += data_part;
        }
        if (line_end == std::string::npos) break;
        line_start = line_end + 1;
      }
      if (!aggregated.empty()) {
        try {
          auto evt = fastmcpp::util::json::parse(aggregated);
          if (on_event) on_event(evt);
        } catch (...) {
          fastmcpp::Json item = fastmcpp::Json{{"type","text"},{"text", aggregated}};
          fastmcpp::Json evt = fastmcpp::Json{{"content", fastmcpp::Json::array({item})}};
          if (on_event) on_event(evt);
        }
      }
    }
    if (flush_all && pos < buffer.size()) {
      std::string rest = buffer.substr(pos);
      if (!rest.empty()) {
        try {
          auto evt = fastmcpp::util::json::parse(rest);
          if (on_event) on_event(evt);
        } catch (...) {
          fastmcpp::Json item = fastmcpp::Json{{"type","text"},{"text", rest}};
          fastmcpp::Json evt = fastmcpp::Json{{"content", fastmcpp::Json::array({item})}};
          if (on_event) on_event(evt);
        }
      }
      buffer.clear();
    }
    if (pos > 0) buffer.erase(0, pos);
  };

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
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

  if (code != CURLE_OK) {
    throw fastmcpp::TransportError(std::string("HTTP stream POST failed: ") + curl_easy_strerror(code));
  }
  if (status < 200 || status >= 300) {
    throw fastmcpp::TransportError("HTTP stream POST error: " + std::to_string(status));
  }
#else
  (void)route; (void)payload; (void)on_event;
  throw fastmcpp::TransportError("libcurl not available; POST streaming unsupported in this build");
#endif
}

fastmcpp::Json WebSocketTransport::request(const std::string& route, const fastmcpp::Json& payload) {
  using easywsclient::WebSocket;
  std::string full = url_;
  if (!full.empty() && full.back() != '/') full.push_back('/');
  full += route;
  std::unique_ptr<WebSocket> ws(WebSocket::from_url(full));
  if (!ws) throw fastmcpp::TransportError("WS connect failed: " + full);
  ws->send(payload.dump());
  std::string resp;
  bool got = false;
  auto onmsg = [&](const std::string& msg){ resp = msg; got = true; };
  // Wait up to ~2s
  for (int i = 0; i < 40 && !got; ++i) {
    ws->poll(50);
    ws->dispatch(onmsg);
  }
  ws->close();
  if (!got) throw fastmcpp::TransportError("WS no response");
  return fastmcpp::util::json::parse(resp);
}

void WebSocketTransport::request_stream(const std::string& route,
                                        const fastmcpp::Json& payload,
                                        const std::function<void(const fastmcpp::Json&)>& on_event) {
  using easywsclient::WebSocket;
  std::string full = url_;
  if (!full.empty() && full.back() != '/') full.push_back('/');
  full += route;
  std::unique_ptr<WebSocket> ws(WebSocket::from_url(full));
  if (!ws) throw fastmcpp::TransportError("WS connect failed: " + full);

  // Send initial payload
  ws->send(payload.dump());

  // Pump loop: dispatch frames for a reasonable period or until closed
  // Stop after a short idle timeout window to avoid hanging indefinitely
  std::string frame;
  auto onmsg = [&](const std::string& msg){ frame = msg; };

  const int max_iters = 400; // ~20s total at 50ms per poll
  int idle_iters = 0;
  for (int i = 0; i < max_iters; ++i) {
    ws->poll(50);
    frame.clear();
    ws->dispatch(onmsg);
    if (!frame.empty()) {
      try {
        auto evt = fastmcpp::util::json::parse(frame);
        if (on_event) on_event(evt);
      } catch (...) {
        fastmcpp::Json item = fastmcpp::Json{{"type","text"},{"text", frame}};
        fastmcpp::Json evt = fastmcpp::Json{{"content", fastmcpp::Json::array({item})}};
        if (on_event) on_event(evt);
      }
      idle_iters = 0; // reset idle counter on data
    } else {
      // No message arrived in this poll slice
      if (++idle_iters > 60) {
        // ~3s idle without frames → assume stream done
        break;
      }
    }
  }
  ws->close();
}

fastmcpp::Json StdioTransport::request(const std::string& route, const fastmcpp::Json& payload) {
  // Use TinyProcessLibrary (fetched via CMake) for cross-platform subprocess handling
  // Build command line
  std::ostringstream cmd;
  cmd << command_;
  for (const auto& a : args_) {
    cmd << " " << a;
  }

#ifdef TINY_PROCESS_LIB_AVAILABLE
  using namespace TinyProcessLib;
  std::string stdout_data;
  std::string stderr_data;

  // Open log file if path was provided (RAII - closes automatically)
  std::ofstream log_file_stream;
  std::ostream* stderr_target = nullptr;

  if (log_file_.has_value()) {
    // Open file in append mode
    log_file_stream.open(log_file_.value(), std::ios::app);
    if (log_file_stream.is_open()) {
      stderr_target = &log_file_stream;
    }
  } else if (log_stream_ != nullptr) {
    stderr_target = log_stream_;
  }

  // Stderr callback: write to log file/stream if configured, otherwise capture
  auto stderr_callback = [&](const char *bytes, size_t n) {
    if (stderr_target != nullptr) {
      stderr_target->write(bytes, n);
      stderr_target->flush();
    }
    // Always capture for error messages (in case of process failure)
    stderr_data.append(bytes, n);
  };

  Process process(cmd.str(), "",
                  [&](const char *bytes, size_t n) { stdout_data.append(bytes, n); },
                  stderr_callback,
                  true);

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
  if (exit_code != 0) {
    throw fastmcpp::TransportError("StdioTransport process exit code: " + std::to_string(exit_code) + (stderr_data.empty() ? std::string("") : ("; stderr: ") + stderr_data));
  }
  // Read first line from stdout_data
  auto pos = stdout_data.find('\n');
  std::string first_line = pos == std::string::npos ? stdout_data : stdout_data.substr(0, pos);
  if (first_line.empty()) throw fastmcpp::TransportError("StdioTransport: no response");
  return fastmcpp::util::json::parse(first_line);
#else
  throw fastmcpp::TransportError("TinyProcessLib is not integrated; cannot run StdioTransport");
#endif
}

} // namespace fastmcpp::client
