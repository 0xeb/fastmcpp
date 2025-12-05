// Resource Templates unit tests
// Tests for RFC 6570 URI template support

#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/resources/template.hpp"

#include <cassert>
#include <iostream>

using namespace fastmcpp::resources;
using namespace fastmcpp;

// Test helper: assert with message
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
        if ((a) != (b))                                                                            \
        {                                                                                          \
            std::cerr << "FAIL: " << msg << " - expected '" << (b) << "' but got '" << (a) << "'"  \
                      << " (line " << __LINE__ << ")" << std::endl;                                \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

// Test URL encoding/decoding
int test_url_encoding()
{
    std::cout << "  test_url_encoding..." << std::endl;

    // Basic encoding
    ASSERT_EQ(url_encode("hello world"), "hello%20world", "Space encoding");
    ASSERT_EQ(url_encode("foo+bar"), "foo%2Bbar", "Plus encoding");
    ASSERT_EQ(url_encode("a/b/c"), "a%2Fb%2Fc", "Slash encoding");
    ASSERT_EQ(url_encode("test@example.com"), "test%40example.com", "At sign encoding");

    // Characters that should NOT be encoded
    ASSERT_EQ(url_encode("hello-world"), "hello-world", "Hyphen not encoded");
    ASSERT_EQ(url_encode("hello_world"), "hello_world", "Underscore not encoded");
    ASSERT_EQ(url_encode("hello.world"), "hello.world", "Dot not encoded");
    ASSERT_EQ(url_encode("hello~world"), "hello~world", "Tilde not encoded");

    // Basic decoding
    ASSERT_EQ(url_decode("hello%20world"), "hello world", "Space decoding");
    ASSERT_EQ(url_decode("foo%2Bbar"), "foo+bar", "Plus decoding");
    ASSERT_EQ(url_decode("test%40example.com"), "test@example.com", "At sign decoding");

    // Plus as space
    ASSERT_EQ(url_decode("hello+world"), "hello world", "Plus to space decoding");

    // Roundtrip
    std::string original = "hello world! @#$%";
    ASSERT_EQ(url_decode(url_encode(original)), original, "Roundtrip encoding/decoding");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test path parameter extraction
int test_extract_path_params()
{
    std::cout << "  test_extract_path_params..." << std::endl;

    auto params = extract_path_params("weather://{city}/current");
    ASSERT_EQ(params.size(), 1u, "One path param");
    ASSERT_EQ(params[0], "city", "Param name is city");

    params = extract_path_params("file://{path*}");
    ASSERT_EQ(params.size(), 1u, "One wildcard param");
    ASSERT_EQ(params[0], "path", "Wildcard param name");

    params = extract_path_params("api://{version}/{resource}/{id}");
    ASSERT_EQ(params.size(), 3u, "Three path params");
    ASSERT_EQ(params[0], "version", "First param");
    ASSERT_EQ(params[1], "resource", "Second param");
    ASSERT_EQ(params[2], "id", "Third param");

    // Should not extract query params
    params = extract_path_params("search://{query}{?limit,offset}");
    ASSERT_EQ(params.size(), 1u, "Only path param, not query");
    ASSERT_EQ(params[0], "query", "Path param name");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test query parameter extraction
int test_extract_query_params()
{
    std::cout << "  test_extract_query_params..." << std::endl;

    auto params = extract_query_params("search://{query}{?limit,offset}");
    ASSERT_EQ(params.size(), 2u, "Two query params");
    ASSERT_EQ(params[0], "limit", "First query param");
    ASSERT_EQ(params[1], "offset", "Second query param");

    params = extract_query_params("api://{resource}{?fields}");
    ASSERT_EQ(params.size(), 1u, "One query param");
    ASSERT_EQ(params[0], "fields", "Query param name");

    // No query params
    params = extract_query_params("simple://{id}");
    ASSERT_TRUE(params.empty(), "No query params");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test template parsing
int test_template_parse()
{
    std::cout << "  test_template_parse..." << std::endl;

    ResourceTemplate templ;
    templ.uri_template = "weather://{city}/forecast/{date}";
    templ.name = "Weather Forecast";
    templ.parse();

    ASSERT_EQ(templ.parsed_params.size(), 2u, "Two params parsed");
    ASSERT_EQ(templ.parsed_params[0].name, "city", "First param name");
    ASSERT_TRUE(!templ.parsed_params[0].is_wildcard, "Not wildcard");
    ASSERT_TRUE(!templ.parsed_params[0].is_query, "Not query");
    ASSERT_EQ(templ.parsed_params[1].name, "date", "Second param name");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test URI matching
int test_template_match()
{
    std::cout << "  test_template_match..." << std::endl;

    ResourceTemplate templ;
    templ.uri_template = "weather://{city}/current";
    templ.name = "Current Weather";
    templ.parse();

    // Should match
    auto match = templ.match("weather://london/current");
    ASSERT_TRUE(match.has_value(), "Should match london");
    ASSERT_EQ(match->at("city"), "london", "City is london");

    match = templ.match("weather://new-york/current");
    ASSERT_TRUE(match.has_value(), "Should match new-york");
    ASSERT_EQ(match->at("city"), "new-york", "City is new-york");

    // Should not match
    match = templ.match("weather://london/forecast");
    ASSERT_TRUE(!match.has_value(), "Should not match /forecast");

    match = templ.match("temperature://london/current");
    ASSERT_TRUE(!match.has_value(), "Should not match different scheme");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test multi-parameter matching
int test_multi_param_match()
{
    std::cout << "  test_multi_param_match..." << std::endl;

    ResourceTemplate templ;
    templ.uri_template = "api://{version}/{resource}/{id}";
    templ.name = "API Resource";
    templ.parse();

    auto match = templ.match("api://v1/users/123");
    ASSERT_TRUE(match.has_value(), "Should match");
    ASSERT_EQ(match->at("version"), "v1", "Version is v1");
    ASSERT_EQ(match->at("resource"), "users", "Resource is users");
    ASSERT_EQ(match->at("id"), "123", "ID is 123");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test URL-encoded parameter matching
int test_encoded_param_match()
{
    std::cout << "  test_encoded_param_match..." << std::endl;

    ResourceTemplate templ;
    templ.uri_template = "search://{query}";
    templ.name = "Search";
    templ.parse();

    // URL-encoded query
    auto match = templ.match("search://hello%20world");
    ASSERT_TRUE(match.has_value(), "Should match encoded URI");
    ASSERT_EQ(match->at("query"), "hello world", "Query is decoded");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test wildcard parameter matching
int test_wildcard_match()
{
    std::cout << "  test_wildcard_match..." << std::endl;

    ResourceTemplate templ;
    templ.uri_template = "file://{path*}";
    templ.name = "File";
    templ.parse();

    ASSERT_TRUE(templ.parsed_params[0].is_wildcard, "Should be wildcard");

    auto match = templ.match("file://a/b/c/d.txt");
    ASSERT_TRUE(match.has_value(), "Should match path with slashes");
    ASSERT_EQ(match->at("path"), "a/b/c/d.txt", "Path includes slashes");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test ResourceManager with templates
int test_resource_manager_templates()
{
    std::cout << "  test_resource_manager_templates..." << std::endl;

    ResourceManager mgr;

    // Register a template
    ResourceTemplate templ;
    templ.uri_template = "weather://{city}/current";
    templ.name = "Current Weather";
    templ.description = "Get current weather for a city";
    templ.mime_type = "application/json";
    templ.provider = [](const Json& params) -> ResourceContent
    {
        std::string city = params.value("city", "unknown");
        Json data = {{"city", city}, {"temperature", 20}, {"conditions", "sunny"}};
        return ResourceContent{"weather://" + city + "/current", "application/json", data.dump()};
    };

    mgr.register_template(std::move(templ));

    // List templates
    auto templates = mgr.list_templates();
    ASSERT_EQ(templates.size(), 1u, "One template registered");
    ASSERT_EQ(templates[0].name, "Current Weather", "Template name");

    // Read via template match
    auto content = mgr.read("weather://paris/current");
    ASSERT_EQ(content.uri, "weather://paris/current", "Content URI");
    ASSERT_TRUE(content.mime_type.has_value(), "Has mime type");
    ASSERT_EQ(*content.mime_type, "application/json", "Mime type");

    // Parse the returned content
    auto json_content = Json::parse(std::get<std::string>(content.data));
    ASSERT_EQ(json_content["city"], "paris", "City in content");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// Test query parameter matching
int test_query_param_match()
{
    std::cout << "  test_query_param_match..." << std::endl;

    ResourceTemplate templ;
    templ.uri_template = "search://{query}{?limit,offset}";
    templ.name = "Search";
    templ.parse();

    ASSERT_EQ(templ.parsed_params.size(), 3u, "Three params total");

    // Match with query params
    auto match = templ.match("search://test?limit=10&offset=20");
    ASSERT_TRUE(match.has_value(), "Should match with query params");
    ASSERT_EQ(match->at("query"), "test", "Query param");
    ASSERT_EQ(match->at("limit"), "10", "Limit param");
    ASSERT_EQ(match->at("offset"), "20", "Offset param");

    // Match without query params
    match = templ.match("search://test");
    ASSERT_TRUE(match.has_value(), "Should match without query params");
    ASSERT_EQ(match->at("query"), "test", "Query param without query string");

    std::cout << "    PASS" << std::endl;
    return 0;
}

int main()
{
    std::cout << "Resource Templates Tests" << std::endl;
    std::cout << "========================" << std::endl;

    int failures = 0;

    failures += test_url_encoding();
    failures += test_extract_path_params();
    failures += test_extract_query_params();
    failures += test_template_parse();
    failures += test_template_match();
    failures += test_multi_param_match();
    failures += test_encoded_param_match();
    failures += test_wildcard_match();
    failures += test_resource_manager_templates();
    failures += test_query_param_match();

    std::cout << std::endl;
    if (failures == 0)
    {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    else
    {
        std::cout << failures << " test(s) FAILED" << std::endl;
        return 1;
    }
}
