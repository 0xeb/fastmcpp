cmake_minimum_required(VERSION 3.16)
# CMP0169: Allow FetchContent_Populate() for deps without CMakeLists.txt
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
project(fastmcpp VERSION 2.14.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FASTMCPP_BUILD_TESTS "Build tests" ON)
option(FASTMCPP_BUILD_EXAMPLES "Build examples" ON)
option(FASTMCPP_ENABLE_POST_STREAMING "Enable POST streaming via libcurl (optional)" OFF)
option(FASTMCPP_FETCH_CURL "Fetch and build libcurl statically for POST streaming" ON)
option(FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS "Enable built-in OpenAI/Anthropic sampling handlers (requires libcurl)" OFF)

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
    src/mcp/tasks.cpp
    src/resources/resource.cpp
    src/resources/manager.cpp
    src/resources/template.cpp
    src/prompts/prompt.cpp
    src/prompts/manager.cpp
    src/tools/tool.cpp
    src/tools/manager.cpp
    src/server/elicitation.cpp
    src/server/server.cpp
    src/server/context.cpp
    src/server/middleware.cpp
    src/server/security_middleware.cpp
    src/server/response_limiting_middleware.cpp
    src/server/ping_middleware.cpp
    src/server/sampling.cpp
    src/server/http_server.cpp
    src/server/stdio_server.cpp
    src/server/sse_server.cpp
    src/server/streamable_http_server.cpp
    src/client/client.cpp
    src/client/sampling_handlers.cpp
    src/client/transports.cpp
    src/telemetry.cpp
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

# Optional: libcurl for POST streaming and sampling handlers (modular)
if(FASTMCPP_ENABLE_POST_STREAMING OR FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS)
  if(FASTMCPP_FETCH_CURL)
    message(STATUS "FASTMCPP_FETCH_CURL=ON: fetching curl via FetchContent (static-only)")
    include(FetchContent)

    # Configure curl for a minimal library build.
    set(BUILD_CURL_EXE OFF CACHE BOOL "Build curl executable" FORCE)
    set(CURL_DISABLE_TESTS ON CACHE BOOL "Disable curl tests" FORCE)
    set(CURL_DISABLE_INSTALL ON CACHE BOOL "Disable curl install targets" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "Disable LDAP support" FORCE)

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
      GIT_TAG curl-8_9_1
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
  else()
    if(FASTMCPP_ENABLE_POST_STREAMING)
      message(STATUS "libcurl not found; POST streaming will be disabled at runtime (request_stream_post throws)")
    endif()
    if(FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS)
      message(STATUS "libcurl not found; built-in sampling handlers will be unavailable")
    endif()
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

  add_executable(fastmcpp_tools_transform tests/tools/test_tool_transform.cpp)
  target_link_libraries(fastmcpp_tools_transform PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools_transform COMMAND fastmcpp_tools_transform)

  add_executable(fastmcpp_tools_transform_ext tests/tools/test_tool_transform_extended.cpp)
  target_link_libraries(fastmcpp_tools_transform_ext PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools_transform_ext COMMAND fastmcpp_tools_transform_ext)

  add_executable(fastmcpp_tools_manager tests/tools/test_tool_manager.cpp)
  target_link_libraries(fastmcpp_tools_manager PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools_manager COMMAND fastmcpp_tools_manager)

  add_executable(fastmcpp_tools_timeout tests/tools/test_tool_timeout.cpp)
  target_link_libraries(fastmcpp_tools_timeout PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools_timeout COMMAND fastmcpp_tools_timeout)

  add_executable(fastmcpp_integration tests/integration.cpp)
  target_link_libraries(fastmcpp_integration PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_integration COMMAND fastmcpp_integration)

  add_test(NAME fastmcpp_cli_sum COMMAND fastmcpp client sum 2 3)
  add_test(NAME fastmcpp_cli_tasks_help COMMAND fastmcpp tasks --help)
  add_test(NAME fastmcpp_cli_tasks_demo COMMAND fastmcpp tasks demo)
  add_executable(fastmcpp_cli_tasks_ux tests/cli/tasks_cli.cpp)
  add_test(NAME fastmcpp_cli_tasks_ux COMMAND fastmcpp_cli_tasks_ux)
  add_executable(fastmcpp_cli_generated_e2e tests/cli/generated_cli_e2e.cpp)
  target_link_libraries(fastmcpp_cli_generated_e2e PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_cli_generated_e2e COMMAND fastmcpp_cli_generated_e2e)

  add_executable(fastmcpp_http_integration tests/server/http_integration.cpp)   
  target_link_libraries(fastmcpp_http_integration PRIVATE fastmcpp_core)        
  add_test(NAME fastmcpp_http_integration COMMAND fastmcpp_http_integration)    

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

  add_executable(fastmcpp_schema_dereference_toggle tests/schema/dereference_toggle.cpp)
  target_link_libraries(fastmcpp_schema_dereference_toggle PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_schema_dereference_toggle COMMAND fastmcpp_schema_dereference_toggle)

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

  add_executable(fastmcpp_sse_tasks_notifications tests/server/sse_tasks_notifications.cpp)
  target_link_libraries(fastmcpp_sse_tasks_notifications PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_sse_tasks_notifications COMMAND fastmcpp_sse_tasks_notifications)

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

  add_executable(fastmcpp_resources_templates tests/resources/templates.cpp)
  target_link_libraries(fastmcpp_resources_templates PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_resources_templates COMMAND fastmcpp_resources_templates)

  add_executable(fastmcpp_server_basic tests/server/basic.cpp)
  target_link_libraries(fastmcpp_server_basic PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_basic COMMAND fastmcpp_server_basic)

  add_executable(fastmcpp_server_patterns tests/server/patterns.cpp)
  target_link_libraries(fastmcpp_server_patterns PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_patterns COMMAND fastmcpp_server_patterns)

  # Split into 4 parts to avoid MSVC heap exhaustion on CI (C1060 + OutOfMemoryException)
  add_executable(fastmcpp_server_interactions_part1 tests/server/interactions_part1.cpp)
  target_link_libraries(fastmcpp_server_interactions_part1 PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_interactions_part1 COMMAND fastmcpp_server_interactions_part1)

  add_executable(fastmcpp_server_interactions_part2a tests/server/interactions_part2a.cpp)
  target_link_libraries(fastmcpp_server_interactions_part2a PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_interactions_part2a COMMAND fastmcpp_server_interactions_part2a)

  add_executable(fastmcpp_server_interactions_part2b tests/server/interactions_part2b.cpp)
  target_link_libraries(fastmcpp_server_interactions_part2b PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_interactions_part2b COMMAND fastmcpp_server_interactions_part2b)

  add_executable(fastmcpp_server_interactions_part3 tests/server/interactions_part3.cpp)
  target_link_libraries(fastmcpp_server_interactions_part3 PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_interactions_part3 COMMAND fastmcpp_server_interactions_part3)

  add_executable(fastmcpp_server_context_meta tests/server/context_meta.cpp)
  target_link_libraries(fastmcpp_server_context_meta PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_context_meta COMMAND fastmcpp_server_context_meta)
  add_executable(fastmcpp_server_context_full tests/server/test_context_full.cpp)
  target_link_libraries(fastmcpp_server_context_full PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_context_full COMMAND fastmcpp_server_context_full)

  add_executable(fastmcpp_server_context_sse_integration tests/server/test_context_sse_integration.cpp)
  target_link_libraries(fastmcpp_server_context_sse_integration PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_context_sse_integration COMMAND fastmcpp_server_context_sse_integration)

  add_executable(fastmcpp_server_context_sampling tests/server/test_context_sampling.cpp)
  target_link_libraries(fastmcpp_server_context_sampling PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_context_sampling COMMAND fastmcpp_server_context_sampling)

  add_executable(fastmcpp_server_sampling_tools tests/server/test_sampling_tools.cpp)
  target_link_libraries(fastmcpp_server_sampling_tools PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_sampling_tools COMMAND fastmcpp_server_sampling_tools)

  add_executable(fastmcpp_server_elicitation_defaults tests/server/test_elicitation_defaults.cpp)
  target_link_libraries(fastmcpp_server_elicitation_defaults PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_elicitation_defaults COMMAND fastmcpp_server_elicitation_defaults)

  add_executable(fastmcpp_server_session tests/server/test_server_session.cpp)
  target_link_libraries(fastmcpp_server_session PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_session COMMAND fastmcpp_server_session)

  add_executable(fastmcpp_server_security_limits tests/server/security_limits.cpp)
  target_link_libraries(fastmcpp_server_security_limits PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_security_limits COMMAND fastmcpp_server_security_limits)

  add_executable(fastmcpp_server_sse_session_security tests/server/sse_session_security.cpp)
  target_link_libraries(fastmcpp_server_sse_session_security PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_sse_session_security COMMAND fastmcpp_server_sse_session_security)

  # SSE session security with fastmcpp::client::HttpTransport (not raw httplib)
  add_executable(fastmcpp_client_sse_session_client tests/client/sse_session_client.cpp)
  target_link_libraries(fastmcpp_client_sse_session_client PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_sse_session_client COMMAND fastmcpp_client_sse_session_client)

  # SSE + HTTP integration (real network, not LoopbackTransport)
  add_executable(fastmcpp_server_sse_http_integration tests/server/sse_http_integration.cpp)
  target_link_libraries(fastmcpp_server_sse_http_integration PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_sse_http_integration COMMAND fastmcpp_server_sse_http_integration)

  # SSE bidirectional server-initiated requests (ServerSession -> client -> response)
  add_executable(fastmcpp_server_sse_bidirectional_requests tests/server/sse_bidirectional_requests.cpp)
  target_link_libraries(fastmcpp_server_sse_bidirectional_requests PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_sse_bidirectional_requests COMMAND fastmcpp_server_sse_bidirectional_requests)

  # Streamable HTTP integration (MCP spec 2025-03-26)
  add_executable(fastmcpp_server_streamable_http_integration tests/server/streamable_http_integration.cpp)
  target_link_libraries(fastmcpp_server_streamable_http_integration PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_streamable_http_integration COMMAND fastmcpp_server_streamable_http_integration)

  add_executable(fastmcpp_server_auth_cors_security tests/server/auth_cors_security.cpp)
  target_link_libraries(fastmcpp_server_auth_cors_security PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_auth_cors_security COMMAND fastmcpp_server_auth_cors_security)

  add_executable(fastmcpp_server_security_middleware tests/server/security_middleware.cpp)
  target_link_libraries(fastmcpp_server_security_middleware PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_security_middleware COMMAND fastmcpp_server_security_middleware)

  add_executable(fastmcpp_client_transports tests/client/transports.cpp)
  target_link_libraries(fastmcpp_client_transports PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_transports COMMAND fastmcpp_client_transports)

  add_executable(fastmcpp_client_http_client_security tests/client/http_client_security.cpp)
  target_link_libraries(fastmcpp_client_http_client_security PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_http_client_security COMMAND fastmcpp_client_http_client_security)

  add_executable(fastmcpp_client_api_basic tests/client/api_basic.cpp)
  target_link_libraries(fastmcpp_client_api_basic PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_api_basic COMMAND fastmcpp_client_api_basic)

  add_executable(fastmcpp_client_api_advanced tests/client/api_advanced.cpp)
  target_link_libraries(fastmcpp_client_api_advanced PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_api_advanced COMMAND fastmcpp_client_api_advanced)

  add_executable(fastmcpp_client_api_icons tests/client/api_icons.cpp)
  target_link_libraries(fastmcpp_client_api_icons PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_api_icons COMMAND fastmcpp_client_api_icons)

  add_executable(fastmcpp_client_tasks tests/client/tasks.cpp)
  target_link_libraries(fastmcpp_client_tasks PRIVATE fastmcpp_core)      
  add_test(NAME fastmcpp_client_tasks COMMAND fastmcpp_client_tasks)      

  add_executable(fastmcpp_client_sampling_handlers tests/client/sampling_handlers.cpp)
  target_link_libraries(fastmcpp_client_sampling_handlers PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_client_sampling_handlers COMMAND fastmcpp_client_sampling_handlers)

  add_executable(fastmcpp_server_middleware tests/server/middleware.cpp)
  target_link_libraries(fastmcpp_server_middleware PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_middleware COMMAND fastmcpp_server_middleware)

  add_executable(fastmcpp_server_middleware_pipeline tests/server/test_middleware_pipeline.cpp)
  target_link_libraries(fastmcpp_server_middleware_pipeline PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_server_middleware_pipeline COMMAND fastmcpp_server_middleware_pipeline)

  add_executable(fastmcpp_telemetry tests/telemetry/tracing.cpp)
  target_link_libraries(fastmcpp_telemetry PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_telemetry COMMAND fastmcpp_telemetry)

  add_executable(fastmcpp_stdio_client tests/transports/stdio_client.cpp)
  target_link_libraries(fastmcpp_stdio_client PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_stdio_client COMMAND fastmcpp_stdio_client)

  add_executable(fastmcpp_stdio_failure tests/transports/stdio_failure.cpp)
  target_link_libraries(fastmcpp_stdio_failure PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_stdio_failure COMMAND fastmcpp_stdio_failure)

  # App mounting tests
  add_executable(fastmcpp_app_mounting tests/app/mounting.cpp)
  target_link_libraries(fastmcpp_app_mounting PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_app_mounting COMMAND fastmcpp_app_mounting)

  # App ergonomics tests
  add_executable(fastmcpp_app_ergonomics tests/app/ergonomics.cpp)
  target_link_libraries(fastmcpp_app_ergonomics PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_app_ergonomics COMMAND fastmcpp_app_ergonomics)

  # MCP Apps metadata parity tests
  add_executable(fastmcpp_app_mcp_apps tests/app/mcp_apps.cpp)
  target_link_libraries(fastmcpp_app_mcp_apps PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_app_mcp_apps COMMAND fastmcpp_app_mcp_apps)

  # Filesystem provider tests
  add_library(fastmcpp_fs_test_plugin SHARED tests/fs/test_plugin.cpp)
  target_compile_definitions(fastmcpp_fs_test_plugin PRIVATE FASTMCPP_PROVIDER_EXPORTS)
  target_link_libraries(fastmcpp_fs_test_plugin PRIVATE nlohmann_json::nlohmann_json)
  target_include_directories(fastmcpp_fs_test_plugin PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  )

  add_executable(fastmcpp_fs_provider tests/fs/provider.cpp)
  target_link_libraries(fastmcpp_fs_provider PRIVATE fastmcpp_core)
  add_dependencies(fastmcpp_fs_provider fastmcpp_fs_test_plugin)
  add_test(NAME fastmcpp_fs_provider COMMAND fastmcpp_fs_provider)
  set_tests_properties(fastmcpp_fs_provider PROPERTIES
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:fastmcpp_fs_provider>"
  )

  add_executable(fastmcpp_provider_transforms tests/providers/transforms.cpp)
  target_link_libraries(fastmcpp_provider_transforms PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_provider_transforms COMMAND fastmcpp_provider_transforms)

  add_executable(fastmcpp_provider_version_filter tests/providers/version_filter.cpp)
  target_link_libraries(fastmcpp_provider_version_filter PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_provider_version_filter COMMAND fastmcpp_provider_version_filter)

  add_executable(fastmcpp_provider_skills tests/providers/skills_provider.cpp)
  target_link_libraries(fastmcpp_provider_skills PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_provider_skills COMMAND fastmcpp_provider_skills)

  add_executable(fastmcpp_provider_openapi tests/providers/openapi_provider.cpp)
  target_link_libraries(fastmcpp_provider_openapi PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_provider_openapi COMMAND fastmcpp_provider_openapi)

  # Proxy tests
  add_executable(fastmcpp_proxy_basic tests/proxy/basic.cpp)
  target_link_libraries(fastmcpp_proxy_basic PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_proxy_basic COMMAND fastmcpp_proxy_basic)

  # Main header compile test
  add_executable(fastmcpp_main_header tests/main_header/compile_test.cpp)
  target_link_libraries(fastmcpp_main_header PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_main_header COMMAND fastmcpp_main_header)

  # Sync-cycle tests (Phase 11)
  add_executable(fastmcpp_mcp_error_codes tests/mcp/test_error_codes.cpp)
  target_link_libraries(fastmcpp_mcp_error_codes PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_mcp_error_codes COMMAND fastmcpp_mcp_error_codes)

  add_executable(fastmcpp_pagination tests/mcp/test_pagination.cpp)
  target_link_libraries(fastmcpp_pagination PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_pagination COMMAND fastmcpp_pagination)

  add_executable(fastmcpp_tools_transform_enabled tests/tools/test_tool_transform_enabled.cpp)
  target_link_libraries(fastmcpp_tools_transform_enabled PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools_transform_enabled COMMAND fastmcpp_tools_transform_enabled)

  add_executable(fastmcpp_tools_sequential tests/tools/test_tool_sequential.cpp)
  target_link_libraries(fastmcpp_tools_sequential PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_tools_sequential COMMAND fastmcpp_tools_sequential)

  add_executable(fastmcpp_session_state tests/server/test_session_state.cpp)
  target_link_libraries(fastmcpp_session_state PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_session_state COMMAND fastmcpp_session_state)

  add_executable(fastmcpp_response_limiting tests/server/test_response_limiting.cpp)
  target_link_libraries(fastmcpp_response_limiting PRIVATE fastmcpp_core)
  add_test(NAME fastmcpp_response_limiting COMMAND fastmcpp_response_limiting)

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

  # POST streaming transport test (requires libcurl)
  if(FASTMCPP_ENABLE_POST_STREAMING AND TARGET CURL::libcurl)
    add_executable(fastmcpp_post_streaming tests/transports/post_streaming.cpp)
    target_link_libraries(fastmcpp_post_streaming PRIVATE fastmcpp_core)
    add_test(NAME fastmcpp_post_streaming COMMAND fastmcpp_post_streaming)
    set_tests_properties(fastmcpp_post_streaming PROPERTIES RUN_SERIAL TRUE)
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

  # MCP Apps example (FastMCP 3.x surface)
  add_executable(fastmcpp_example_mcp_apps examples/mcp_apps.cpp)
  target_link_libraries(fastmcpp_example_mcp_apps PRIVATE fastmcpp_core)

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
    if(TARGET CURL::libcurl)
      add_test(NAME fastmcpp_example_streaming_post_demo COMMAND fastmcpp_example_streaming_post_demo)
    else()
      message(STATUS "libcurl not found; skipping test fastmcpp_example_streaming_post_demo")
    endif()
  endif()
endif()
