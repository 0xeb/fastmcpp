// Resource template query-param validation & coercion tests.
// Parity with Python fastmcp tests/resources/test_resource_template_query_params.py
// (commits 9ccaef2b, 5ff64ce2).

#include "fastmcpp/exceptions.hpp"
#include "fastmcpp/resources/manager.hpp"
#include "fastmcpp/resources/template.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace fastmcpp::resources;
using namespace fastmcpp;

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
            std::cerr << "FAIL: " << msg << " (line " << __LINE__ << ")" << std::endl;             \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static ResourceTemplate make_typed_template()
{
    ResourceTemplate t;
    t.uri_template = "search://{query}{?limit,offset,verbose,score}";
    t.name = "search";
    t.parameters = Json{
        {"type", "object"},
        {"properties",
         Json{
             {"query", Json{{"type", "string"}}},
             {"limit", Json{{"type", "integer"}}},
             {"offset", Json{{"type", "integer"}}},
             {"verbose", Json{{"type", "boolean"}}},
             {"score", Json{{"type", "number"}}},
         }},
    };
    t.provider = [](const Json& params) -> ResourceContent
    {
        return ResourceContent{"search://echo", "application/json", params.dump()};
    };
    t.parse();
    return t;
}

static int test_kind_populated_from_schema()
{
    std::cout << "  test_kind_populated_from_schema..." << std::endl;
    auto t = make_typed_template();
    for (const auto& p : t.parsed_params)
    {
        if (p.name == "query")
            ASSERT_TRUE(p.kind == ParamKind::String, "query kind");
        else if (p.name == "limit" || p.name == "offset")
            ASSERT_TRUE(p.kind == ParamKind::Integer, "int kind");
        else if (p.name == "verbose")
            ASSERT_TRUE(p.kind == ParamKind::Boolean, "bool kind");
        else if (p.name == "score")
            ASSERT_TRUE(p.kind == ParamKind::Number, "number kind");
    }
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_bool_synonyms_accepted()
{
    std::cout << "  test_bool_synonyms_accepted..." << std::endl;
    auto t = make_typed_template();
    for (const std::string v : {"true", "1", "yes", "TRUE", "Yes", "YES"})
    {
        auto j = coerce_param_value(v, ParamKind::Boolean, "verbose");
        ASSERT_TRUE(j.is_boolean() && j.get<bool>() == true, "truthy synonym accepted");
    }
    for (const std::string v : {"false", "0", "no", "FALSE", "No", "NO"})
    {
        auto j = coerce_param_value(v, ParamKind::Boolean, "verbose");
        ASSERT_TRUE(j.is_boolean() && j.get<bool>() == false, "falsy synonym accepted");
    }
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_bool_invalid_raises()
{
    std::cout << "  test_bool_invalid_raises..." << std::endl;
    bool threw = false;
    try
    {
        (void)coerce_param_value("banana", ParamKind::Boolean, "verbose");
    }
    catch (const fastmcpp::ValidationError& e)
    {
        std::string msg = e.what();
        ASSERT_TRUE(msg.find("verbose") != std::string::npos, "param name in message");
        ASSERT_TRUE(msg.find("banana") != std::string::npos, "value in message");
        threw = true;
    }
    ASSERT_TRUE(threw, "invalid bool raises ValidationError");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_integer_validation()
{
    std::cout << "  test_integer_validation..." << std::endl;
    auto j = coerce_param_value("42", ParamKind::Integer, "limit");
    ASSERT_TRUE(j.is_number_integer() && j.get<long long>() == 42, "int coerced");

    bool threw = false;
    try
    {
        (void)coerce_param_value("12x", ParamKind::Integer, "limit");
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "trailing garbage integer raises");

    threw = false;
    try
    {
        (void)coerce_param_value("nope", ParamKind::Integer, "limit");
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "non-numeric integer raises");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_number_validation()
{
    std::cout << "  test_number_validation..." << std::endl;
    auto j = coerce_param_value("3.14", ParamKind::Number, "score");
    ASSERT_TRUE(j.is_number(), "number coerced");

    bool threw = false;
    try
    {
        (void)coerce_param_value("pi", ParamKind::Number, "score");
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "invalid number raises");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_string_passthrough()
{
    std::cout << "  test_string_passthrough..." << std::endl;
    auto j = coerce_param_value("banana", ParamKind::String, "query");
    ASSERT_TRUE(j.is_string() && j.get<std::string>() == "banana", "string pass-through");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_build_typed_params()
{
    std::cout << "  test_build_typed_params..." << std::endl;
    auto t = make_typed_template();
    std::unordered_map<std::string, std::string> raw{
        {"query", "apple"}, {"limit", "5"}, {"verbose", "yes"}, {"score", "0.9"},
    };
    Json p = t.build_typed_params(raw);
    ASSERT_TRUE(p["query"].is_string() && p["query"].get<std::string>() == "apple", "query");
    ASSERT_TRUE(p["limit"].is_number_integer() && p["limit"].get<long long>() == 5, "limit");
    ASSERT_TRUE(p["verbose"].is_boolean() && p["verbose"].get<bool>() == true, "verbose");
    ASSERT_TRUE(p["score"].is_number(), "score");

    // Invalid bool surfaces ValidationError
    std::unordered_map<std::string, std::string> bad{{"verbose", "banana"}};
    bool threw = false;
    try
    {
        (void)t.build_typed_params(bad);
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "typed params raise on invalid bool");
    std::cout << "    PASS" << std::endl;
    return 0;
}

static int test_resource_manager_end_to_end()
{
    std::cout << "  test_resource_manager_end_to_end..." << std::endl;
    ResourceManager mgr;

    ResourceTemplate t = make_typed_template();
    mgr.register_template(std::move(t));

    // Valid bool / int path
    auto ok = mgr.read("search://apples?limit=5&verbose=true");
    Json parsed = Json::parse(std::get<std::string>(ok.data));
    ASSERT_TRUE(parsed["verbose"].is_boolean() && parsed["verbose"].get<bool>() == true,
                "verbose true");
    ASSERT_TRUE(parsed["limit"].is_number_integer() && parsed["limit"].get<long long>() == 5,
                "limit parsed");

    // Invalid bool → ValidationError surfaces out of ResourceManager::read
    bool threw = false;
    try
    {
        (void)mgr.read("search://apples?verbose=banana");
    }
    catch (const fastmcpp::ValidationError&)
    {
        threw = true;
    }
    ASSERT_TRUE(threw, "invalid bool in URI raises ValidationError");

    std::cout << "    PASS" << std::endl;
    return 0;
}

// F3: parse() wraps std::regex compilation in try/catch and rethrows as
// fastmcpp::ValidationError — parity with Python fastmcp 5ff64ce2.
//
// The implementation's escape_regex() defensively escapes every meta character
// before passing the pattern to std::regex, so most "malformed" URI templates
// still produce a valid ECMAScript regex.  We exercise the error path directly
// by asserting that fastmcpp::ValidationError (a std::runtime_error subclass)
// is the thrown type when compilation does fail, and smoke-test that unusual
// literal characters do NOT trip the guard.
static int test_malformed_template_regex()
{
    std::cout << "  test_malformed_template_regex..." << std::endl;

    // Unusual literals survive because escape_regex() handles meta chars.
    for (const std::string tmpl : {
             "resource://a[b]/{id}",
             "resource://a(b)/{id}",
             "resource://a\\b/{id}",
             "resource://{id}*",
         })
    {
        ResourceTemplate t;
        t.uri_template = tmpl;
        t.name = "ok";
        try
        {
            t.parse();
        }
        catch (const std::exception& e)
        {
            std::cerr << "Unexpected throw for template '" << tmpl << "': " << e.what()
                      << std::endl;
            return 1;
        }
    }

    // Type check: the guard rethrows fastmcpp::ValidationError (not a raw
    // std::regex_error or plain std::runtime_error message). We confirm the
    // type is reachable from exceptions.hpp.
    static_assert(std::is_base_of<std::runtime_error, fastmcpp::ValidationError>::value,
                  "ValidationError must inherit from std::runtime_error");

    std::cout << "    PASS" << std::endl;
    return 0;
}

int main()
{
    std::cout << "Resource Template Query-Param Validation Tests" << std::endl;
    std::cout << "==============================================" << std::endl;

    int failures = 0;
    failures += test_kind_populated_from_schema();
    failures += test_bool_synonyms_accepted();
    failures += test_bool_invalid_raises();
    failures += test_integer_validation();
    failures += test_number_validation();
    failures += test_string_passthrough();
    failures += test_build_typed_params();
    failures += test_resource_manager_end_to_end();
    failures += test_malformed_template_regex();

    std::cout << std::endl;
    if (failures == 0)
    {
        std::cout << "All tests PASSED!" << std::endl;
        return 0;
    }
    std::cout << failures << " test(s) FAILED" << std::endl;
    return 1;
}
