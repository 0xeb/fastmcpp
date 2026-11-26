<div align="center">

<img src="logo.jpg" width="550" alt="fastmcpp logo">

**High-performance C++ implementation of the Model Context Protocol (MCP)**

[![CI](https://github.com/0xeb/fastmcpp/actions/workflows/ci.yml/badge.svg)](https://github.com/0xeb/fastmcpp/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/0xeb/fastmcpp.svg)](https://github.com/0xeb/fastmcpp/blob/main/LICENSE)

</div>

---

fastmcpp is a C++ port of the Python [fastmcp](https://github.com/jlowin/fastmcp) library, providing native performance for MCP servers and clients with support for tools, resources, prompts, and MCP-standard transport layers (STDIO, HTTP/SSE, Streamable HTTP).

**Status:** Beta – core MCP features track the Python `fastmcp` reference.

**Current version:** 4.2.6

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
- Sessionless MCP `2026-07-28` alongside the handshake revisions: one server
  answers both, chosen per request on Streamable HTTP and per connection on
  STDIO. `Client::connect()` negotiates it and falls back to `initialize()`
  against older servers. See [Sessionless MCP (fastmcp 4)](#sessionless-mcp-fastmcp-4).
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
| `FASTMCPP_BUILD_EXAMPLES`       | ON      | Build the example programs                       |
| `FASTMCPP_BUILD_CLI`            | ON      | Build the `fastmcpp` command-line executable     |
| `FASTMCPP_ENABLE_POST_STREAMING` | OFF     | Enable HTTP POST streaming (requires libcurl)   |
| `FASTMCPP_FETCH_CURL`           | ON      | Fetch and build curl (via FetchContent) if not found |
| `FASTMCPP_ENABLE_OPENSSL`       | OFF     | HTTPS for the cpp-httplib transports, via OpenSSL |
| `FASTMCPP_ENABLE_WINHTTP`       | ON (Windows) | Build the WinHTTP client transport          |
| `FASTMCPP_ENABLE_CURL_TRANSPORT` | OFF    | Build the libcurl client transport (all platforms) |
| `FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS` | OFF | Built-in OpenAI/Anthropic sampling handlers |

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

#### Host/Origin protection (DNS rebinding)

The HTTP, SSE and streamable HTTP servers can validate the `Host` and `Origin` headers, as
fastmcp's `http_host_origin_protection` does. It is off by default. The servers take their
default from the environment, and `set_host_origin_guard(...)` overrides it:

| Variable | Values |
|---|---|
| `FASTMCPP_HTTP_HOST_ORIGIN_PROTECTION` | `off` (default), `auto`, `strict` (`true`/`1` = strict) |
| `FASTMCPP_HTTP_ALLOWED_HOSTS` | comma-separated extra trusted hosts |
| `FASTMCPP_HTTP_ALLOWED_ORIGINS` | comma-separated extra trusted origins |

Since 3.4.11.1 these variables take effect; earlier versions parsed them but ignored them.

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

### Authentication (bearer token, OAuth)

`HttpRpcTransport::set_auth()` attaches authentication to the HTTP client transports
(`StreamableHttpTransport`, `WinHttpTransport`, `CurlTransport`), as fastmcp's
`Client(auth=...)` does:

```cpp
#include <fastmcpp/client/auth.hpp>

auto transport = std::make_unique<client::StreamableHttpTransport>("https://example.com/mcp");
transport->set_auth(std::make_shared<client::auth::BearerAuth>("my-token"));
// or the full OAuth flow: discovery, registration, PKCE, refresh
transport->set_auth(std::make_shared<client::auth::OAuth>());
client::Client c(std::move(transport));
```

`OAuth` follows the MCP authorization flow (RFC 9728 / RFC 8414 discovery, dynamic client
registration or a pre-registered `client_id`, PKCE S256, RFC 8707 `resource`, refresh, scope
step-up). By default it opens the system browser and waits on a loopback callback; set
`OAuthOptions::redirect_handler` / `callback_handler` for headless use, and
`OAuthOptions::token_storage` (`FileStore`) to keep tokens across runs. `https://` endpoints
need a build with WinHTTP or libcurl.

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

    // Connect: tries the sessionless 2026-07-28 protocol, falls back to the
    // initialize handshake (with an Mcp-Session-Id) against older servers.
    auto init = client.connect();
    std::cout << "Server: " << init.serverInfo.name
              << " (" << init.protocolVersion << ")" << std::endl;

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

## Sessionless MCP (fastmcp 4)

Upstream fastmcp 4 adds the sessionless MCP `2026-07-28` protocol. Servers
serve it next to the handshake revisions without any code change; most
programs need nothing. What changes:

- **Two protocol eras from one server.** On Streamable HTTP a request whose
  `MCP-Protocol-Version` header is not a handshake revision is served
  sessionlessly: no `Mcp-Session-Id`, POST only, the protocol envelope in each
  request's `params._meta`, JSON-RPC errors mapped to HTTP 400/404. On STDIO
  the client's first request decides the connection's era. The SSE transport is
  handshake-only.
- **`Client::connect()`** (new) negotiates the era: it probes with
  `server/discover` and falls back to `initialize()` against an older server.
  `initialize()` itself is unchanged, so existing clients keep the handshake.
- **No server-initiated requests on the sessionless era.** `Context::elicit()`
  and `Context::sample()` throw on a sessionless request rather than wait for a
  reply that cannot arrive; they work as before on handshake connections. Ask
  for input by returning an `InputRequiredResult` instead; see below.
- **Component version in `_meta.fastmcp`.** List entries carry
  `_meta.fastmcp = {"tags": [...], "version": "..."}` as fastmcp emits it; the
  top-level `version` and top-level `fastmcp` keys are gone. The client reads
  both forms.
- **Resource not found is `-32602`** (was `-32002`), with the message
  `Resource not found: '<uri>'` and `data.uri`, on every handler.
- **`RpcError`.** The HTTP client transports raise `fastmcpp::RpcError` (a
  `TransportError` carrying `code()` and `data()`) for JSON-RPC errors.
- **fastmcp 4 servers list static resources with a trailing slash**
  (`file://readme.txt/`) and resolve only that form; read resources by the URI
  the server lists.

### Asking the client for input across rounds

On `2026-07-28` a tool, prompt or resource that needs input *returns* a
description of what it needs. The client fulfils it with its elicitation,
sampling or roots callback and calls again with the answers; the handler runs
again from the top, reads them, and either asks for more or finishes. Nothing
stays alive on the server between rounds.

```cpp
#include "fastmcpp/mcp/input_required.hpp"

app.tool("book_flight", schema, [](const Json& args) -> Json {
    auto answers = mcp::input_responses_of(args);   // nullopt on the first round
    if (!answers)
        return mcp::input_required(
            {{"dest", mcp::elicit_input("Where to?", dest_schema)}});
    if (answers->contains("dest"))
    {
        auto dest = (*answers)["dest"]["content"]["dest"].get<std::string>();
        return mcp::input_required({{"date", mcp::elicit_input("When?", date_schema)}},
                                   "dest=" + dest);   // carried to the next round
    }
    auto state = mcp::request_state_of(args);        // "dest=..."
    return "Booked " + state->substr(5) + " on " +
           (*answers)["date"]["content"]["date"].get<std::string>();
});
```

- Tools may return `mcp::input_required(...)` or throw
  `mcp::InputRequired(...)`; prompts and resources throw it. Tools can also
  read `Context::input_responses()` and `Context::request_state()`.
- The state is sealed on the wire with AES-256-GCM, bound to the method, the
  tool/prompt name or resource URI, the arguments and the server name, and
  expires after 10 minutes. The format is the MCP Python SDK's, so C++ and
  Python replicas that share a key accept each other's tokens. By default each
  process generates its own key; when several replicas serve one endpoint, give
  them a shared one with
  `StreamableHttpServerWrapper::set_request_state_security(mcp::RequestStateSecurity::from_keys({secret}))`
  (or the `DualEraHandler` constructor).
- On a handshake connection a handler that asks this way gets a `-32602` error
  naming the negotiated version; use `Context::elicit()` there.
- `Client::call_tool()`, `get_prompt()` and `read_resource()` drive the rounds
  automatically, up to `set_input_required_max_rounds()` (default 10). Without
  a matching callback the client refuses the request, as fastmcp does.

### Extensions

A `server::ServerExtension` contributes a capability advertised under
`capabilities.extensions[identifier]` on `2026-07-28`, new request methods
(`MethodBinding`, optionally limited to protocol versions), and a
`tools/call` interceptor. Register it with `FastMCP::add_extension()`. The
client opts extensions in per request with `Client::set_extension(id)`.

### Background tasks

`server::TasksExtension` (`io.modelcontextprotocol/tasks`) runs tools whose
`ToolOptions::task_support` is `Optional` or `Required` in the background when
the client opts in, and serves `tasks/get`, `tasks/update` and `tasks/cancel`.
A task tool may ask for input across legs as above.

```cpp
#include "fastmcpp/server/tasks_extension.hpp"

FastMCP::ToolOptions opts;
opts.task_support = TaskSupport::Optional;
app.tool("render", schema, render_fn, opts);
app.add_extension(std::make_shared<server::TasksExtension>());
```

The client declares the extension by default: `call_tool()` follows a task to
its result transparently, and `call_tool_task()` returns a handle
(`status()`, `wait()`, `result()`, `cancel()`). Tasks live in process memory;
there is no Redis/Docket backend.

### Skills

`skills::Skill` is an agent skill (a `SKILL.md` plus supporting files). Serve
skills with `skills::SkillCatalogProvider` or `skills::SkillsDirectoryProvider`;
adding either registers `skills::SkillsExtension`
(`io.modelcontextprotocol/skills`, `skills/list` and `skills/get`) and serves
every file as a `skill://` resource next to a `_manifest` listing.

```cpp
#include "fastmcpp/skills/provider.hpp"

app.add_provider(std::make_shared<skills::SkillCatalogProvider>(std::vector<skills::Skill>{
    skills::Skill("code-review", "Review a pull request.", "# Code review\n..."),
    skills::Skill::from_directory("./skills/deploy"),
}));
```

On the client, `client::skills::list_skills()`, `get_skill()` and
`read_skill()` (in `fastmcpp/client/skills.hpp`) need
`set_extension(skills::kSkillsExtensionId)`. `read_skill()` verifies every
file's size and SHA-256 digest; without the extension it reads the
`_manifest` resource instead. The resource-only
`providers::SkillProvider`/`providers::SkillsDirectoryProvider` remain for
compatibility; upstream deprecates them in favour of these.

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

