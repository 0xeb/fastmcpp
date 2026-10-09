<div align="center">

<img src="logo.jpg" width="550" alt="fastmcpp logo">

**High-performance C++ implementation of the Model Context Protocol (MCP)**

[![CI](https://github.com/0xeb/fastmcpp/actions/workflows/ci.yml/badge.svg)](https://github.com/0xeb/fastmcpp/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/0xeb/fastmcpp.svg)](https://github.com/0xeb/fastmcpp/blob/main/LICENSE)

</div>

---

fastmcpp is a C++ port of the Python [fastmcp](https://github.com/jlowin/fastmcp) library, providing native performance for MCP servers and clients with support for tools, resources, prompts, and MCP-standard transport layers (STDIO, HTTP/SSE, Streamable HTTP).

**Status:** Beta – core MCP features track the Python `fastmcp` reference.

**Current version:** 3.4.7.1

## Features

- Core MCP protocol implementation (JSON‑RPC).
- Multiple transports: STDIO, HTTP (SSE), Streamable HTTP.
- Streamable HTTP transport (MCP spec 2025-03-26) with session management.
- WinHTTP client transport on Windows: HTTPS through Schannel, with the system
  certificate store and enterprise proxy configuration, and no OpenSSL.
- libcurl client transport on every platform, binding to whichever TLS backend
  the platform provides (`FASTMCPP_ENABLE_CURL_TRANSPORT=ON`).
- Per-request header provider for rotating credentials such as OAuth tokens.
- HTTP failures reported as `TransportHttpError`, retaining the status code,
  response headers (including `WWW-Authenticate`) and body.
- Tool management and invocation.
- Resources and prompts support.
- Resource templates with URI pattern matching.
- Resource annotations (MCP 2025-11-25): audience targeting, priority hints, icons.
- MCP protocol version `2025-11-25` by default, with negotiation: a server echoes
  back the revision a client asked for when it recognises it, so an older client
  keeps working. Override per client with `Client::set_protocol_version()`.
- JSON Schema validation.
- FastMCP high-level application class.
- ProxyApp for backend server proxying.
- Providers system for modular tool/resource composition.
- Provider transforms: namespace prefixing, visibility filtering, tool wrapping.
- ServerSession for bidirectional communication, sampling, and server-initiated notifications.
- Built-in middleware: Logging, Timing, Caching, RateLimiting, ErrorHandling.
- Tool transforms for input/output processing.
- Integration with MCP‑compatible CLI tools.
- Cross‑platform: Windows, Linux, macOS.

## Requirements

- C++17 or later compiler.
- CMake 3.16 or higher.
- `nlohmann/json` (fetched automatically).

Optional:

- libcurl (for HTTP POST streaming and the libcurl client transport; fetched
  when `FASTMCPP_FETCH_CURL=ON`, which is the default).
- cpp‑httplib (HTTP server, fetched automatically).
- OpenSSL, only if you want HTTPS through the cpp‑httplib transports
  (`FASTMCPP_ENABLE_OPENSSL=ON`). See "Reaching `https://` servers" below for
  the alternatives that need no extra dependency.

### Reaching `https://` servers

The cpp‑httplib transports negotiate TLS only when cpp‑httplib itself is
compiled with `CPPHTTPLIB_OPENSSL_SUPPORT`, so with the default
`FASTMCPP_ENABLE_OPENSSL=OFF` an `https://` URL fails at runtime with
`'https' scheme is not supported`. The URL parser accepts the scheme and
defaults to port 443; only the TLS handshake is missing.

Three ways to fix that:

- **Windows** — use `fastmcpp::client::WinHttpTransport`, built by default
  (`FASTMCPP_ENABLE_WINHTTP=ON`). TLS comes from Schannel, and the system
  certificate store, enterprise proxy settings and CA policy come with it. No
  extra dependency.
- **Any platform** — use `fastmcpp::client::CurlTransport`, built with
  `FASTMCPP_ENABLE_CURL_TRANSPORT=ON`. libcurl binds to whichever TLS backend
  the platform provides, so this reaches `https://` on Linux and macOS as well
  as Windows.
- **OpenSSL** — set `FASTMCPP_ENABLE_OPENSSL=ON`, with one caveat worth knowing:
  it is necessary but not sufficient when your project supplies its own
  `httplib::httplib` target. fastmcpp then leaves `CPPHTTPLIB_OPENSSL_SUPPORT`
  to that target to avoid an ODR mismatch, so cpp‑httplib must itself be built
  with its `openssl` feature or the build will succeed and then fail at runtime.

All three implement `ITransport`, so they drop into `Client` in place of
`StreamableHttpTransport`. The two above share `HttpRpcTransport`, which owns
the JSON-RPC framing, the redirect policy and session handling, so they also
support the per-request header provider and report failures as
`TransportHttpError`.

## Building

### Basic build

```bash
git clone https://github.com/0xeb/fastmcpp.git
cd fastmcpp

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

### Recommended configuration options

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DFASTMCPP_ENABLE_POST_STREAMING=ON \
  -DFASTMCPP_FETCH_CURL=ON \
  -DFASTMCPP_ENABLE_STREAMING_TESTS=ON
```

Key options:

| Option                           | Default | Description                                      |
|----------------------------------|---------|--------------------------------------------------|
| `CMAKE_BUILD_TYPE`              | Debug   | Build configuration (Debug/Release/RelWithDebInfo) |
| `FASTMCPP_BUILD_TESTS`          | ON      | Build the test suite                             |
| `FASTMCPP_BUILD_EXAMPLES`       | ON      | Build the example programs                       |
| `FASTMCPP_BUILD_CLI`            | ON      | Build the `fastmcpp` command-line executable     |
| `FASTMCPP_ENABLE_POST_STREAMING` | OFF     | Enable HTTP POST streaming (requires libcurl)   |
| `FASTMCPP_FETCH_CURL`           | ON      | Fetch and build curl (via FetchContent) if not found |
| `FASTMCPP_ENABLE_OPENSSL`       | OFF     | HTTPS for the cpp-httplib transports, via OpenSSL |
| `FASTMCPP_ENABLE_WINHTTP`       | ON (Windows) | Build the WinHTTP client transport          |
| `FASTMCPP_ENABLE_CURL_TRANSPORT` | OFF    | Build the libcurl client transport (all platforms) |
| `FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS` | OFF | Built-in OpenAI/Anthropic sampling handlers |
| `FASTMCPP_ENABLE_STREAMING_TESTS` | OFF   | Enable SSE streaming tests                      |

### Platform notes

**Windows (Visual Studio):**

```bash
cmake -B build -S . -G "Visual Studio 17 2022"
cmake --build build --config Release
```

**Linux/macOS:**

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## Testing

```bash
# Run all tests
ctest --test-dir build -C Release --output-on-failure

# Parallel
ctest --test-dir build -C Release -j4 --output-on-failure

# Run a specific test
ctest --test-dir build -C Release -R fastmcp_smoke --output-on-failure

# List tests
ctest --test-dir build -C Release -N
```

## Basic Usage

### STDIO MCP server

```cpp
#include <fastmcpp/tools/manager.hpp>
#include <fastmcpp/mcp/handler.hpp>
#include <fastmcpp/server/stdio_server.hpp>

int main() {
    fastmcpp::tools::ToolManager tm;
    // register tools on tm...

    auto handler = fastmcpp::mcp::make_mcp_handler(
        "myserver", "1.0.0", tm
    );

    fastmcpp::server::StdioServerWrapper server(handler);
    server.run();  // blocking
    return 0;
}
```

### HTTP server

```cpp
#include <fastmcpp/server/server.hpp>
#include <fastmcpp/server/http_server.hpp>

int main() {
    auto srv = std::make_shared<fastmcpp::server::Server>();
    srv->register_get("/health", [](const nlohmann::json&) {
        return nlohmann::json{{"status", "ok"}};
    });

    fastmcpp::server::HttpServerWrapper http(srv, "127.0.0.1", 8080);
    http.start();  // non‑blocking

    std::this_thread::sleep_for(std::chrono::hours(1));
    http.stop();
    return 0;
}
```

### HTTP client

```cpp
#include <fastmcpp/client/client.hpp>
#include <fastmcpp/client/transports.hpp>

int main() {
    // Create client with HTTP transport
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::HttpTransport>("http://localhost:8080")
    );

    // Initialize MCP session
    auto init = client.initialize();
    std::cout << "Connected to: " << init.serverInfo.name << std::endl;

    // List available tools
    auto tools = client.list_tools();
    for (const auto& tool : tools) {
        std::cout << "Tool: " << tool.name << std::endl;
    }

    // Call a tool
    auto result = client.call_tool("calculator", {{"a", 5}, {"b", 3}});
    std::cout << "Result: " << result.text() << std::endl;

    return 0;
}
```

### Keyless web search and fetch with Parallel

The [Parallel Search MCP](https://docs.parallel.ai/integrations/mcp/search-mcp)
example connects to `https://search.parallel.ai/mcp` using `CurlTransport` and
Streamable HTTP. It initializes the client, discovers the available tools, then
calls `web_search` or `web_fetch` and prints the returned URLs and excerpts.
No API key or model is needed. Anonymous access is free for light use and subject
to rate limits.

```bash
cmake -B build-parallel -S . -DCMAKE_BUILD_TYPE=Release \
  -DFASTMCPP_BUILD_TESTS=OFF -DFASTMCPP_BUILD_EXAMPLES=ON \
  -DFASTMCPP_ENABLE_CURL_TRANSPORT=ON
cmake --build build-parallel --config Release --target fastmcpp_example_parallel_search -j2

./build-parallel/fastmcpp_example_parallel_search search "C++ MCP client examples"
./build-parallel/fastmcpp_example_parallel_search fetch https://modelcontextprotocol.io/docs/getting-started/intro
```

On Windows, the executable is under `build-parallel/Release/` for multi-config
generators. The build needs libcurl with a TLS backend and a trusted CA store;
the existing `FASTMCPP_FETCH_CURL` option fetches libcurl by default. If your
toolchain supplies libcurl, add `-DFASTMCPP_FETCH_CURL=OFF` when configuring.

Each invocation makes one tool call with certificate verification enabled and
a 120-second transport timeout. Errors go to stderr with a nonzero exit status.
For a multi-call application, reuse the returned `session_id` in later search
and fetch arguments. This example executes tools directly; it does not run an
LLM agent loop. Existing client and build defaults are unchanged.

### Streamable HTTP server (MCP spec 2025-03-26)

```cpp
#include <fastmcpp/tools/manager.hpp>
#include <fastmcpp/mcp/handler.hpp>
#include <fastmcpp/server/streamable_http_server.hpp>

int main() {
    fastmcpp::tools::ToolManager tm;
    // register tools on tm...

    auto handler = fastmcpp::mcp::make_mcp_handler(
        "myserver", "1.0.0", tm
    );

    // Streamable HTTP server on /mcp endpoint
    fastmcpp::server::StreamableHttpServerWrapper server(
        handler, "127.0.0.1", 8080, "/mcp"
    );
    server.start();  // non-blocking

    std::this_thread::sleep_for(std::chrono::hours(1));
    server.stop();
    return 0;
}
```

### Streamable HTTP client

```cpp
#include <fastmcpp/client/client.hpp>
#include <fastmcpp/client/transports.hpp>

int main() {
    // Create client with Streamable HTTP transport (MCP spec 2025-03-26)
    fastmcpp::client::Client client(
        std::make_unique<fastmcpp::client::StreamableHttpTransport>(
            "http://localhost:8080", "/mcp"
        )
    );

    // Initialize MCP session (session ID managed automatically)
    auto init = client.initialize();
    std::cout << "Server: " << init.serverInfo.name << std::endl;

    // Use the same clean API as other transports
    auto tools = client.list_tools();
    auto result = client.call_tool("echo", {{"message", "Hello!"}});

    return 0;
}
```

### Proxy server

Create a proxy that forwards requests to a backend MCP server while allowing local overrides:

```cpp
#include <fastmcpp/proxy.hpp>
#include <fastmcpp/server/sse_server.hpp>
#include <fastmcpp/util/schema_build.hpp>

int main() {
    using fastmcpp::util::schema_build::to_object_schema_from_simple;

    // Create a proxy to a remote backend
    auto proxy = fastmcpp::create_proxy("http://backend:8080/mcp");

    // Add local tools that extend or override remote capabilities
    proxy.local_tools().register_tool({
        "double",
        to_object_schema_from_simple({{"n", "number"}}),  // input: {n: number}
        {{"type", "number"}},                             // output schema
        [](const fastmcpp::Json& args) { return args["n"].get<int>() * 2; }
    });

    // Create MCP handler and serve via SSE
    auto handler = fastmcpp::mcp::make_mcp_handler(proxy);
    fastmcpp::server::SseServerWrapper server(handler, "127.0.0.1", 8080);
    server.start();

    // Server runs until stopped...
    return 0;
}
```

The `create_proxy()` factory function automatically detects the transport type from the URL:
- `http://` or `https://` URLs use HTTP transport

Local tools, resources, and prompts take precedence over remote ones with the same name.

### Providers

Providers enable modular composition of tools and resources with automatic transforms:

```cpp
#include <fastmcpp/providers/filesystem_provider.hpp>
#include <fastmcpp/providers/local_provider.hpp>

int main() {
    using namespace fastmcpp::providers;

    // Create a filesystem provider that exposes directory contents
    FilesystemProvider fs_provider("./data", {
        .allowed_extensions = {".txt", ".json", ".md"},
        .max_file_size = 1024 * 1024  // 1MB
    });

    // Create a local provider with custom tools
    LocalProvider local;
    local.add_tool("greet", /*...*/);

    // Apply namespace transform to prefix all tool names
    auto namespaced = apply_transform<NamespaceTransform>(local, "myapp");
    // Tools now: myapp_greet, etc.

    auto handler = fastmcpp::mcp::make_mcp_handler(namespaced);
    // ...
}
```

**Provider Transforms:**

| Transform | Description |
|-----------|-------------|
| `NamespaceTransform` | Prefix tool/resource names (e.g., `math_add`) |
| `VisibilityTransform` | Filter which tools/resources are exposed |
| `ToolTransform` | Wrap tool inputs/outputs for preprocessing |

### Resource Annotations (MCP 2025-11-25)

Resources can include metadata hints for clients:

```cpp
fastmcpp::resources::ResourceDefinition res;
res.uri = "config://settings";
res.name = "Application Settings";
res.mime_type = "application/json";
res.annotations = Json{
    {"audience", Json::array({"user", "assistant"})},
    {"priority", 0.8}
};
res.icons = {{"icon.png", "image/png"}};
res.provider = [](const Json&) {
    return ResourceContent{"config://settings", "application/json",
                           R"({"theme": "dark"})"};
};
```

Annotations help clients:
- **audience**: Who should see this resource (`user`, `assistant`, or both)
- **priority**: Relative importance (0.0–1.0) for display ordering
- **icons**: Visual indicators for UI rendering

## Examples

See the `examples/` directory for complete programs, including:

- `stdio_server.cpp` – STDIO MCP server.
- `server_quickstart.cpp` – HTTP server with routes.
- `client_quickstart.cpp` – HTTP client usage.
- `tool_example.cpp` – tool registration and invocation.
- `middleware_example.cpp` – request/response middleware.

## Project Structure

```text
fastmcpp/
  include/fastmcpp/   # Public headers (client, server, tools, etc.)
  src/                # Implementation
  tests/              # Test suite (standalone executables, no framework)
  examples/           # Example programs
  CMakeLists.txt      # Build configuration
  LICENSE             # Apache 2.0 license
  NOTICE              # Attribution notices
  README.md           # This file
```

## Projects Using This Library

| Project | Description |
|---------|-------------|
| [copilot-sdk-cpp](https://github.com/0xeb/copilot-sdk-cpp) | C++ SDK for GitHub Copilot CLI |
| [claude-agent-sdk-cpp](https://github.com/0xeb/claude-agent-sdk-cpp) | C++ SDK for Claude Code CLI with MCP support |

Want to add your project? Open a PR!

## Contributing

Contributions are welcome. Please:

1. Ensure all tests pass.
2. Follow the existing code style.
3. Add tests for new features.
4. Update documentation as needed.

## Author

Elias Bachaalany ([@0xeb](https://github.com/0xeb))

Pair-programmed with Claude Code and Codex.

## License

Copyright 2025 Elias Bachaalany

Licensed under the Apache License 2.0. See `LICENSE` and `NOTICE` for details.

This is a C++ port of [fastmcp](https://github.com/jlowin/fastmcp) by Jeremiah Lowin. The Python library is the canonical implementation; fastmcpp aims to match its behavior for core features.

## Support

For issues and questions, use the GitHub issue tracker: <https://github.com/0xeb/fastmcpp/issues>.

