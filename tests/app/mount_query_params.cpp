// Tests that resource templates carrying query params survive mounting.
// Parity intent with Python fastmcp commit cb341911 (Fix resource templates
// with query params on mounted servers).
//
// fastmcpp's mount model is direct-dispatch (parent strips prefix and forwards
// the URI to the mounted child), so the query string is preserved end-to-end
// without needing the FastMCPProvider-style {?param} expansion that the Python
// fix targets. These tests lock that behavior so future changes do not
// regress it.

#include "fastmcpp/app.hpp"
#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/resources/template.hpp"

#include <iostream>
#include <string>

using namespace fastmcpp;
using fastmcpp::resources::ResourceContent;
using fastmcpp::resources::ResourceTemplate;

#define ASSERT_TRUE(cond, msg)                                                                     \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b, msg)                                                                       \
    do                                                                                             \
    {                                                                                              \
        if (!((a) == (b)))                                                                         \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

namespace
{
ResourceTemplate make_weather_template()
{
    ResourceTemplate t;
    t.uri_template = "weather://{city}/current{?units,detail}";
    t.name = "weather";
    t.parameters = Json{
        {"type", "object"},
        {"properties",
         Json{
             {"city", Json{{"type", "string"}}},
             {"units", Json{{"type", "string"}}},
             {"detail", Json{{"type", "boolean"}}},
         }},
    };
    t.provider = [](const Json& params) -> ResourceContent
    {
        // Echo back the params so the test can verify everything came through.
        return ResourceContent{"weather://echo", "application/json", params.dump()};
    };
    t.parse();
    return t;
}
} // namespace

static int test_mount_preserves_query_params()
{
    std::cout << "  test_mount_preserves_query_params..." << std::endl;
    FastMCP child("weather_app", "1.0.0");
    child.resources().register_template(make_weather_template());

    FastMCP parent("main", "1.0.0");
    parent.mount(child, "forecast");

    // Client requests via the parent-namespaced URI with query params.
    auto content =
        parent.read_resource("weather://forecast/paris/current?units=metric&detail=true");
    auto parsed = Json::parse(std::get<std::string>(content.data));
    ASSERT_TRUE(parsed.is_object(), "params arrived as object");
    ASSERT_EQ(parsed.value("city", std::string{}), std::string("paris"), "city");
    ASSERT_EQ(parsed.value("units", std::string{}), std::string("metric"), "units");
    ASSERT_TRUE(parsed["detail"].is_boolean() && parsed["detail"].get<bool>() == true,
                "bool detail coerced (F1 + mount synergy)");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_mount_works_without_query_params()
{
    std::cout << "  test_mount_works_without_query_params..." << std::endl;
    FastMCP child("weather_app", "1.0.0");
    child.resources().register_template(make_weather_template());

    FastMCP parent("main", "1.0.0");
    parent.mount(child, "forecast");

    auto content = parent.read_resource("weather://forecast/london/current");
    auto parsed = Json::parse(std::get<std::string>(content.data));
    ASSERT_EQ(parsed.value("city", std::string{}), std::string("london"), "city without query");
    ASSERT_TRUE(!parsed.contains("units"), "no units when omitted");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_mount_invalid_bool_query_param_raises()
{
    std::cout << "  test_mount_invalid_bool_query_param_raises..." << std::endl;
    // Demonstrates F1 + F6 together: a typed bool param coming through the
    // mount path triggers ValidationError, mirroring Python parity.
    FastMCP child("weather_app", "1.0.0");
    child.resources().register_template(make_weather_template());

    FastMCP parent("main", "1.0.0");
    parent.mount(child, "forecast");

    bool threw = false;
    try
    {
        (void)parent.read_resource("weather://forecast/paris/current?detail=banana");
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "invalid bool through mount raises");
    std::cout << "    PASS" << std::endl;
    return 0;
}

int main()
{
    std::cout << "Mount + Query Params Tests" << std::endl;
    std::cout << "==========================" << std::endl;
    int failures = 0;
    failures += test_mount_preserves_query_params();
    failures += test_mount_works_without_query_params();
    failures += test_mount_invalid_bool_query_param_raises();
    std::cout << std::endl;
    if (failures == 0)
    {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) FAILED" << std::endl;
    return 1;
}
