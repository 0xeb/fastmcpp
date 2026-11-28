<div align="center">

<img src="logo.jpg" width="550" alt="fastmcpp logo">

**C++ implementation of the Model Context Protocol (MCP)**

[![CI](https://github.com/0xeb/fastmcpp/actions/workflows/ci.yml/badge.svg)](https://github.com/0xeb/fastmcpp/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/0xeb/fastmcpp.svg)](https://github.com/0xeb/fastmcpp/blob/main/LICENSE)

</div>

---

fastmcpp is a C++ port of [fastmcp](https://github.com/PrefectHQ/fastmcp): MCP servers and clients
with tools, resources, prompts, providers and middleware, over STDIO, HTTP/SSE and Streamable HTTP.

**Version:** 4.2.6.5. The first three numbers are the fastmcp release it matches; the fourth is a
fastmcpp-only fix on top.

## Features

- Server and client for MCP `2025-11-25` (default) and earlier handshake revisions, negotiated per
  client, plus the sessionless `2026-07-28` protocol from the same server.
- Transports: STDIO, HTTP/SSE, Streamable HTTP. HTTPS clients through WinHTTP (Windows) or libcurl
  (any platform).
- `FastMCP` application: tools, resources, resource templates, prompts, mounting, providers
  (local, filesystem, OpenAPI, skills, proxy) and transforms (namespace, visibility, tool
  transform, version filter, search, prompts/resources as tools).
- Middleware: logging, timing, caching, rate limiting, error handling, ping, response limiting.
- Elicitation, sampling, progress, completions, `subscriptions/listen`, background tasks,
  extensions and skills.
- Client auth: bearer token, OAuth (discovery, dynamic registration, PKCE, refresh), client
  credentials and `private_key_jwt` (RFC 7523).
- Server auth: JWT, introspection and static-token verifiers, protected resource metadata,
  `MultiAuth`, and per-component authorization (`require_scopes`, `restrict_tag`,
  `require_roles`, `AuthMiddleware`).
- OAuth authorization server: `/authorize`, `/token`, `/register`, `/revoke`, RFC 8414 metadata,
  Client ID Metadata Documents (CIMD) with `private_key_jwt`, `JWTIssuer`, identity assertion
  grants (ID-JAG), SSRF-safe fetching and redirect URI validation.
- `fastmcpp` command-line tool: discover, list and call servers; install server configs; create
  and validate CIMD documents (`fastmcpp auth cimd`).
- Windows, Linux, macOS.

## Building

Requires a C++17 compiler and CMake 3.16+. Dependencies (nlohmann/json, cpp-httplib, libcurl when
needed) are fetched automatically.

```bash
git clone https://github.com/0xeb/fastmcpp.git
cd fastmcpp
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

| Option | Default | Description |
|---|---|---|
| `FASTMCPP_BUILD_EXAMPLES` | ON | Build the example programs |
| `FASTMCPP_BUILD_CLI` | ON | Build the `fastmcpp` executable |
| `FASTMCPP_ENABLE_WINHTTP` | ON (Windows) | WinHTTP client transport (HTTPS via Schannel) |
| `FASTMCPP_ENABLE_CURL_TRANSPORT` | OFF | libcurl client transport (HTTPS on every platform) |
| `FASTMCPP_ENABLE_POST_STREAMING` | OFF | Streamed POST responses (libcurl) |
| `FASTMCPP_FETCH_CURL` | ON | Fetch and build libcurl when it is not found |
| `FASTMCPP_ENABLE_OPENSSL` | OFF | HTTPS for the cpp-httplib transports |
| `FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS` | OFF | Built-in OpenAI/Anthropic sampling handlers |
| `FASTMCPP_ENABLE_SERVER_AUTH` | OFF | JOSE: `JWTVerifier`, `JWTIssuer`, CIMD and ID-JAG assertions, `private_key_jwt` signing (CNG on Windows, OpenSSL 3 elsewhere) |

### HTTPS

The default cpp-httplib transports have no TLS, so an `https://` URL fails with `'https' scheme is
not supported`. Use `client::WinHttpTransport` on Windows (built by default) or
`client::CurlTransport` (`FASTMCPP_ENABLE_CURL_TRANSPORT=ON`); both drop into `Client` and support
the per-request header provider and auth. `FASTMCPP_ENABLE_OPENSSL=ON` also works, but if your
project supplies its own `httplib::httplib` target, that target must itself be built with OpenSSL.

## Quick start

### Server

```cpp
#include <fastmcpp/app.hpp>
#include <fastmcpp/mcp/handler.hpp>
#include <fastmcpp/server/stdio_server.hpp>

int main()
{
    using fastmcpp::Json;
    fastmcpp::FastMCP app("calculator", "1.0.0");
    app.tool("add", Json{{"a", "number"}, {"b", "number"}},
             [](const Json& args) { return args.at("a").get<double>() + args.at("b").get<double>(); });

    fastmcpp::server::StdioServerWrapper server(fastmcpp::mcp::make_mcp_handler(app));
    server.run();
}
```

Serve the same handler over Streamable HTTP or SSE instead:

```cpp
fastmcpp::server::StreamableHttpServerWrapper http(handler, "127.0.0.1", 8080, "/mcp");
http.start();
```

### Client

```cpp
#include <fastmcpp/client/client.hpp>
#include <fastmcpp/client/transports.hpp>

fastmcpp::client::Client client(
    std::make_unique<fastmcpp::client::StreamableHttpTransport>("http://localhost:8080", "/mcp"));
client.connect(); // sessionless 2026-07-28, falling back to the initialize handshake
for (const auto& tool : client.list_tools())
    std::cout << tool.name << "\n";
std::cout << client.call_tool("add", {{"a", 2}, {"b", 3}}).text() << "\n";
```

### Proxy

```cpp
auto proxy = fastmcpp::create_proxy("http://backend:8080/mcp");
proxy.local_tools().register_tool(my_tool); // local components win over remote ones
fastmcpp::server::SseServerWrapper server(fastmcpp::mcp::make_mcp_handler(proxy), "127.0.0.1", 8080);
server.start();
```

### Client auth

```cpp
transport->set_auth(std::make_shared<client::auth::BearerAuth>("my-token"));
transport->set_auth(std::make_shared<client::auth::OAuth>()); // full OAuth flow
```

`OAuth` opens the system browser and waits on a loopback callback by default; set
`OAuthOptions::redirect_handler`/`callback_handler` for headless use and
`OAuthOptions::token_storage` to keep tokens across runs.

For machine-to-machine access, `PrivateKeyJWTOAuth` runs the `client_credentials` grant with a
signed client assertion (needs `FASTMCPP_ENABLE_SERVER_AUTH=ON` to sign):

```cpp
client::auth::SignedJWTParameters jwt;
jwt.issuer = jwt.subject = "https://app.example.com/client.json";
jwt.signing_key = private_key_pem;                  // RS/PS/ES 256/384/512
client::auth::PrivateKeyJWTOptions o;
o.client_id = jwt.issuer;
o.assertion_provider = jwt.create_assertion_provider();
o.issuer = "https://auth.example.com";              // only send assertions to this AS
transport->set_auth(std::make_shared<client::auth::PrivateKeyJWTOAuth>(o));
```

### Server auth

```cpp
using namespace fastmcpp::server;
auth::JWTVerifierOptions jwt;                       // FASTMCPP_ENABLE_SERVER_AUTH=ON
jwt.jwks_uri = "https://auth.example.com/.well-known/jwks.json";
jwt.issuer = "https://auth.example.com";
jwt.audience = "https://api.example.com/mcp";
app.set_auth(std::make_shared<auth::RemoteAuthProvider>(
    std::make_shared<auth::JWTVerifier>(jwt),
    std::vector<std::string>{"https://auth.example.com"}, "https://api.example.com"));

FastMCP::ToolOptions admin;
admin.auth = {require_scopes({"admin"})};          // hidden from tokens without "admin"
app.tool("purge", purge_fn, admin);

StreamableHttpServerWrapper srv(mcp::make_mcp_handler(app), "0.0.0.0", 8080);
srv.set_auth(app.auth());                           // also SseServerWrapper, HttpServerWrapper
```

Requests without a valid bearer token get `401` with a `WWW-Authenticate` challenge pointing at
`/.well-known/oauth-protected-resource/mcp`; a token missing a required scope gets `403`.
Component checks are skipped on STDIO.

To be the authorization server as well, give the HTTP server an `OAuthProvider`. Subclass it to
store clients and tokens your way, or start with `InMemoryOAuthProvider`:

```cpp
auth::OAuthProviderOptions o;
o.base_url = "http://127.0.0.1:8000";
o.client_registration_options = auth::ClientRegistrationOptions{true};
StreamableHttpServerWrapper srv(mcp::make_mcp_handler(app), "127.0.0.1", 8000);
srv.set_auth(std::make_shared<auth::InMemoryOAuthProvider>(o));
```

`CIMDClientManager`, `PrivateKeyJWTClientAuthenticator`, `JWTIssuer` and
`IdentityAssertionValidator` are the parts for custom providers that accept CIMD clients, sign
their own tokens or take identity assertion grants.

## Protocol notes

- **Two protocol eras from one server.** On Streamable HTTP a request whose `MCP-Protocol-Version`
  is not a handshake revision is served sessionlessly: no `Mcp-Session-Id`, POST only, the envelope
  in `params._meta`. On STDIO the first request decides. SSE is handshake-only.
- **No server-initiated requests on the sessionless era.** `Context::elicit()` and
  `Context::sample()` throw there; ask for input by returning `mcp::input_required(...)` instead
  (below).
- **Tool errors are results.** Bad arguments, an unknown tool or an exception from the tool body
  come back as `isError: true`. Middleware sees them first, as `ValidationError`, `NotFoundError`
  or `fastmcpp::Error`, and can turn them into JSON-RPC errors:

  ```cpp
  struct JsonRpcArgumentErrors : fastmcpp::server::Middleware
  {
      fastmcpp::Json on_call_tool(const fastmcpp::server::MiddlewareContext& ctx,
                                  fastmcpp::server::CallNext next) override
      {
          try { return next(ctx); }
          catch (const fastmcpp::ValidationError& e)
          {
              return {{"jsonrpc", "2.0"},
                      {"id", ctx.message.value("id", fastmcpp::Json())},
                      {"error", {{"code", -32602}, {"message", e.what()}}}};
          }
      }
  };
  ```

- **Resource not found is `-32602`**, with `data.uri`; an unknown prompt is `-32602` too.
  Malformed JSON-RPC messages are answered `-32600 Invalid Request`.
- **Component metadata** is in `_meta.fastmcp` (`tags`, `version`). fastmcp 4 servers list static
  resources with a trailing slash (`file://readme.txt/`); read them by the listed URI.
- **Host/Origin protection** (DNS rebinding) for the HTTP servers is off by default. Enable it with
  `set_host_origin_guard(...)` or `FASTMCPP_HTTP_HOST_ORIGIN_PROTECTION=auto|strict`, adding hosts
  and origins with `FASTMCPP_HTTP_ALLOWED_HOSTS` / `FASTMCPP_HTTP_ALLOWED_ORIGINS`.

## Asking the client for input

On `2026-07-28` a handler that needs input *returns* what it needs; the client answers with its
elicitation, sampling or roots callback and calls again, and the handler runs again from the top.

```cpp
#include "fastmcpp/mcp/input_required.hpp"

app.tool("book_flight", schema, [](const Json& args) -> Json {
    auto answers = mcp::input_responses_of(args); // nullopt on the first round
    if (!answers)
        return mcp::input_required({{"dest", mcp::elicit_input("Where to?", dest_schema)}});
    return "Booked " + (*answers)["dest"]["content"]["dest"].get<std::string>();
});
```

State carried between rounds (`mcp::request_state_of`) is sealed with AES-256-GCM in the MCP
Python SDK's format and expires after 10 minutes. Replicas behind one endpoint need a shared key:
`StreamableHttpServerWrapper::set_request_state_security(mcp::RequestStateSecurity::from_keys({secret}))`.
`Client::call_tool()`, `get_prompt()` and `read_resource()` drive the rounds automatically.

## Extensions, tasks and skills

- **Extensions:** a `server::ServerExtension` adds a capability under
  `capabilities.extensions[id]`, request methods and a `tools/call` interceptor
  (`FastMCP::add_extension()`); clients opt in with `Client::set_extension(id)`.
- **Background tasks:** add `server::TasksExtension` and give a tool
  `ToolOptions::task_support = TaskSupport::Optional` (or `Required`). `call_tool()` follows a task
  to its result; `call_tool_task()` returns a handle. Tasks live in process memory.
- **Skills:** serve `SKILL.md` bundles with `skills::SkillCatalogProvider` or
  `skills::SkillsDirectoryProvider`; clients use `client::skills::list_skills()`, `get_skill()` and
  `read_skill()` (which verifies sizes and SHA-256 digests).

## Examples

`examples/` has complete programs, among them `simple_echo.cpp` (STDIO server),
`stdio_instructions_server.cpp` (`FastMCP` app), `client_api.cpp`, `openapi_provider.cpp`,
`skills_provider.cpp`, `context_elicitation.cpp` and `tool_injection_middleware.cpp`.

## Projects using fastmcpp

| Project | Description |
|---|---|
| [copilot-sdk-cpp](https://github.com/0xeb/copilot-sdk-cpp) | C++ SDK for GitHub Copilot CLI |
| [claude-agent-sdk-cpp](https://github.com/0xeb/claude-agent-sdk-cpp) | C++ SDK for Claude Code CLI with MCP support |

## License

Copyright 2025 Elias Bachaalany ([@0xeb](https://github.com/0xeb)). Apache License 2.0; see
`LICENSE` and `NOTICE`. fastmcpp is a port of [fastmcp](https://github.com/PrefectHQ/fastmcp) by
Jeremiah Lowin, which remains the reference implementation.

Issues: <https://github.com/0xeb/fastmcpp/issues>.
