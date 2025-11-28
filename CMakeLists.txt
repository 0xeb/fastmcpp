cmake_minimum_required(VERSION 3.16)
project(fastmcpp VERSION 2.13.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FASTMCPP_BUILD_TESTS "Build tests" ON)
option(FASTMCPP_BUILD_EXAMPLES "Build examples" ON)
option(FASTMCPP_ENABLE_POST_STREAMING "Enable POST streaming via libcurl (optional)" OFF)
option(FASTMCPP_FETCH_CURL "Attempt to fetch libcurl if not found (experimental)" OFF)
option(FASTMCPP_ENABLE_WS_STREAMING_TESTS "Enable WebSocket streaming tests (requires external server)" OFF)
option(FASTMCPP_ENABLE_LOCAL_WS_TEST "Enable local WebSocket server test (depends on httplib ws server support)" OFF)

add_library(fastmcpp_core
    src/types.cpp
    src/util/schema_build.cpp
    src/mcp/handler.cpp
    src/resources/resource.cpp
    src/resources/manager.cpp
    src/prompts/prompt.cpp
    src/prompts/manager.cpp
    src/tools/tool.cpp
    src/tools/manager.cpp
    src/server/server.cpp
    src/server/context.cpp
    src/server/middleware.cpp
    src/server/http_server.cpp
    src/server/stdio_server.cpp
    src/server/sse_server.cpp
    src/client/client.cpp
    src/client/transports.cpp
    src/util/json_schema.cpp
    src/util/json_schema_type.cpp
    src/settings.cpp
)
target_include_directories(fastmcpp_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Version header is public
target_compile_definitions(fastmcpp_core PUBLIC FASTMCPP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} FASTMCPP_VERSION_MINOR=${PROJECT_VERSION_MINOR} FASTMCPP_VERSION_PATCH=${PROJECT_VERSION_PATCH})

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

# Header-only HTTP library (cpp-httplib)
include(FetchContent)
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

# Header-only WebSocket client (easywsclient)
FetchContent_Declare(
  easywsclient
  GIT_REPOSITORY https://github.com/dhbaird/easywsclient.git
  GIT_TAG master
)
FetchContent_GetProperties(easywsclient)
if(NOT easywsclient_POPULATED)
  FetchContent_Populate(easywsclient)
endif()
target_include_directories(fastmcpp_core PUBLIC ${easywsclient_SOURCE_DIR})
target_sources(fastmcpp_core PRIVATE ${easywsclient_SOURCE_DIR}/easywsclient.cpp)

# Optional: libcurl for POST streaming receive support (modular)
if(FASTMCPP_ENABLE_POST_STREAMING)
  find_package(CURL)
  if(NOT CURL_FOUND AND FASTMCPP_FETCH_CURL)
    message(STATUS "CURL not found; FASTMCPP_FETCH_CURL requested (skipping auto-fetch in this build). Please provide CURL via your toolchain or package manager.")
  endif()
  if(CURL_FOUND)
    target_compile_definitions(fastmcpp_core PRIVATE FASTMCPP_POST_STREAMING)
    target_link_libraries(fastmcpp_core PRIVATE CURL::libcurl)
  else()
    message(STATUS "libcurl not found; POST streaming will be disabled at runtime (request_stream_post throws)")
  endif()
endif()

# TinyProcessLib for cross-platform subprocess (header-only)
FetchContent_Declare(
  tiny_process_lib
  GIT_REPOSITORY https://github.com/eidheim/tiny-process-library.git
  GIT_TAG master
)
FetchContent_GetProperties(tiny_process_lib)
if(NOT tiny_process_lib_POPULATED)
  FetchContent_Populate(tiny_process_lib)
endif()
target_include_directories(fastmcpp_core PUBLIC ${tiny_process_lib_SOURCE_DIR})
target_compile_definitions(fastmcpp_core PUBLIC TINY_PROCESS_LIB_AVAILABLE)
target_sources(fastmcpp_core PRIVATE ${tiny_process_lib_SOURCE_DIR}/process.cpp)
if(UNIX)
  target_sources(fastmcpp_core PRIVATE ${tiny_process_lib_SOURCE_DIR}/process_unix.cpp)
elseif(WIN32)
  target_sources(fastmcpp_core PRIVATE ${tiny_process_lib_SOURCE_DIR}/process_win.cpp)
endif()
find_package(Threads REQUIRED)
target_link_libraries(fastmcpp_core PRIVATE Threads::Threads)

add_executable(fastmcpp src/cli/main.cpp)
target_link_libraries(fastmcpp PRIVATE fastmcpp_core)

if(FASTMCPP_BUILD_TESTS)
  enable_testing()
  add_executable(fastmcp_smoke tests/smoke.cpp)
  target_link_libraries(fastmcp_smoke PRIVATE fastmcpp_core)
  add_test(NAME fastmcp_smoke COMMAND fastmcp_smoke)

  add_executable(fastmcp_json_types tests/json_types.cpp)
  target_link_libraries(fastmcp_json_types PRIVATE fastmcpp_core)
  add_test(NAME fastmcp_json_types COMMAND fastmcp_json_types)

  add_executable(fastmcpp_resources tests/resources/basic.cpp)
  target_link_libraries(fastmcpp_resources PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_resources COMMAND fastmcpp_resources)

  add_executable(fastmcpp_prompts tests/prompts/basic.cpp)
  target_link_libraries(fastmcpp_prompts PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_prompts COMMAND fastmcpp_prompts)

  add_executable(fastmcpp_tools tests/tools/basic.cpp)
  target_link_libraries(fastmcpp_tools PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools COMMAND fastmcpp_tools)

  add_executable(fastmcpp_integration tests/integration.cpp)
  target_link_libraries(fastmcpp_integration PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_integration COMMAND fastmcpp_integration)

  add_test(NAME fastmcpp_cli_sum COMMAND fastmcpp client sum 2 3)

  add_executable(fastmcpp_http_integration tests/server/http_integration.cpp)
  target_link_libraries(fastmcpp_http_integration PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_http_integration COMMAND fastmcpp_http_integration)

  add_executable(fastmcpp_http_auth tests/server/http_auth.cpp)
  target_link_libraries(fastmcpp_http_auth PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_http_auth COMMAND fastmcpp_http_auth)

  add_executable(fastmcpp_json_schema tests/schema/json_schema.cpp)
  target_link_libraries(fastmcpp_json_schema PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_json_schema COMMAND fastmcpp_json_schema)

  add_executable(fastmcpp_type_basic tests/schema/type_basic.cpp)
  target_link_libraries(fastmcpp_type_basic PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_type_basic COMMAND fastmcpp_type_basic)

  add_executable(fastmcpp_type_constraints tests/schema/type_constraints.cpp)
  target_link_libraries(fastmcpp_type_constraints PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_type_constraints COMMAND fastmcpp_type_constraints)

  add_executable(fastmcpp_type_composite tests/schema/type_composite.cpp)
  target_link_libraries(fastmcpp_type_composite PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_type_composite COMMAND fastmcpp_type_composite)

  add_executable(fastmcpp_schema_build tests/schema/build.cpp)
  target_link_libraries(fastmcpp_schema_build PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_schema_build COMMAND fastmcpp_schema_build)

  add_executable(fastmcpp_content tests/content.cpp)
  target_link_libraries(fastmcpp_content PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_content COMMAND fastmcpp_content)

  add_executable(fastmcpp_mcp_server tests/mcp/server.cpp)
  target_link_libraries(fastmcpp_mcp_server PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_mcp_server COMMAND fastmcpp_mcp_server)

  add_executable(fastmcpp_mcp_handler tests/mcp/handler.cpp)
  target_link_libraries(fastmcpp_mcp_handler PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_mcp_handler COMMAND fastmcpp_mcp_handler)

  add_executable(fastmcpp_mcp_server_handler tests/mcp/server_handler.cpp)
  target_link_libraries(fastmcpp_mcp_server_handler PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_mcp_server_handler COMMAND fastmcpp_mcp_server_handler)

  add_executable(fastmcpp_mcp_server_toolmanager tests/mcp/server_toolmanager.cpp)
  target_link_libraries(fastmcpp_mcp_server_toolmanager PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_mcp_server_toolmanager COMMAND fastmcpp_mcp_server_toolmanager)

  add_executable(fastmcpp_settings tests/settings.cpp)
  target_link_libraries(fastmcpp_settings PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_settings COMMAND fastmcpp_settings)

  add_executable(fastmcpp_stdio_server tests/transports/stdio_server.cpp)
  target_link_libraries(fastmcpp_stdio_server PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_stdio_server COMMAND fastmcpp_stdio_server)

  add_executable(fastmcpp_sse_server tests/server/sse.cpp)
  target_link_libraries(fastmcpp_sse_server PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_sse_server COMMAND fastmcpp_sse_server)

  # MCP SSE format compliance test (regression test for GitHub Issue #1)
  add_executable(fastmcpp_sse_mcp_format tests/server/sse_mcp_format.cpp)
  target_link_libraries(fastmcpp_sse_mcp_format PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_sse_mcp_format COMMAND fastmcpp_sse_mcp_format)

  # Advanced test suites (Task 3.4)
  add_executable(fastmcpp_tools_validation tests/tools/validation.cpp)
  target_link_libraries(fastmcpp_tools_validation PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools_validation COMMAND fastmcpp_tools_validation)

  add_executable(fastmcpp_tools_edge_cases tests/tools/edge_cases.cpp)
  target_link_libraries(fastmcpp_tools_edge_cases PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools_edge_cases COMMAND fastmcpp_tools_edge_cases)

  add_executable(fastmcpp_resources_advanced tests/resources/advanced.cpp)
  target_link_libraries(fastmcpp_resources_advanced PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_resources_advanced COMMAND fastmcpp_resources_advanced)

  add_executable(fastmcpp_server_basic tests/server/basic.cpp)
  target_link_libraries(fastmcpp_server_basic PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_basic COMMAND fastmcpp_server_basic)

  add_executable(fastmcpp_server_patterns tests/server/patterns.cpp)
  target_link_libraries(fastmcpp_server_patterns PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_patterns COMMAND fastmcpp_server_patterns)

  # Server interactions helpers library (compiled separately to avoid MSVC heap exhaustion)
  add_library(fastmcpp_interactions_helpers STATIC tests/server/interactions_helpers.cpp)
  target_link_libraries(fastmcpp_interactions_helpers PRIVATE fastmcpp_core)

  # Server interactions split into 6 parts
  add_executable(fastmcpp_server_interactions_p1 tests/server/interactions_p1.cpp)
  target_link_libraries(fastmcpp_server_interactions_p1 PRIVATE fastmcpp_core fastmcpp_interactions_helpers)
  add_test(NAME fastmcpp_server_interactions_p1 COMMAND fastmcpp_server_interactions_p1)

  add_executable(fastmcpp_server_interactions_p2 tests/server/interactions_p2.cpp)
  target_link_libraries(fastmcpp_server_interactions_p2 PRIVATE fastmcpp_core fastmcpp_interactions_helpers)
  add_test(NAME fastmcpp_server_interactions_p2 COMMAND fastmcpp_server_interactions_p2)

  add_executable(fastmcpp_server_interactions_p3 tests/server/interactions_p3.cpp)
  target_link_libraries(fastmcpp_server_interactions_p3 PRIVATE fastmcpp_core fastmcpp_interactions_helpers)
  add_test(NAME fastmcpp_server_interactions_p3 COMMAND fastmcpp_server_interactions_p3)

  add_executable(fastmcpp_server_interactions_p4 tests/server/interactions_p4.cpp)
  target_link_libraries(fastmcpp_server_interactions_p4 PRIVATE fastmcpp_core fastmcpp_interactions_helpers)
  add_test(NAME fastmcpp_server_interactions_p4 COMMAND fastmcpp_server_interactions_p4)

  add_executable(fastmcpp_server_interactions_p5 tests/server/interactions_p5.cpp)
  target_link_libraries(fastmcpp_server_interactions_p5 PRIVATE fastmcpp_core fastmcpp_interactions_helpers)
  add_test(NAME fastmcpp_server_interactions_p5 COMMAND fastmcpp_server_interactions_p5)

  add_executable(fastmcpp_server_interactions_p6 tests/server/interactions_p6.cpp)
  target_link_libraries(fastmcpp_server_interactions_p6 PRIVATE fastmcpp_core fastmcpp_interactions_helpers)
  add_test(NAME fastmcpp_server_interactions_p6 COMMAND fastmcpp_server_interactions_p6)

  add_executable(fastmcpp_server_context_meta tests/server/context_meta.cpp)
  target_link_libraries(fastmcpp_server_context_meta PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_context_meta COMMAND fastmcpp_server_context_meta)

  add_executable(fastmcpp_client_transports tests/client/transports.cpp)
  target_link_libraries(fastmcpp_client_transports PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_transports COMMAND fastmcpp_client_transports)

  add_executable(fastmcpp_client_api_basic tests/client/api_basic.cpp)
  target_link_libraries(fastmcpp_client_api_basic PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_api_basic COMMAND fastmcpp_client_api_basic)

  add_executable(fastmcpp_client_api_advanced tests/client/api_advanced.cpp)
  target_link_libraries(fastmcpp_client_api_advanced PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_api_advanced COMMAND fastmcpp_client_api_advanced)

  add_executable(fastmcpp_server_middleware tests/server/middleware.cpp)
  target_link_libraries(fastmcpp_server_middleware PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_middleware COMMAND fastmcpp_server_middleware)

  add_executable(fastmcpp_stdio_client tests/transports/stdio_client.cpp)
  target_link_libraries(fastmcpp_stdio_client PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_stdio_client COMMAND fastmcpp_stdio_client)

  add_executable(fastmcpp_stdio_failure tests/transports/stdio_failure.cpp)
  target_link_libraries(fastmcpp_stdio_failure PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_stdio_failure COMMAND fastmcpp_stdio_failure)
  set_tests_properties(fastmcpp_stdio_client PROPERTIES
    LABELS "conformance"
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:fastmcpp_stdio_client>"
  )

  # Ensure stdio MCP server example is available for stdio client test, even when examples are off.
  if(NOT TARGET fastmcpp_example_stdio_mcp_server)
    add_executable(fastmcpp_example_stdio_mcp_server examples/stdio_mcp_server.cpp)
    target_link_libraries(fastmcpp_example_stdio_mcp_server PRIVATE fastmcpp_core)
  endif()

  option(FASTMCPP_ENABLE_STREAMING_TESTS "Enable streaming SSE tests (experimental)" OFF)
  if(FASTMCPP_ENABLE_STREAMING_TESTS)
    add_executable(fastmcpp_streaming_sse tests/server/streaming_sse.cpp)
    target_link_libraries(fastmcpp_streaming_sse PRIVATE fastmcpp_core)
    add_test(NAME fastmcpp_streaming_sse COMMAND fastmcpp_streaming_sse)
    # Avoid port conflicts with other HTTP tests when running in parallel; currently unstable on some hosts
    set_tests_properties(fastmcpp_streaming_sse PROPERTIES RUN_SERIAL TRUE)
  endif()

  if(FASTMCPP_ENABLE_WS_STREAMING_TESTS)
    add_executable(fastmcpp_ws_streaming tests/transports/ws_streaming.cpp)
    target_link_libraries(fastmcpp_ws_streaming PRIVATE fastmcpp_core)
    add_test(NAME fastmcpp_ws_streaming COMMAND fastmcpp_ws_streaming)
    # Test auto-skips if FASTMCPP_WS_URL is not set

    if(FASTMCPP_ENABLE_LOCAL_WS_TEST)
      add_executable(fastmcpp_ws_streaming_local tests/transports/ws_streaming_local.cpp)
      target_link_libraries(fastmcpp_ws_streaming_local PRIVATE fastmcpp_core)
      add_test(NAME fastmcpp_ws_streaming_local COMMAND fastmcpp_ws_streaming_local)
      set_tests_properties(fastmcpp_ws_streaming_local PROPERTIES RUN_SERIAL TRUE)
    endif()
  endif()
endif()

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

  # Context API example (v2.13.0+)
  add_executable(fastmcpp_example_context_introspection examples/context_introspection.cpp)
  target_link_libraries(fastmcpp_example_context_introspection PRIVATE fastmcpp_core)

  # StdioTransport log_file example (v2.13.0+)
  add_executable(fastmcpp_example_stdio_log_file examples/stdio_log_file.cpp)
  target_link_libraries(fastmcpp_example_stdio_log_file PRIVATE fastmcpp_core)

  # Tool Injection Middleware example (v2.13.0+)
  add_executable(fastmcpp_example_tool_injection_middleware examples/tool_injection_middleware.cpp)
  target_link_libraries(fastmcpp_example_tool_injection_middleware PRIVATE fastmcpp_core)

  # Server Metadata example (v2.13.0+)
  add_executable(fastmcpp_example_server_metadata examples/server_metadata.cpp)
  target_link_libraries(fastmcpp_example_server_metadata PRIVATE fastmcpp_core)

  # SSE Robustness example (v2.13.0+)
  add_executable(fastmcpp_example_sse_robustness examples/sse_robustness.cpp)
  target_link_libraries(fastmcpp_example_sse_robustness PRIVATE fastmcpp_core)

  # SSE Inspector Test - Long-running server for MCP Inspector testing
  add_executable(fastmcpp_example_sse_inspector_test examples/sse_inspector_test.cpp)
  target_link_libraries(fastmcpp_example_sse_inspector_test PRIVATE fastmcpp_core)

  # Streaming demo (SSE via GET). This demonstrates the client streaming API.
  add_executable(fastmcpp_example_streaming_demo examples/streaming_demo.cpp)
  target_link_libraries(fastmcpp_example_streaming_demo PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_example_streaming_demo COMMAND fastmcpp_example_streaming_demo)
  # Example is self-contained but can be flaky in constrained envs
  set_tests_properties(fastmcpp_example_streaming_demo PROPERTIES RUN_SERIAL TRUE)

  # POST streaming demo (requires libcurl + option enabled)
  if(FASTMCPP_ENABLE_POST_STREAMING)
    add_executable(fastmcpp_example_streaming_post_demo examples/streaming_post_demo.cpp)
    target_link_libraries(fastmcpp_example_streaming_post_demo PRIVATE fastmcpp_core)
    if(CURL_FOUND)
      add_test(NAME fastmcpp_example_streaming_post_demo COMMAND fastmcpp_example_streaming_post_demo)
    else()
      message(STATUS "libcurl not found; skipping test fastmcpp_example_streaming_post_demo")
    endif()
  endif()
endif()
