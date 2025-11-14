# fastmcpp

High‑performance C++ implementation of the Model Context Protocol (MCP), with support for tools, resources, prompts, and multiple transport layers (STDIO, HTTP/SSE, WebSocket).

fastmcpp is a C++ port of the Python [fastmcp](https://github.com/jlowin/fastmcp) library, providing native performance for MCP servers and clients with a small, focused dependency set.

**Status:** Beta – core MCP features track the Python `fastmcp` reference, but the C++ test suite is intentionally much smaller than the Python one.

**Current version:** 2.13.0. Python `fastmcp` remains the canonical source of truth for behavior and API; this C++ port is expected to follow it.

## Features

- Core MCP protocol implementation (JSON‑RPC).
- Multiple transports: STDIO, HTTP (SSE), WebSocket.
- Tool management and invocation.
- Resources and prompts support.
- JSON Schema validation.
- Middleware for request/response processing.
- Integration with MCP‑compatible CLI tools.
- Cross‑platform: Windows, Linux, macOS.

## Requirements

- C++17 or later compiler.
- CMake 3.20 or higher.
- `nlohmann/json` (fetched automatically).

Optional:

- libcurl (for HTTP POST streaming).
- cpp‑httplib (HTTP server, fetched automatically).
- easywsclient (WebSocket client, fetched automatically).

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
  -DFASTMCPP_ENABLE_STREAMING_TESTS=ON \
  -DFASTMCPP_ENABLE_WS_STREAMING_TESTS=ON
```

Key options:

| Option                           | Default | Description                                      |
|----------------------------------|---------|--------------------------------------------------|
| `CMAKE_BUILD_TYPE`              | Debug   | Build configuration (Debug/Release/RelWithDebInfo) |
| `FASTMCPP_ENABLE_POST_STREAMING` | OFF     | Enable HTTP POST streaming (requires libcurl)   |
| `FASTMCPP_FETCH_CURL`           | OFF     | Fetch and build curl if not found               |
| `FASTMCPP_ENABLE_STREAMING_TESTS` | OFF   | Enable SSE streaming tests                      |
| `FASTMCPP_ENABLE_WS_STREAMING_TESTS` | OFF | Enable WebSocket streaming tests                |

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

Current status (CI / WSL configuration):

- 24/24 tests passing (100% success rate).
- 3 streaming tests disabled due to infrastructure dependencies.
- C++ test line count is much smaller than the Python `fastmcp` suite (see CCSDK parity docs).

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
    auto transport = std::make_shared<fastmcpp::client::HttpTransport>(
        "http://localhost:8080"
    );

    fastmcpp::client::Client client(transport);
    auto response = client.call("tool/invoke", {
        {"name", "calculator"},
        {"input", {{"operation", "add"}, {"a", 5}, {"b", 3}}}
    });

    std::cout << response.dump() << std::endl;
    return 0;
}
```

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
  tests/              # Test suite (GoogleTest)
  examples/           # Example programs
  CMakeLists.txt      # Build configuration
  LICENSE             # Apache 2.0 license
  NOTICE              # Attribution notices
  README.md           # This file
```

## Contributing

Contributions are welcome. Please:

1. Ensure all tests pass.
2. Follow the existing code style.
3. Add tests for new features.
4. Update documentation as needed.

## License

Copyright 2025 Elias Bachaalany

Licensed under the Apache License 2.0. See `LICENSE` and `NOTICE` for details.

This is a C++ port of [fastmcp](https://github.com/jlowin/fastmcp) by Jeremiah Lowin. The Python library is the canonical implementation; fastmcpp aims to match its behavior for core features.

## Support

For issues and questions, use the GitHub issue tracker: <https://github.com/0xeb/fastmcpp/issues>.

