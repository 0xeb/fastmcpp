cmake_minimum_required(VERSION 3.16)
# CMP0169: Allow FetchContent_Populate() for deps without CMakeLists.txt
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
# major.minor.patch = upstream fastmcp parity level; optional 4th = local revision.
project(fastmcpp VERSION 4.2.6.4 LANGUAGES CXX)

# Empty for a 3-component version, which would expand to FASTMCPP_VERSION_TWEAK=.
if(NOT DEFINED PROJECT_VERSION_TWEAK OR PROJECT_VERSION_TWEAK STREQUAL "")
  set(PROJECT_VERSION_TWEAK 0)
endif()
set(FASTMCPP_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}.${PROJECT_VERSION_TWEAK}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FASTMCPP_BUILD_EXAMPLES "Build examples" ON)
option(FASTMCPP_ENABLE_POST_STREAMING "Enable POST streaming via libcurl (optional)" OFF)
option(FASTMCPP_FETCH_CURL "Fetch and build libcurl statically for POST streaming" ON)
option(FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS "Enable built-in OpenAI/Anthropic sampling handlers (requires libcurl)" OFF)
option(FASTMCPP_ENABLE_OPENSSL "Enable HTTPS/SSL support via OpenSSL (for cpp-httplib)" OFF)

# TLS via Schannel, no extra dependency. Reaches https:// without OpenSSL.
if(WIN32)
  option(FASTMCPP_ENABLE_WINHTTP "Build the WinHTTP client transport (Windows only)" ON)
else()
  set(FASTMCPP_ENABLE_WINHTTP OFF)
endif()

# Portable https://, using whichever TLS backend libcurl binds to. Off by default
# because it pulls libcurl into the build.
option(FASTMCPP_ENABLE_CURL_TRANSPORT "Build the libcurl client transport (all platforms)" OFF)
# Server-side OAuth: JOSE/JWT verification and signing (CNG on Windows, OpenSSL 3 elsewhere).
option(FASTMCPP_ENABLE_SERVER_AUTH "Build server-side OAuth (JWT verification, authorization server)" OFF)

add_library(fastmcpp_core STATIC
    src/types.cpp
    src/util/schema_build.cpp
    src/app.cpp
    src/providers/transforms/namespace.cpp
    src/providers/transforms/tool_transform.cpp
    src/providers/transforms/visibility.cpp
    src/providers/transforms/version_filter.cpp
    src/providers/transforms/prompts_as_tools.cpp
    src/providers/transforms/resources_as_tools.cpp
    src/providers/filesystem_provider.cpp
    src/providers/skills_provider.cpp
    src/providers/openapi_provider.cpp
    src/proxy.cpp
    src/mcp/handler.cpp
    src/mcp/envelope.cpp
    src/mcp/dual_era.cpp
    src/mcp/request_state.cpp
    src/mcp/tasks.cpp
    src/mcp/subscriptions.cpp
    src/resources/resource.cpp
    src/resources/manager.cpp
    src/resources/template.cpp
    src/resources/security.cpp
    src/server/auth_context.cpp
    src/server/sessions.cpp
    src/prompts/prompt.cpp
    src/prompts/manager.cpp
    src/tools/tool.cpp
    src/tools/manager.cpp
    src/server/elicitation.cpp
    src/server/extension.cpp
    src/server/tasks_extension.cpp
    src/server/subscriptions.cpp
    src/server/completions.cpp
    src/skills/frontmatter.cpp
    src/skills/provider.cpp
    src/skills/skill.cpp
    src/server/server.cpp
    src/server/context.cpp
    src/server/middleware.cpp
    src/server/security_middleware.cpp
    src/server/host_origin_guard.cpp
    src/server/response_limiting_middleware.cpp
    src/server/ping_middleware.cpp
    src/server/sampling.cpp
    src/server/http_server.cpp
    src/server/stdio_server.cpp
    src/server/sse_server.cpp
    src/server/streamable_http_server.cpp
    src/server/authorization.cpp
    src/server/auth.cpp
    src/server/auth_middleware.cpp
    src/client/client.cpp
    src/client/sampling_handlers.cpp
    src/client/transports.cpp
    src/client/http_detail.cpp
    src/client/sse_stream.cpp
    src/client/http_rpc_transport.cpp
    src/client/auth.cpp
    src/client/response_cache.cpp
    src/telemetry.cpp
    src/util/json_schema.cpp
    src/util/json_schema_type.cpp
    src/util/linear_regex.cpp
    src/util/crypto.cpp
    src/settings.cpp
)
if(WIN32 AND FASTMCPP_ENABLE_WINHTTP)
  target_sources(fastmcpp_core PRIVATE src/client/winhttp_transport.cpp)
  target_link_libraries(fastmcpp_core PUBLIC winhttp)
  target_compile_definitions(fastmcpp_core PUBLIC FASTMCPP_HAS_WINHTTP)
  message(STATUS "FASTMCPP_ENABLE_WINHTTP=ON: WinHTTP client transport enabled")
endif()

target_include_directories(fastmcpp_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Version header is public
target_compile_definitions(fastmcpp_core PUBLIC
    FASTMCPP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    FASTMCPP_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    FASTMCPP_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    FASTMCPP_VERSION_TWEAK=${PROJECT_VERSION_TWEAK}
    FASTMCPP_VERSION_STRING="${FASTMCPP_VERSION_STRING}")

# MSVC: avoid parallel compilation PDB contention (C1041) in large targets
if(MSVC)
  target_compile_options(fastmcpp_core PRIVATE /FS)
endif()

# nlohmann_json dependency - use existing target if available (e.g., from vcpkg)
if(NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

target_link_libraries(fastmcpp_core PUBLIC nlohmann_json::nlohmann_json)

# Client OAuth (src/client/auth.cpp): BCryptGenRandom for PKCE/state, ShellExecuteW for the browser.
if(WIN32)
  target_link_libraries(fastmcpp_core PUBLIC bcrypt shell32)
endif()

if(FASTMCPP_ENABLE_SERVER_AUTH)
  target_sources(fastmcpp_core PRIVATE
      src/util/bigint.cpp
      src/util/der.cpp
      src/util/eddsa.cpp
      src/util/jose.cpp
      src/server/auth_jwt.cpp)
  if(WIN32)
    target_sources(fastmcpp_core PRIVATE src/util/crypto_backend_cng.cpp)
  else()
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
    target_sources(fastmcpp_core PRIVATE src/util/crypto_backend_openssl.cpp)
    target_link_libraries(fastmcpp_core PUBLIC OpenSSL::Crypto)
  endif()
  target_compile_definitions(fastmcpp_core PUBLIC FASTMCPP_HAS_SERVER_AUTH)
  message(STATUS "FASTMCPP_ENABLE_SERVER_AUTH=ON: server-side OAuth enabled")
endif()

# Header-only HTTP library (cpp-httplib)
#
# If the surrounding project already provides an httplib::httplib target, CONSUME
# IT. cpp-httplib is header-only, so fetching a second private copy here puts two
# sets of inline definitions into one binary. Whenever the two differ — a
# different version, or different CPPHTTPLIB_*_SUPPORT feature macros — the
# classes have different layouts per translation unit, the linker keeps one weak
# definition, and callers read and write through mismatched offsets. It surfaces
# as a crash inside httplib's own mutexes, not as a link error.
include(FetchContent)
if(TARGET httplib::httplib)
  # Same headers AND same feature macros as every other TU in the final binary.
  target_link_libraries(fastmcpp_core PUBLIC httplib::httplib)
  set(FASTMCPP_HTTPLIB_EXTERNAL TRUE)
  message(STATUS "fastmcpp: using the project-provided httplib::httplib target")
else()
  FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
  )
  FetchContent_GetProperties(cpp_httplib)
  if(NOT cpp_httplib_POPULATED)
    FetchContent_Populate(cpp_httplib)
  endif()
  target_include_directories(fastmcpp_core PUBLIC ${cpp_httplib_SOURCE_DIR})
  set(FASTMCPP_HTTPLIB_EXTERNAL FALSE)
  message(STATUS "fastmcpp: fetching a private cpp-httplib (no httplib::httplib target found)")
endif()

# Optional: OpenSSL for HTTPS/SSL support
if(FASTMCPP_ENABLE_OPENSSL)
  find_package(OpenSSL REQUIRED)
  # With an external httplib target, that target decides whether the SSL code
  # paths are compiled in. Defining CPPHTTPLIB_OPENSSL_SUPPORT here anyway would
  # re-open the very layout skew this block avoids, from the other side.
  if(FASTMCPP_HTTPLIB_EXTERNAL)
    message(STATUS "FASTMCPP_ENABLE_OPENSSL=ON: deferring CPPHTTPLIB_OPENSSL_SUPPORT "
                   "to the project-provided httplib::httplib target")
  else()
    target_compile_definitions(fastmcpp_core PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
  endif()
  target_link_libraries(fastmcpp_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "FASTMCPP_ENABLE_OPENSSL=ON: HTTPS/SSL support enabled")
else()
  message(STATUS "FASTMCPP_ENABLE_OPENSSL=OFF: HTTPS/SSL support disabled")
endif()

# Optional: libcurl for POST streaming and sampling handlers (modular)
if(FASTMCPP_ENABLE_POST_STREAMING OR FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS OR FASTMCPP_ENABLE_CURL_TRANSPORT)
  if(FASTMCPP_FETCH_CURL)
    message(STATUS "FASTMCPP_FETCH_CURL=ON: fetching curl via FetchContent (static-only)")
    include(FetchContent)

    # Configure curl for a minimal library build.
    set(BUILD_CURL_EXE OFF CACHE BOOL "Build curl executable" FORCE)
    set(CURL_DISABLE_TESTS ON CACHE BOOL "Disable curl tests" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "Disable curl install targets" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "Disable LDAP support" FORCE)
    # libpsl only vets cookie domains; newer curl requires it unless disabled.
    set(CURL_USE_LIBPSL OFF CACHE BOOL "Do not use libpsl" FORCE)
    # Newer curl auto-enables optional libraries it finds on the host (e.g. Homebrew's
    # libssh2 on macOS), which then fail to link into our static build. The transports
    # need only HTTP(S), so keep those out.
    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "Do not use libssh2" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "Do not use nghttp2" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "Do not use libidn2" FORCE)
    set(CURL_BROTLI OFF CACHE STRING "Do not use brotli" FORCE)
    set(CURL_ZSTD OFF CACHE STRING "Do not use zstd" FORCE)

    # Prefer platform TLS backends to avoid OpenSSL as a dependency.
    if(WIN32)
      set(CURL_USE_SCHANNEL ON CACHE BOOL "Use Windows Schannel for TLS" FORCE)
      set(CURL_USE_OPENSSL OFF CACHE BOOL "Do not use OpenSSL" FORCE)
    endif()

    # Always build curl statically to avoid runtime DLL dependencies.
    if(DEFINED BUILD_SHARED_LIBS)
      set(_fastmcpp_prev_build_shared_libs "${BUILD_SHARED_LIBS}")
    else()
      set(_fastmcpp_prev_build_shared_libs "__UNDEFINED__")
    endif()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)

    FetchContent_Declare(
      curl
      GIT_REPOSITORY https://github.com/curl/curl.git
      # 8.23.0+ builds against OpenSSL 4, which made ASN1_INTEGER opaque.
      GIT_TAG curl-8_23_0
    )
    FetchContent_MakeAvailable(curl)

    if(TARGET libcurl_static AND NOT TARGET CURL::libcurl)
      add_library(CURL::libcurl ALIAS libcurl_static)
    endif()

    if(_fastmcpp_prev_build_shared_libs STREQUAL "__UNDEFINED__")
      unset(BUILD_SHARED_LIBS CACHE)
    else()
      set(BUILD_SHARED_LIBS "${_fastmcpp_prev_build_shared_libs}" CACHE BOOL "Build shared libraries" FORCE)
    endif()
  else()
    # Best effort for users who provide a curl toolchain: request static linkage.
    set(CURL_USE_STATIC_LIBS ON)
    find_package(CURL QUIET)

    if(CURL_FOUND AND NOT TARGET CURL::libcurl)
      add_library(CURL::libcurl UNKNOWN IMPORTED)
      set_target_properties(CURL::libcurl PROPERTIES
        IMPORTED_LOCATION "${CURL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIRS}"
      )
    endif()
  endif()

  if(TARGET CURL::libcurl)
    target_link_libraries(fastmcpp_core PRIVATE CURL::libcurl)
    target_compile_definitions(fastmcpp_core PRIVATE FASTMCPP_HAS_CURL)
    if(FASTMCPP_ENABLE_POST_STREAMING)
      target_compile_definitions(fastmcpp_core PRIVATE FASTMCPP_POST_STREAMING)
    endif()
    if(FASTMCPP_ENABLE_CURL_TRANSPORT)
      target_sources(fastmcpp_core PRIVATE src/client/curl_transport.cpp)
      # PUBLIC: the header is installed, so consumers need the same guard.
      target_compile_definitions(fastmcpp_core PUBLIC FASTMCPP_HAS_CURL_TRANSPORT)
      message(STATUS "FASTMCPP_ENABLE_CURL_TRANSPORT=ON: libcurl client transport enabled")
    endif()
  else()
    if(FASTMCPP_ENABLE_POST_STREAMING)
      message(STATUS "libcurl not found; POST streaming will be disabled at runtime (request_stream_post throws)")
    endif()
    if(FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS)
      message(STATUS "libcurl not found; built-in sampling handlers will be unavailable")
    endif()
    if(FASTMCPP_ENABLE_CURL_TRANSPORT)
      message(FATAL_ERROR "FASTMCPP_ENABLE_CURL_TRANSPORT=ON but libcurl was not found; set FASTMCPP_FETCH_CURL=ON or provide a curl toolchain")
    endif()
  endif()
endif()

# Cross-platform subprocess management
target_sources(fastmcpp_core PRIVATE src/internal/process.cpp)
find_package(Threads REQUIRED)
target_link_libraries(fastmcpp_core PRIVATE Threads::Threads)

# Embedders link fastmcpp_core and never invoke this CLI, but it was unconditional, so
# every consumer compiled and linked it on every build. Default ON keeps standalone
# fastmcpp unchanged; embedders turn it off next to FASTMCPP_BUILD_EXAMPLES.
option(FASTMCPP_BUILD_CLI "Build the fastmcpp command-line executable" ON)
if(FASTMCPP_BUILD_CLI)
  add_executable(fastmcpp src/cli/main.cpp)
  target_link_libraries(fastmcpp PRIVATE fastmcpp_core)
endif()

# End of test scope: examples must not inherit the Debug CRT bootstrap.
set_property(DIRECTORY PROPERTY LINK_LIBRARIES "")

if(FASTMCPP_BUILD_EXAMPLES)
  add_executable(fastmcpp_example_server examples/server_quick_start.cpp)
  target_link_libraries(fastmcpp_example_server PRIVATE fastmcpp_core)

  add_executable(fastmcpp_example_client examples/client_quick_start.cpp)
  target_link_libraries(fastmcpp_example_client PRIVATE fastmcpp_core)

  add_executable(fastmcpp_example_client_api examples/client_api.cpp)
  target_link_libraries(fastmcpp_example_client_api PRIVATE fastmcpp_core)

  # Removed outdated server_toolmanager_handler example target (file no longer exists)

  add_executable(fastmcpp_example_stdio examples/simple_echo.cpp)
  target_link_libraries(fastmcpp_example_stdio PRIVATE fastmcpp_core)

  # stdio_mcp_server target is created conditionally in tests section (line 227-230)
  # to support stdio client test. If building examples, ensure it exists.
  if(NOT TARGET fastmcpp_example_stdio_mcp_server)
    add_executable(fastmcpp_example_stdio_mcp_server examples/stdio_mcp_server.cpp)
    target_link_libraries(fastmcpp_example_stdio_mcp_server PRIVATE fastmcpp_core)
  endif()

  # Mirrored Python examples
  add_executable(fastmcpp_example_config_server examples/config_server.cpp)
  target_link_libraries(fastmcpp_example_config_server PRIVATE fastmcpp_core)

  add_executable(fastmcpp_example_serializer examples/serializer.cpp)
  target_link_libraries(fastmcpp_example_serializer PRIVATE fastmcpp_core)

  add_executable(fastmcpp_example_complex_inputs examples/complex_inputs.cpp)
  target_link_libraries(fastmcpp_example_complex_inputs PRIVATE fastmcpp_core)

  add_executable(fastmcpp_example_tags_example examples/tags_example.cpp)
  target_link_libraries(fastmcpp_example_tags_example PRIVATE fastmcpp_core)

  # MCP Apps example (FastMCP 3.x surface)
  add_executable(fastmcpp_example_mcp_apps examples/mcp_apps.cpp)
  target_link_libraries(fastmcpp_example_mcp_apps PRIVATE fastmcpp_core)

  # Instructions HTTP server (E2E test target)
  add_executable(fastmcpp_example_instructions_server examples/instructions_server.cpp)
  target_link_libraries(fastmcpp_example_instructions_server PRIVATE fastmcpp_core)

  # Skills provider example
  add_executable(fastmcpp_example_skills_provider examples/skills_provider.cpp)
  target_link_libraries(fastmcpp_example_skills_provider PRIVATE fastmcpp_core)

  # OpenAPI provider example
  add_executable(fastmcpp_example_openapi_provider examples/openapi_provider.cpp)
  target_link_libraries(fastmcpp_example_openapi_provider PRIVATE fastmcpp_core)

  # Context API example (v2.13.0+)
  add_executable(fastmcpp_example_context_introspection examples/context_introspection.cpp)
  target_link_libraries(fastmcpp_example_context_introspection PRIVATE fastmcpp_core)

  # Context elicitation example (schema defaults + client callback)
  add_executable(fastmcpp_example_context_elicitation examples/context_elicitation.cpp)
  target_link_libraries(fastmcpp_example_context_elicitation PRIVATE fastmcpp_core)

  # SSE elicitation server example (Context::elicit over ServerSession)
  add_executable(fastmcpp_example_sse_elicitation_server examples/sse_elicitation_server.cpp)
  target_link_libraries(fastmcpp_example_sse_elicitation_server PRIVATE fastmcpp_core)

  # StdioTransport log_file example (v2.13.0+)
  add_executable(fastmcpp_example_stdio_log_file examples/stdio_log_file.cpp)
  target_link_libraries(fastmcpp_example_stdio_log_file PRIVATE fastmcpp_core)

  # Tool Injection Middleware example (v2.13.0+)
  add_executable(fastmcpp_example_tool_injection_middleware examples/tool_injection_middleware.cpp)
  target_link_libraries(fastmcpp_example_tool_injection_middleware PRIVATE fastmcpp_core)

  # Response Limiting Middleware example
  add_executable(fastmcpp_example_response_limiting_middleware examples/response_limiting_middleware.cpp)
  target_link_libraries(fastmcpp_example_response_limiting_middleware PRIVATE fastmcpp_core)

  # Server Metadata example (v2.13.0+)
  add_executable(fastmcpp_example_server_metadata examples/server_metadata.cpp)
  target_link_libraries(fastmcpp_example_server_metadata PRIVATE fastmcpp_core)

  # SSE Robustness example (v2.13.0+)
  add_executable(fastmcpp_example_sse_robustness examples/sse_robustness.cpp)
  target_link_libraries(fastmcpp_example_sse_robustness PRIVATE fastmcpp_core)

  # SSE Inspector Test - Long-running server for MCP Inspector testing
  add_executable(fastmcpp_example_sse_inspector_server examples/sse_inspector_server.cpp)
  target_link_libraries(fastmcpp_example_sse_inspector_server PRIVATE fastmcpp_core)

  # Streaming demo (SSE via GET). This demonstrates the client streaming API.
  add_executable(fastmcpp_example_streaming_demo examples/streaming_demo.cpp)
  target_link_libraries(fastmcpp_example_streaming_demo PRIVATE fastmcpp_core)

  # POST streaming demo (requires libcurl + option enabled)
  if(FASTMCPP_ENABLE_POST_STREAMING)
    add_executable(fastmcpp_example_streaming_post_demo examples/streaming_post_demo.cpp)
    target_link_libraries(fastmcpp_example_streaming_post_demo PRIVATE fastmcpp_core)
  endif()
endif()
